// m0test -- userland checks for the desktop M0 kernel prerequisites
// (docs/superpowers/plans/2026-09-23-desktop-m0-kernel-prereqs.md).
// Prints "m0test: ok <name>" / "m0test: FAIL <name> ..." per check and
// "PASS m0test" only if every check passed. Works on the FAT disk
// (/root/m0), which is where SQLite and the desktop keep their files.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>

static int fails;
#define CHECK(name, cond, ...) do { if (cond) printf("m0test: ok %s\n", name); \
    else { fails++; printf("m0test: FAIL %s: ", name); printf(__VA_ARGS__); printf(" (errno=%d)\n", errno); } } while (0)

#define DIR0 "/root/m0"

static void test_excl(void) {
    unlink(DIR0 "/excl");
    int a = open(DIR0 "/excl", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK("excl_create", a >= 0, "first O_EXCL create failed");
    errno = 0;
    int b = open(DIR0 "/excl", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK("excl_eexist", b < 0 && errno == EEXIST, "second O_EXCL create: fd=%d", b);
    int c = open(DIR0 "/excl", O_CREAT | O_RDWR, 0644);
    CHECK("creat_existing", c >= 0, "plain O_CREAT of an existing file failed");
    if (a >= 0) { close(a); }
    if (c >= 0) { close(c); }
}

static void test_pwrite(void) {
    int fd = open(DIR0 "/pw", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) { CHECK("pwrite_open", 0, "open"); return; }
    write(fd, "0123456789", 10);
    lseek(fd, 2, SEEK_SET);
    ssize_t n = pwrite(fd, "AB", 2, 5);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    char buf[16] = {0};
    pread(fd, buf, 10, 0);
    CHECK("pwrite", n == 2 && pos == 2 && memcmp(buf, "01234AB789", 10) == 0,
          "n=%zd pos=%ld buf=%.10s", n, (long)pos, buf);
    close(fd);
}

static int write_file(const char *p, const char *text) {
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { return -1; }
    int n = (int)write(fd, text, strlen(text));
    close(fd);
    return n;
}
static int read_file(const char *p, char *buf, int cap) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) { return -1; }
    int n = (int)read(fd, buf, cap - 1);
    close(fd);
    buf[n < 0 ? 0 : n] = 0;
    return n;
}

static void test_rename(void) {
    char buf[64];
    mkdir(DIR0 "/ra", 0755); mkdir(DIR0 "/rb", 0755);
    unlink(DIR0 "/ra/one"); unlink(DIR0 "/ra/two"); unlink(DIR0 "/rb/three");

    write_file(DIR0 "/ra/one", "first");
    int rc = rename(DIR0 "/ra/one", DIR0 "/ra/two");
    CHECK("rename_same_dir", rc == 0 && access(DIR0 "/ra/one", F_OK) != 0 &&
          read_file(DIR0 "/ra/two", buf, sizeof buf) == 5 && !strcmp(buf, "first"), "rc=%d buf=%s", rc, buf);

    rc = rename(DIR0 "/ra/two", DIR0 "/rb/three");
    CHECK("rename_cross_dir", rc == 0 && read_file(DIR0 "/rb/three", buf, sizeof buf) == 5 &&
          access(DIR0 "/ra/two", F_OK) != 0, "rc=%d", rc);

    write_file(DIR0 "/ra/victim", "old contents");
    rc = rename(DIR0 "/rb/three", DIR0 "/ra/victim");
    CHECK("rename_replaces", rc == 0 && read_file(DIR0 "/ra/victim", buf, sizeof buf) == 5 &&
          !strcmp(buf, "first"), "rc=%d buf=%s", rc, buf);

    // An fd open across the rename keeps writing to the (moved) file.
    int fd = open(DIR0 "/ra/victim", O_WRONLY | O_APPEND);
    rc = rename(DIR0 "/ra/victim", DIR0 "/rb/moved");
    write(fd, "+more", 5);
    close(fd);
    CHECK("rename_open_fd", rc == 0 && read_file(DIR0 "/rb/moved", buf, sizeof buf) == 10 &&
          !strcmp(buf, "first+more"), "rc=%d buf=%s", rc, buf);

    // A directory moved to a new parent: its ".." follows.
    mkdir(DIR0 "/ra/sub", 0755);
    write_file(DIR0 "/ra/sub/f", "x");
    rc = rename(DIR0 "/ra/sub", DIR0 "/rb/sub");
    struct stat a, b;
    int ok = rc == 0 && stat(DIR0 "/rb/sub/f", &a) == 0 && stat(DIR0 "/rb/sub/..", &a) == 0 &&
             stat(DIR0 "/rb", &b) == 0;
    CHECK("rename_dir", ok, "rc=%d", rc);

    errno = 0;
    CHECK("rename_into_self", rename(DIR0 "/rb", DIR0 "/rb/sub/x") == -1 && errno == EINVAL, "no EINVAL");
    errno = 0;
    CHECK("rename_exdev", rename(DIR0 "/rb/moved", "/proc/moved") == -1 && errno == EXDEV, "no EXDEV");
    errno = 0;
    CHECK("rename_enoent", rename(DIR0 "/nope", DIR0 "/nope2") == -1 && errno == ENOENT, "no ENOENT");
}

