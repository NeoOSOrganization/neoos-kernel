// /sbin/login.nex -- authenticate, become the user, run their shell.
//
// N5. init spawns this on the terminal as `god`; it verifies a password
// against /etc/passwd, drops to the account's uid/gid, and execs the
// shell. init respawns it, so leaving the shell returns a fresh prompt
// rather than powering the machine off.
//
// Built against musl, not libneoos, for ONE reason: crypt(). musl ships
// crypt() with SHA-256-crypt, SHA-512-crypt, MD5 and bcrypt back-ends,
// all pure computation with no files and no randomness -- so NeoOS gets
// real password hashing with nothing ported and nothing hand-rolled.
// Writing SHA-256 here would have been strictly worse: more code, and a
// bare hash rather than the rounds-based KDF $6$ actually is.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>

#define PASSWD_PATH "/etc/passwd"
#define LINE_MAX_   512

struct account {
    char name[64];
    int  uid, gid;
    char home[128];
    char shell[128];
    char hash[256];
};

// name:uid:gid:home:shell:hash
//
// Linux's field ORDER for the first five, with the hash in the same file
// rather than a separate /etc/shadow. There is no privilege boundary yet
// that a second file would enforce -- everything runs as god until this
// program drops -- so splitting them would be security theatre.
static int parse_line(char *line, struct account *a) {
    char *f[6];
    int n = 0;
    char *p = line;
    f[n++] = p;
    while (*p && n < 6) {
        if (*p == ':') { *p = 0; f[n++] = p + 1; }
        p++;
    }
    if (n != 6) { return 0; }

    snprintf(a->name,  sizeof a->name,  "%s", f[0]);
    a->uid = atoi(f[1]);
    a->gid = atoi(f[2]);
    snprintf(a->home,  sizeof a->home,  "%s", f[3]);
    snprintf(a->shell, sizeof a->shell, "%s", f[4]);
    snprintf(a->hash,  sizeof a->hash,  "%s", f[5]);
    return a->name[0] != 0;
}

static int find_account(const char *name, struct account *out) {
    FILE *f = fopen(PASSWD_PATH, "r");
    if (!f) { return 0; }
    char line[LINE_MAX_];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) { line[--n] = 0; }
        if (!line[0] || line[0] == '#') { continue; }
        struct account a;
        if (!parse_line(line, &a)) { continue; }
        if (strcmp(a.name, name) == 0) { *out = a; found = 1; break; }
    }
    fclose(f);
    return found;
}

// Reads a line with echo OFF. Returns 0 on success.
//
// The echo state is VERIFIED rather than assumed: if the terminal will
// not turn it off, this refuses to prompt instead of printing the
// password on the screen. A login that silently echoes is worse than one
// that will not run.
static int read_secret(char *out, size_t cap) {
    struct termios old, raw;
    if (tcgetattr(0, &old) != 0) { return -1; }
    raw = old;
    raw.c_lflag &= (unsigned)~ECHO;
    if (tcsetattr(0, TCSANOW, &raw) != 0) { return -1; }

    struct termios check;
    if (tcgetattr(0, &check) != 0 || (check.c_lflag & ECHO)) {
        tcsetattr(0, TCSANOW, &old);
        return -1;                       // echo still on: do not prompt
    }

    int ok = fgets(out, (int)cap, stdin) ? 0 : -1;
    tcsetattr(0, TCSANOW, &old);
    fputs("\n", stdout);                 // the Enter the user pressed was not echoed
    fflush(stdout);
    if (ok == 0) {
        size_t n = strlen(out);
        while (n && (out[n-1] == '\n' || out[n-1] == '\r')) { out[--n] = 0; }
    }
    return ok;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) { s[--n] = 0; }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    for (;;) {
        char name[64];
        fputs("neoos login: ", stdout);
        fflush(stdout);
        if (!fgets(name, sizeof name, stdin)) { return 0; }   // terminal gone
        trim(name);
        if (!name[0]) { continue; }

        char pass[256];
        fputs("password: ", stdout);
        fflush(stdout);
        if (read_secret(pass, sizeof pass) != 0) {
            fputs("login: cannot disable echo; refusing to ask for a password\n", stdout);
            return 1;
        }

        struct account a;
        int have = find_account(name, &a);

        // The password is hashed and compared EVEN WHEN the account does
        // not exist, against a fixed hash, so that a wrong name and a
        // wrong password take the same path. Answering "no such user"
        // faster than "wrong password" tells an attacker which names are
        // real.
        const char *want = have ? a.hash
            : "$6$notauser$0000000000000000000000000000000000000000000000000000000000000000000000000000000000";
        char *got = crypt(pass, want);
        int ok = have && got && strcmp(got, want) == 0;

        // Wipe it: this process goes on to exec a shell, and the
        // password has no business still being in its memory.
        memset(pass, 0, sizeof pass);

        if (!ok) {
            fputs("login incorrect\n\n", stdout);
            continue;
        }

        // Group BEFORE user: after the uid is dropped, the right to
        // change the group is gone with it.
        if (setgid(a.gid) != 0 || setuid(a.uid) != 0) {
            fputs("login: could not drop privilege\n", stdout);
            return 1;
        }

        if (chdir(a.home) != 0) { chdir("/"); }

        setenv("USER", a.name, 1);
        setenv("HOME", a.home, 1);
        setenv("SHELL", a.shell, 1);
        if (!getenv("PATH")) { setenv("PATH", "/bin:/usr/tests", 1); }

        char *argv[] = { a.shell, 0 };
        execve(a.shell, argv, environ);
        fputs("login: could not run the shell\n", stdout);
        return 1;
    }
}
