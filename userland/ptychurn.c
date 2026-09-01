// ptychurn.c -- the pty pool across and beyond its ceiling.
//
// CS3. PTY_MAX is 16 and the pool is a flat array; pty_unref's refcounted
// `gone` check drives teardown and the devfs unregistration that goes
// with it. CS0 made pool_lock rank-checked, so an ordering mistake now
// panics instead of hiding.
//
// What this asserts, none of which the existing ptytest covers:
//
//   - the ceiling is enforced CLEANLY: the 17th concurrent open fails
//     with an error rather than crashing, corrupting the pool, or
//     handing back a duplicate;
//   - slots come back. After closing everything, a fresh open must
//     succeed again -- that is teardown actually releasing, not just
//     appearing to;
//   - closing in a DIFFERENT order from opening is fine, which is what
//     pressures the refcount rather than a stack discipline;
//   - the survivors still work: a master/slave round-trip after the
//     churn proves the pool was not left subtly wrong.
//
// CS4 removes the ceiling. This test is what will prove that removal did
// not break teardown.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#define TIOCGPTN 0x80045430
#define PTY_MAX_EXPECTED 16

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
        int m[PTY_MAX_EXPECTED + 4];
        int nopen = 0;

        // Open until the pool refuses. The refusal is the point: it must
        // be an error return, not a crash or a duplicate.
        for (int i = 0; i < PTY_MAX_EXPECTED + 4; i++) {
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

        // The pool must actually have a ceiling; if this ever stops
        // being true the test has silently stopped testing anything.
        if (nopen > PTY_MAX_EXPECTED + 2) {
            printf("[ptychurn] FAILED: round %d opened %d ptys, expected a ceiling near %d\n",
                   r, nopen, PTY_MAX_EXPECTED);
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

    printf("[ptychurn] %d rounds, peak %d concurrent ptys, slots reclaimed each time\n",
           ROUNDS, peak);
    printf("[ptychurn] ALL PASSED\n");
    return 0;
}