// Persian, with a ZWNJ (U+200C) the way Persian is actually written.
#define FA_NAME "\xd9\x81\xd8\xa7\xdb\x8c\xd9\x84\xe2\x80\x8c\xd9\x87\xd8\xa7.txt"   /* فایل‌ها.txt */

static void test_utf8_names(void) {
    char path[256], buf[32];
    snprintf(path, sizeof path, DIR0 "/%s", FA_NAME);
    unlink(path);
    int w = write_file(path, "salam");
    int found = 0;
    DIR *d = opendir(DIR0);
    struct dirent *e;
    while (d && (e = readdir(d))) { if (!strcmp(e->d_name, FA_NAME)) { found = 1; } }
    if (d) { closedir(d); }
    CHECK("utf8_name", w == 5 && found && read_file(path, buf, sizeof buf) == 5, "w=%d found=%d", w, found);
    errno = 0;
    int bad = open(DIR0 "/bad\xff\xfe.txt", O_CREAT | O_WRONLY, 0644);
    CHECK("utf8_invalid", bad < 0 && errno == EINVAL, "fd=%d", bad);
    if (bad >= 0) { close(bad); }
}

// ---- POSIX record locks -------------------------------------------------

#define LK DIR0 "/lockfile"

static int lk(int fd, int cmd, int type, off_t start, off_t len) {
    struct flock fl = { .l_type = (short)type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, cmd, &fl);
}
static int getlk(int fd, int type, off_t start, off_t len, struct flock *out) {
    *out = (struct flock){ .l_type = (short)type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return fcntl(fd, F_GETLK, out);
}
static int child_status(pid_t pid) {
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100;
}
static void say(int fd, char c) { write(fd, &c, 1); }
static char hear(int fd) { char c = 0; read(fd, &c, 1); return c; }

static void test_locks(void) {
    int fd = open(LK, O_CREAT | O_RDWR, 0644);
    if (fd < 0) { CHECK("lock_open", 0, "open"); return; }
    pid_t me = getpid();

    // 1. A write lock excludes; F_GETLK names the holder and its range.
    lk(fd, F_SETLK, F_WRLCK, 0, 100);
    pid_t c = fork();
    if (c == 0) {
        int f = open(LK, O_RDWR);
        struct flock g;
        int bad = 0;
        if (!(lk(f, F_SETLK, F_WRLCK, 50, 10) == -1 && errno == EAGAIN)) { bad |= 1; }
        if (getlk(f, F_WRLCK, 50, 10, &g) != 0 || g.l_type != F_WRLCK || g.l_pid != me ||
            g.l_start != 0 || g.l_len != 100) { bad |= 2; }
        if (lk(f, F_SETLK, F_RDLCK, 100, 100) != 0) { bad |= 4; }       // just past it: free
        _exit(bad);
    }
    int st = child_status(c);
    CHECK("lock_conflict", st == 0, "child status %d", st);

    // 2. Read locks share; a write lock against a read lock fails.
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    lk(fd, F_SETLK, F_RDLCK, 0, 10);
    c = fork();
    if (c == 0) {
        int f = open(LK, O_RDWR);
        int bad = 0;
        if (lk(f, F_SETLK, F_RDLCK, 0, 10) != 0) { bad |= 1; }
        if (!(lk(f, F_SETLK, F_WRLCK, 5, 1) == -1 && errno == EAGAIN)) { bad |= 2; }
        _exit(bad);
    }
    st = child_status(c);
    CHECK("lock_shared", st == 0, "child status %d", st);

    // 3. F_SETLKW sleeps until the holder lets go.
    lk(fd, F_SETLK, F_WRLCK, 0, 1);
    int p2c[2], c2p[2];
    pipe(p2c); pipe(c2p);
    c = fork();
    if (c == 0) {
        int f = open(LK, O_RDWR);
        say(c2p[1], 'b');                                  // about to block
        int rc = lk(f, F_SETLKW, F_WRLCK, 0, 1);
        say(c2p[1], rc == 0 ? 'g' : 'x');
        _exit(rc == 0 ? 0 : 1);
    }
    hear(c2p[0]);
    lk(fd, F_SETLK, F_UNLCK, 0, 1);
    char got = hear(c2p[0]);
    st = child_status(c);
    CHECK("lock_setlkw_wakes", got == 'g' && st == 0, "got=%c status %d", got, st);

    // 4. Closing ANY fd for the file drops the process's locks on it.
    int fd2 = open(LK, O_RDWR);
    lk(fd, F_SETLK, F_WRLCK, 0, 0);
    close(fd2);
    c = fork();
    if (c == 0) { int f = open(LK, O_RDWR); _exit(lk(f, F_SETLK, F_WRLCK, 0, 0) == 0 ? 0 : 1); }
    st = child_status(c);
    CHECK("lock_close_releases_all", st == 0, "child status %d", st);
    close(fd);
    fd = open(LK, O_RDWR);

    // 5. Exit releases.
    c = fork();
    if (c == 0) { int f = open(LK, O_RDWR); lk(f, F_SETLK, F_WRLCK, 0, 0); _exit(0); }
    child_status(c);
    CHECK("lock_exit_releases", lk(fd, F_SETLK, F_WRLCK, 0, 0) == 0, "lock after child exit");

    // 6. Unlocking the middle splits the range.
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    lk(fd, F_SETLK, F_WRLCK, 0, 100);
    lk(fd, F_SETLK, F_UNLCK, 40, 20);                     // hole [40, 59]
    c = fork();
    if (c == 0) {
        int f = open(LK, O_RDWR);
        struct flock g;
        int bad = 0;
        getlk(f, F_WRLCK, 45, 1, &g); if (g.l_type != F_UNLCK) { bad |= 1; }
        getlk(f, F_WRLCK, 30, 1, &g); if (g.l_type != F_WRLCK || g.l_start != 0 || g.l_len != 40) { bad |= 2; }
        getlk(f, F_WRLCK, 70, 1, &g); if (g.l_type != F_WRLCK || g.l_start != 60 || g.l_len != 40) { bad |= 4; }
        _exit(bad);
    }
    st = child_status(c);
    CHECK("lock_split", st == 0, "child status %d", st);
    lk(fd, F_SETLK, F_UNLCK, 0, 0);

    // 7. Two processes each waiting for the other: one gets EDEADLK.
    lk(fd, F_SETLK, F_WRLCK, 0, 1);
    int go[2], res[2];
    pipe(go); pipe(res);
    c = fork();
    if (c == 0) {
        int f = open(LK, O_RDWR);
        lk(f, F_SETLK, F_WRLCK, 1, 1);
        say(res[1], 'r');                                  // holding byte 1
        hear(go[0]);
        int rc = lk(f, F_SETLKW, F_WRLCK, 0, 1);
        int dl = rc == -1 && errno == EDEADLK;
        if (dl) { lk(f, F_SETLK, F_UNLCK, 1, 1); }
        _exit(dl ? 7 : (rc == 0 ? 0 : 1));
    }
    hear(res[0]);
    say(go[1], 'g');
    int rc = lk(fd, F_SETLKW, F_WRLCK, 1, 1);
    int parent_dl = rc == -1 && errno == EDEADLK;
    if (parent_dl) { lk(fd, F_SETLK, F_UNLCK, 0, 1); }
    st = child_status(c);
    int child_dl = st == 7;
    CHECK("lock_edeadlk", parent_dl + child_dl == 1, "parent_dl=%d child status %d", parent_dl, st);
    close(fd);
}

// Leaves nothing behind from an earlier run: the disk image outlives
// the boot, and a rename onto a directory the last run left non-empty
// is (correctly) ENOTEMPTY.
static void rm_rf(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { return; }
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
    // Deleting while listing can skip entries (the listing is by index),
    // so sweep until a pass removes nothing -- bounded, so a name that
    // cannot be removed ends the test instead of hanging it.
    for (int pass = 0; pass < 8; pass++) {
        DIR *d = opendir(path);
        if (!d) { break; }
        int removed = 0;
        struct dirent *e;
        char sub[512];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { continue; }
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            rm_rf(sub);
            removed++;
        }
        closedir(d);
        if (!removed) { break; }
    }
    rmdir(path);
}

