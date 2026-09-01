// polltrunc.c -- select(2) must report every ready fd, not the first 16.
//
// CS2.2: sys_poll.c collected select()'s interesting descriptors into a
// fixed 16-entry array while accepting nfds up to FD_SETSIZE (1024), so
// every ready fd past the sixteenth it encountered was silently dropped
// -- no error, no truncation flag, just a wrong answer. A shell waiting
// on more than sixteen things would hang on the ones it never heard
// about.

#include <unistd.h>
#include <stdio.h>
#include <poll.h>
#include <neoos_test.h>

#define NPIPES 20

int main(void) {
    int rfd[NPIPES];
    int maxfd = 0;

    // Each pipe gets a byte written into it, so every read end is
    // readable at the same time and select() must name all of them.
    for (int i = 0; i < NPIPES; i++) {
        int fds[2];
        if (pipe(fds) != 0) {
            printf("[polltrunc] FAILED: pipe %d\n", i);
            return 1;
        }
        if (write(fds[1], "x", 1) != 1) {
            printf("[polltrunc] FAILED: write %d\n", i);
            return 1;
        }
        rfd[i] = fds[0];
        if (rfd[i] > maxfd) { maxfd = rfd[i]; }
    }

    fd_set r;
    FD_ZERO(&r);
    for (int i = 0; i < NPIPES; i++) { FD_SET(rfd[i], &r); }

    struct timeval tv = { 0, 0 };   // non-blocking: they are all ready now
    int n = select(maxfd + 1, &r, 0, 0, &tv);
    if (n != NPIPES) {
        printf("[polltrunc] FAILED: select reported %d of %d ready\n", n, NPIPES);
        return 1;
    }
    for (int i = 0; i < NPIPES; i++) {
        if (!FD_ISSET(rfd[i], &r)) {
            printf("[polltrunc] FAILED: fd %d ready but not reported\n", rfd[i]);
            return 1;
        }
    }
    printf("[polltrunc] %d simultaneously-ready fds all reported\n", NPIPES);

    printf("[polltrunc] ALL PASSED\n");
    return 0;
}
