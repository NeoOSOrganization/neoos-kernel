// sqlitetest -- SQLite on NeoOS (desktop M1): the port, nsql, and the
// kernel's record locks under a real database. `make sqlitetest`.
//
//   sqlitetest           phase 1: migrations, locking, concurrency,
//                        crash recovery; leaves a marker row behind
//   sqlitetest phase2    after a reboot: the marker row survived
//
// Prints "sqlitetest: ok <name>" / "... FAIL ..." and "PASS sqlitetest".
// Processes synchronise through pipes -- never a sleep.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "nsql.h"

static int fails;
#define CHECK(name, cond, ...) do { if (cond) printf("sqlitetest: ok %s\n", name); \
    else { fails++; printf("sqlitetest: FAIL %s: ", name); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DB "/root/sq/app.db"

static const struct nsql_migration migs[] = {
    { 1, "CREATE TABLE kv(k TEXT PRIMARY KEY, v TEXT NOT NULL);" },
    { 2, "CREATE TABLE log(id INTEGER PRIMARY KEY, who INTEGER, n INTEGER);" },
};

static int count(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = 0;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

static int text_eq(sqlite3 *db, const char *sql, const char *want) {
    sqlite3_stmt *st = 0;
    int ok = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        ok = t && !strcmp(t, want);
    }
    sqlite3_finalize(st);
    return ok;
}

static sqlite3 *open_db(void) {
    sqlite3 *db = 0;
    return nsql_open_path(DB, migs, 2, &db) == SQLITE_OK ? db : 0;
}

static void say(int fd, char c) { write(fd, &c, 1); }
static char hear(int fd) { char c = 0; read(fd, &c, 1); return c; }

static int phase2(void) {
    sqlite3 *db = open_db();
    CHECK("persisted_across_boot", db && text_eq(db, "SELECT v FROM kv WHERE k='marker'", "phase1"),
          "marker row missing after reboot");
    CHECK("integrity_after_boot", db && text_eq(db, "PRAGMA integrity_check", "ok"), "integrity_check");
    sqlite3_close(db);
    printf(fails ? "sqlitetest: %d FAILED\n" : "PASS sqlitetest\n", fails);
    return fails != 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "phase2")) { return phase2(); }
    mkdir("/root", 0755);
    mkdir("/root/sq", 0755);
    unlink(DB);
    unlink(DB "-journal");

    // 1. Create through nsql: migrations, pragmas, a transaction.
    sqlite3 *db = open_db();
    int ok = db && count(db, "PRAGMA user_version") == 2 &&
             sqlite3_exec(db, "BEGIN; INSERT INTO kv VALUES('a','1'); INSERT INTO kv VALUES('b','2'); COMMIT;", 0, 0, 0) == SQLITE_OK &&
             count(db, "SELECT count(*) FROM kv") == 2;
    CHECK("create_migrate_commit", ok, "db=%p", (void *)db);

    // 2. A writer holding EXCLUSIVE locks another process out (fcntl
    //    record locks underneath); once it commits, the other gets in.
    int go[2], back[2];
    pipe(go); pipe(back);
    pid_t c = fork();
    if (c == 0) {
        sqlite3 *d = open_db();
        sqlite3_exec(d, "BEGIN EXCLUSIVE; INSERT INTO kv VALUES('c','3');", 0, 0, 0);
        say(back[1], 'l');                       // holding the lock
        hear(go[0]);
        int rc = sqlite3_exec(d, "COMMIT", 0, 0, 0);
        sqlite3_close(d);
        say(back[1], rc == SQLITE_OK ? 'c' : 'x');
        _exit(0);
    }
    hear(back[0]);
    sqlite3_busy_timeout(db, 0);                 // fail at once rather than wait
    int busy = sqlite3_exec(db, "INSERT INTO kv VALUES('d','4')", 0, 0, 0);
    say(go[1], 'g');
    char committed = hear(back[0]);
    waitpid(c, 0, 0);
    sqlite3_busy_timeout(db, 2000);
    int after = sqlite3_exec(db, "INSERT INTO kv VALUES('d','4')", 0, 0, 0);
    CHECK("exclusive_lock_excludes", busy == SQLITE_BUSY && committed == 'c' && after == SQLITE_OK &&
          count(db, "SELECT count(*) FROM kv") == 4, "busy=%d committed=%c after=%d", busy, committed, after);

    // 3. Two processes inserting concurrently, each its own transactions:
    //    nothing lost, nothing corrupt.
    #define PER 150
    pid_t w[2];
    for (int k = 0; k < 2; k++) {
        w[k] = fork();
        if (w[k] == 0) {
            sqlite3 *d = open_db();
            sqlite3_busy_timeout(d, 30000);
            char sql[96];
            int bad = 0;
            for (int i = 0; i < PER; i++) {
                snprintf(sql, sizeof sql, "INSERT INTO log(who, n) VALUES(%d, %d)", k, i);
                if (sqlite3_exec(d, sql, 0, 0, 0) != SQLITE_OK) { bad++; }
            }
            sqlite3_close(d);
            _exit(bad ? 1 : 0);
        }
    }
    int st0 = 0, st1 = 0;
    waitpid(w[0], &st0, 0);
    waitpid(w[1], &st1, 0);
    CHECK("concurrent_writers", WEXITSTATUS(st0) == 0 && WEXITSTATUS(st1) == 0 &&
          count(db, "SELECT count(*) FROM log") == 2 * PER && text_eq(db, "PRAGMA integrity_check", "ok"),
          "status %d/%d rows=%d", WEXITSTATUS(st0), WEXITSTATUS(st1), count(db, "SELECT count(*) FROM log"));

    // 4. A writer killed mid-transaction leaves a hot journal; the next
    //    open rolls it back.
    pipe(back);
    c = fork();
    if (c == 0) {
        sqlite3 *d = open_db();
        sqlite3_exec(d, "BEGIN; UPDATE kv SET v='torn' WHERE k='a'; DELETE FROM log;", 0, 0, 0);
        // Force the change out of the page cache into the file, so the
        // journal is genuinely needed to undo it.
        sqlite3_db_cacheflush(d);
        say(back[1], 'd');
        for (;;) { pause(); }
    }
    hear(back[0]);
    kill(c, SIGKILL);
    waitpid(c, 0, 0);
    struct stat js;
    int hot = stat(DB "-journal", &js) == 0;
    sqlite3_close(db);
    db = open_db();
    CHECK("crash_rollback", hot && db && text_eq(db, "SELECT v FROM kv WHERE k='a'", "1") &&
          count(db, "SELECT count(*) FROM log") == 2 * PER && text_eq(db, "PRAGMA integrity_check", "ok"),
          "hot_journal=%d a=%s", hot, db && text_eq(db, "SELECT v FROM kv WHERE k='a'", "torn") ? "torn" : "?");

    // Left for phase 2, after a reboot.
    sqlite3_exec(db, "INSERT OR REPLACE INTO kv VALUES('marker','phase1')", 0, 0, 0);
    sqlite3_close(db);
    printf(fails ? "sqlitetest: %d FAILED\n" : "PASS sqlitetest phase1\n", fails);
    return fails != 0;
}
