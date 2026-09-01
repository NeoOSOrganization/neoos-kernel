// polltest.c -- poll(2) and select(2) over a pipe and /dev/input/event0.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <neoos_test.h>

int main(void) {
    int p[2];
    if (pipe(p) != 0) { printf("[polltest] FAILED: pipe\n"); return 1; }

    struct pollfd pf = { p[0], POLLIN, 0 };

    // Empty pipe, short timeout -> times out with no revents.
    int r = poll(&pf, 1, 50);
    if (r != 0 || pf.revents != 0) {
        printf("[polltest] FAILED: empty pipe not a timeout (r=%d rev=%d)\n", r, pf.revents);
        return 1;
    }

    // A child writes; poll must then report POLLIN.
    int kid = fork();
    if (kid == 0) { write(p[1], "x", 1); exit(0); }
    wait(kid);
    pf.revents = 0;
    r = poll(&pf, 1, 2000);
    if (r != 1 || !(pf.revents & POLLIN)) {
        printf("[polltest] FAILED: POLLIN after write (r=%d rev=%d)\n", r, pf.revents);
        return 1;
    }
    printf("[polltest] pipe POLLIN passed\n");

    // Drain, close the write end -> POLLHUP.
    char c; read(p[0], &c, 1);
    close(p[1]);
    pf.revents = 0;
    r = poll(&pf, 1, 2000);
    if (!(pf.revents & POLLHUP)) {
        printf("[polltest] FAILED: POLLHUP after writer close (rev=%d)\n", pf.revents);
        return 1;
    }
    close(p[0]);
    printf("[polltest] pipe POLLHUP passed\n");

    // select over a fresh pipe.
    if (pipe(p) != 0) { printf("[polltest] FAILED: pipe2\n"); return 1; }
    fd_set rd; FD_ZERO(&rd); FD_SET(p[0], &rd);
    struct timeval tv = { 0, 50000 };
    r = select(p[0] + 1, &rd, 0, 0, &tv);
    if (r != 0) { printf("[polltest] FAILED: select on empty pipe (r=%d)\n", r); return 1; }
    write(p[1], "y", 1);
    FD_ZERO(&rd); FD_SET(p[0], &rd);
    tv.tv_sec = 2; tv.tv_usec = 0;
    r = select(p[0] + 1, &rd, 0, 0, &tv);
    if (r < 1 || !FD_ISSET(p[0], &rd)) {
        printf("[polltest] FAILED: select POLLIN (r=%d)\n", r);
        return 1;
    }
    close(p[0]); close(p[1]);
    printf("[polltest] select passed\n");

    // evdev: not ready with no input; POLLIN after an injected key
    // (test-hooks build only -- otherwise the inject returns -ENOSYS
    // and we skip).
    int ev = open("/dev/input/event0", O_RDONLY);
    if (ev >= 0) {
        struct pollfd e = { ev, POLLIN, 0 };
        // Drain any events another test injected before we got here
        // (activettytest injects KEY_A too). Then the queue is ours.
        fcntl(ev, F_SETFL, O_NONBLOCK);
        char drain[128];
        while (read(ev, drain, sizeof drain) > 0) { }
        fcntl(ev, F_SETFL, 0);
        if (poll(&e, 1, 30) != 0) {
            printf("[polltest] FAILED: evdev ready with no input\n");
            return 1;
        }
        long rc = neoos_test_inject_key(30 /*KEY_A*/, 1);
        if (rc == 0) {
            e.revents = 0;
            if (poll(&e, 1, 2000) != 1 || !(e.revents & POLLIN)) {
                printf("[polltest] FAILED: evdev POLLIN after inject (rev=%d)\n", e.revents);
                return 1;
            }
            char buf[64]; read(ev, buf, sizeof buf);
            neoos_test_inject_key(30, 0);
            printf("[polltest] evdev poll passed\n");
        } else {
            printf("[polltest] evdev poll skipped (production kernel)\n");
        }
        close(ev);
    }

    printf("[polltest] ALL PASSED\n");
    return 0;
}
