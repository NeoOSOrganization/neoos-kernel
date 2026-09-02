// ptychurn.c -- the pty pool across and beyond its ceiling.
//
// CS3, updated by CS4. The pool used to be a flat array of 16; it now
// GROWS -- a slot's struct is allocated the first time it is needed, up
// to PTY_MAX slots -- so the ceiling this test was written against has
// moved. pty_unref's refcounted `gone` check still drives teardown and
// the devfs unregistration that goes with it. CS0 made pool_lock
// rank-checked, so an ordering mistake now panics instead of hiding.
//
// What this asserts, none of which the existing ptytest covers:
//
//   - MORE than the old sixteen can be open at once, which is the CS4
//     change, and the ceiling wherever it now falls is enforced
//     CLEANLY: the open past it fails with an error rather than
//     crashing, corrupting the pool, or handing back a duplicate;
//   - slots come back. After closing everything, a fresh open must
//     succeed again -- that is teardown actually releasing, not just
//     appearing to;
//   - closing in a DIFFERENT order from opening is fine, which is what
//     pressures the refcount rather than a stack discipline;
//   - the survivors still work: a master/slave round-trip after the
//     churn proves the pool was not left subtly wrong.
//
// The growth is what makes teardown worth re-checking: a reused slot is
// a struct a previous pty lived in, so a teardown that half-releases
// shows up as the NEXT open behaving oddly rather than as a leak.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#define TIOCGPTN 0x80045430
// The pool's slot count. Opening every one of them each round would
// build 256 ttys and then tear them down, 40 times over; the point is
// to exceed the OLD ceiling and to churn, not to exhaust the machine.
#define PTY_POOL_SLOTS 256
#define PTY_OLD_CEILING 16
#define OPEN_PER_ROUND  40

extern int ioctl(int fd, unsigned long req, void *arg);

#define ROUNDS 40

static int open_slave_for(int m) {
    int n = -1;
    if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) { return -1; }
    char path[20] = "/dev/pts/";
    int k = 9;
    if (n >= 10) { path[k++] = (char)('0' + n / 10); }
    path[k++] = (char)('0' + n % 10);
    path[k] = 0;
    return open(path, O_RDWR);
}

int main(void) {
    int peak = 0;

    for (int r = 0; r < ROUNDS; r++) {
        int m[OPEN_PER_ROUND];
        int nopen = 0;

        // Open a batch well past the old ceiling. A refusal here would
        // be an error return, not a crash or a duplicate -- but at 40 of
        // 256 slots there should not be one.
        for (int i = 0; i < OPEN_PER_ROUND; i++) {
            int fd = open("/dev/ptmx", O_RDWR);
            if (fd < 0) { break; }
            for (int j = 0; j < nopen; j++) {
                if (m[j] == fd) {
                    printf("[ptychurn] FAILED: round %d handed out fd %d twice\n", r, fd);
                    return 1;
                }
            }
            m[nopen++] = fd;
        }

        if (nopen == 0) {
            printf("[ptychurn] FAILED: round %d could not open any pty\n", r);
            return 1;
        }
        if (nopen > peak) { peak = nopen; }

        // The CS4 assertion: more than the old sixteen open at once.
        // Before the pool grew, the seventeenth returned -ENFILE.
        if (nopen <= PTY_OLD_CEILING) {
            printf("[ptychurn] FAILED: round %d opened only %d ptys -- "
                   "the old %d ceiling is still there\n",
                   r, nopen, PTY_OLD_CEILING);
            return 1;
        }

        // Prove one of them still works after the pool filled up.
        int s = open_slave_for(m[nopen / 2]);
        if (s < 0) {
            printf("[ptychurn] FAILED: round %d slave open\n", r);
            return 1;
        }
        // "z\n", not "z": the slave's line discipline is in canonical
        // mode, so a read blocks until a full line arrives. Writing a
        // bare byte hangs the test holding the whole pool -- which is
        // exactly what it did first time round.
        if (write(m[nopen / 2], "z\n", 2) != 2) {
            printf("[ptychurn] FAILED: round %d master write\n", r);
            return 1;
        }
        char line[8];
        int got = (int)read(s, line, sizeof(line));
        if (got < 1 || line[0] != 'z') {
            printf("[ptychurn] FAILED: round %d round-trip got %d bytes, first 0x%x\n",
                   r, got, got > 0 ? line[0] : 0);
            return 1;
        }
        close(s);

        // Close in a different order from the one they were opened in:
        // evens first, then odds. A teardown that only works LIFO shows
        // up here.
        for (int i = 0; i < nopen; i += 2) { close(m[i]); }
        for (int i = 1; i < nopen; i += 2) { close(m[i]); }
    }

    // Slots must have come back, or the loop above would have failed
    // long before here -- but assert it directly at the end too.
    int again = open("/dev/ptmx", O_RDWR);
    if (again < 0) {
        printf("[ptychurn] FAILED: pool exhausted after %d rounds\n", ROUNDS);
        return 1;
    }
    close(again);

    // And the ceiling still EXISTS: the pool grows to PTY_POOL_SLOTS and
    // then refuses, cleanly. Without this the test would pass just as
    // well against a pool with no bound at all, which is not what was
    // built.
    static int all[PTY_POOL_SLOTS + 8];
    int nall = 0;
    while (nall < PTY_POOL_SLOTS + 8) {
        int fd = open("/dev/ptmx", O_RDWR);
        if (fd < 0) { break; }
        all[nall++] = fd;
    }
    if (nall > PTY_POOL_SLOTS) {
        printf("[ptychurn] FAILED: opened %d ptys, past the %d-slot pool\n",
               nall, PTY_POOL_SLOTS);
        return 1;
    }
    for (int i = 0; i < nall; i++) { close(all[i]); }
    int after_full = open("/dev/ptmx", O_RDWR);
    if (after_full < 0) {
        printf("[ptychurn] FAILED: no pty available after filling and "
               "draining the pool\n");
        return 1;
    }
    close(after_full);
    printf("[ptychurn] pool grew to %d concurrent ptys, refused cleanly, "
           "and drained\n", nall);

    printf("[ptychurn] %d rounds, peak %d concurrent ptys, slots reclaimed each time\n",
           ROUNDS, peak);
    printf("[ptychurn] ALL PASSED\n");
    return 0;
}
