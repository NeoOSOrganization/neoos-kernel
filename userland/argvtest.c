// argvtest.c -- the argument vector across spawn and exec.
//
// CS4. Until this test existed, exec DISCARDED the caller's argv: the
// syscall took only a path, and the kernel built argv = { path } for the
// new image. `sh -c 'ls -la'` would have exec'd `ls` with no arguments
// and no error -- silently the wrong command. The spawn side had a
// vector but capped it at 8 arguments of 128 bytes, and TRUNCATED past
// the cap rather than reporting it.
//
// Both are BusyBox blockers, so both are asserted here:
//
//   - argv survives exec, entry by entry, including entries the old
//     fixed array could not have held (index >= 8, length >= 128);
//   - argv survives spawn the same way;
//   - the ceilings REFUSE rather than truncate. A vector over the total
//     byte budget must come back -E2BIG with the caller still running,
//     because a silently shortened command line is worse than a failure.
//
// One binary in two roles. With arguments it is the CHILD: it checks
// what it was handed and encodes the verdict in its exit status, which
// is the only channel that cannot be faked by the parent. Without them
// it is the PARENT and drives the cases.

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#define SELF "/usr/tests/argvtest.nex"

// Deliberately past the OLD ceilings: 11 entries where 8 fit, and a
// 200-byte argument where 128 fit. Under the previous code this vector
// could not have arrived intact by any path.
#define NARGS 11
#define LONG_LEN 200

static char longarg[LONG_LEN + 1];

static void build_longarg(void) {
    for (int i = 0; i < LONG_LEN; i++) { longarg[i] = (char)('a' + (i % 26)); }
    longarg[LONG_LEN] = 0;
}

// argv[0] is the path; argv[1..] are "a1".."a9" then the long one.
static const char *expect(int i) {
    static char buf[8];
    if (i == 0) { return SELF; }
    if (i == NARGS - 1) { return longarg; }
    buf[0] = 'a'; buf[1] = (char)('0' + i); buf[2] = 0;
    return buf;
}

// Exit codes: 0 pass, 10 wrong argc, 11 wrong string, 12 null entry.
static int child_main(int argc, char **argv) {
    if (argc != NARGS) { return 10; }
    for (int i = 0; i < NARGS; i++) {
        if (!argv[i]) { return 12; }
        if (strcmp(argv[i], expect(i)) != 0) { return 11; }
    }
    // argv must be NULL-terminated, as every C program is entitled to
    // assume -- BusyBox walks it that way rather than trusting argc.
    if (argv[NARGS] != 0) { return 13; }
    return 0;
}

static void fill(char **argv) {
    for (int i = 0; i < NARGS; i++) { argv[i] = (char *)expect(i); }
    argv[NARGS] = 0;
}

static const char *why(int code) {
    switch (code) {
    case 10: return "wrong argc";
    case 11: return "an argument came through wrong";
    case 12: return "a null entry inside argv";
    case 13: return "argv was not NULL-terminated";
    default: return "unknown";
    }
}

static int reap(const char *what, int pid) {
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        printf("[argvtest] FAILED: %s: waitpid\n", what);
        return 1;
    }
    if (!WIFEXITED(st)) {
        printf("[argvtest] FAILED: %s: child died by signal\n", what);
        return 1;
    }
    int code = WEXITSTATUS(st);
    if (code != 0) {
        printf("[argvtest] FAILED: %s: child exit %d -- %s\n", what, code, why(code));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    build_longarg();
    if (argc > 1) { return child_main(argc, argv); }

    // Rebuild expect()'s strings into storage that survives: expect()
    // returns a static buffer, so the vector must be filled from copies.
    static char slots[NARGS][LONG_LEN + 1];
    char *vec[NARGS + 1];
    for (int i = 0; i < NARGS; i++) {
        const char *e = expect(i);
        int n = 0;
        while (e[n] && n < LONG_LEN) { slots[i][n] = e[n]; n++; }
        slots[i][n] = 0;
        vec[i] = slots[i];
    }
    vec[NARGS] = 0;
    (void)fill;

    // 1. exec. This is the case that was broken outright.
    int pid = fork();
    if (pid < 0) { printf("[argvtest] FAILED: fork\n"); return 1; }
    if (pid == 0) {
        execv(SELF, vec);
        // Only reached if exec failed; the parent sees it as exit 20.
        return 20;
    }
    if (reap("execv", pid) != 0) { return 1; }

    // 2. spawn, over the old 8-argument ceiling.
    int sp = spawnv(SELF, vec);
    if (sp < 0) { printf("[argvtest] FAILED: spawnv returned %d\n", sp); return 1; }
    if (reap("spawnv", sp) != 0) { return 1; }

    // 3. The ceiling must REFUSE, not truncate. 512 KiB of arguments is
    // past SPAWN_ARG_TOTAL (256 KiB), so this must fail -- and this
    // process must still be here afterwards to say so, which is the
    // other half of the assertion.
    static char big[8192];
    for (int i = 0; i < (int)sizeof(big) - 1; i++) { big[i] = 'x'; }
    big[sizeof(big) - 1] = 0;
    char *huge[66];
    huge[0] = (char *)SELF;
    for (int i = 1; i < 65; i++) { huge[i] = big; }   // 64 * 8 KiB = 512 KiB
    huge[65] = 0;
    int rc = spawnv(SELF, huge);
    if (rc >= 0) {
        printf("[argvtest] FAILED: a 512 KiB argv was ACCEPTED (pid %d) -- "
               "the total-size ceiling is not enforced\n", rc);
        return 1;
    }
    if (rc != -E2BIG) {
        printf("[argvtest] FAILED: oversized argv gave %d, expected -E2BIG (%d)\n",
               rc, -E2BIG);
        return 1;
    }

    printf("[argvtest] %d arguments (longest %d bytes) survived exec and spawn; "
           "512 KiB argv refused with E2BIG\n", NARGS, LONG_LEN);
    printf("[argvtest] ALL PASSED\n");
    return 0;
}