static void test_rmdir(void) {
    mkdir(DIR0 "/rd", 0755);
    write_file(DIR0 "/rd/f", "x");
    errno = 0;
    int ne = rmdir(DIR0 "/rd") == -1 && errno == ENOTEMPTY;
    unlink(DIR0 "/rd/f");
    int ok = rmdir(DIR0 "/rd") == 0 && access(DIR0 "/rd", F_OK) != 0;
    errno = 0;
    int nd = rmdir(DIR0 "/pw") == -1 && errno == ENOTDIR;
    errno = 0;
    int busy = rmdir("/proc") == -1 && errno == EBUSY;
    errno = 0;
    DIR *notdir = opendir(DIR0 "/pw");
    CHECK("opendir_file_enotdir", !notdir && errno == ENOTDIR, "opendir on a file: %p", (void *)notdir);
    if (notdir) { closedir(notdir); }
    CHECK("rmdir", ne && ok && nd && busy, "notempty=%d ok=%d notdir=%d busy=%d", ne, ok, nd, busy);
}

int main(void) {
    mkdir("/root", 0755);
    rm_rf(DIR0);
    mkdir(DIR0, 0755);
    test_excl();
    test_pwrite();
    test_rename();
    test_utf8_names();
    test_locks();
    test_rmdir();
    if (fails == 0) { printf("PASS m0test\n"); } else { printf("m0test: %d FAILED\n", fails); }
    return fails ? 1 : 0;
}
