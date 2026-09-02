// permtest.c -- credentials, and what an ordinary process may not do.
//
// N3. NeoOS's superuser is `god`, uid 0. The rule for signals is one
// line -- god may signal anything, anyone else only their own processes
// -- and "a normal process cannot kill init" falls out of it, because
// init runs as god.
//
// The NEGATIVE cases are the point. A permission check that has never
// been observed refusing anything is not known to work: it would pass
// every positive test while returning 0 unconditionally. So this asserts
// what must FAIL as carefully as what must succeed:
//
//   - an ordinary process cannot signal init (god's)      -> -EPERM
//   - it cannot signal a process owned by another uid     -> -EPERM
//   - it cannot even PROBE one with sig 0                 -> -EPERM
//   - it cannot climb back to god                         -> -EPERM
//   - it CAN signal its own                               -> 0
//   - god can signal anything                             -> 0
//
// The probe case matters on its own: a caller that may not signal a
// process must not be able to use kill(pid, 0) to discover whether it
// exists.

#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#define UID_ALICE 1000
#define UID_BOB   1001

static int failures;

static void fail(const char *what, int got, int want) {
    printf("[permtest] FAILED: %s -> %d, expected %d\n", what, got, want);
    failures++;
}

// The child drops to `uid` and reports, through its exit status, whether
// each rule held. Exit codes are per-check so a failure names itself.
static int child_as(int uid, int victim_pid, int own_pid) {
    if (setgid(uid) != 0) { return 60; }
    if (setuid(uid) != 0) { return 61; }
    if (getuid() != uid)  { return 62; }

    // Cannot climb back.
    if (setuid(0) != -EPERM) { return 63; }
    if (getuid() != uid)     { return 64; }   // and the failed call changed nothing

    // Cannot signal init, which is god's.
    if (kill(1, SIGTERM) != -EPERM) { return 65; }
    // ...nor even probe it.
    if (kill(1, 0) != -EPERM) { return 66; }

    // Cannot signal a process owned by a different uid.
    if (victim_pid > 0 && kill(victim_pid, SIGTERM) != -EPERM) { return 67; }

    // CAN signal its own. SIGCONT: harmless to a running process, and
    // unlike sig 0 it exercises real delivery rather than the probe path.
    if (kill(own_pid, SIGCONT) != 0) { return 68; }

    return 0;
}

static const char *why(int code) {
    switch (code) {
    case 60: return "setgid failed";
    case 61: return "setuid failed";
    case 62: return "getuid did not report the new uid";
    case 63: return "an ordinary process climbed back to god";
    case 64: return "a REFUSED setuid still changed the uid";
    case 65: return "an ordinary process signalled init";
    case 66: return "an ordinary process probed init with sig 0";
    case 67: return "an ordinary process signalled another uid's process";
    case 68: return "a process could not signal its OWN";
    default: return "unknown";
    }
}

int main(void) {
    if (getuid() != 0) {
        printf("[permtest] FAILED: not started as god (uid=%d)\n", getuid());
        return 1;
    }
    printf("[permtest] running as god (uid 0)\n");

    // A process owned by a DIFFERENT ordinary uid, for the cross-user
    // case. It sleeps by blocking on a pipe nobody writes to, so it is
    // alive for the whole test and needs no timing guesswork.
    int p[2];
    if (pipe(p) != 0) { printf("[permtest] FAILED: pipe\n"); return 1; }
    int victim = fork();
    if (victim < 0) { printf("[permtest] FAILED: fork victim\n"); return 1; }
    if (victim == 0) {
        close(p[1]);
        if (setgid(UID_BOB) != 0 || setuid(UID_BOB) != 0) { exit(70); }
        char c;
        read(p[0], &c, 1);         // blocks until the parent closes p[1]
        exit(0);
    }
    close(p[0]);

    int kid = fork();
    if (kid < 0) { printf("[permtest] FAILED: fork\n"); return 1; }
    if (kid == 0) {
        close(p[1]);
        exit(child_as(UID_ALICE, victim, getpid()));
    }

    int st = 0;
    waitpid(kid, &st, 0);
    if (!WIFEXITED(st)) {
        printf("[permtest] FAILED: the unprivileged child died by signal\n");
        failures++;
    } else if (WEXITSTATUS(st) != 0) {
        fail(why(WEXITSTATUS(st)), WEXITSTATUS(st), 0);
    } else {
        printf("[permtest] an ordinary uid: cannot touch init, cannot touch "
               "another uid, cannot climb back, can signal its own\n");
    }

    // god may signal what the ordinary process could not.
    int rc = kill(victim, SIGTERM);
    if (rc != 0) { fail("god signalling another uid's process", rc, 0); }
    else { printf("[permtest] god may signal any process\n"); }

    close(p[1]);                  // release the victim if it survived
    waitpid(victim, &st, 0);

    // god's own identity is unchanged by any of the above.
    if (getuid() != 0) { fail("god's uid changed", getuid(), 0); }

    if (failures) { printf("[permtest] SOME CHECKS FAILED\n"); return 1; }
    printf("[permtest] ALL PASSED\n");
    return 0;
}
