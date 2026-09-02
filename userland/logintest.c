// logintest.c -- authentication, over a real terminal (N5).
//
// login is driven the way a person drives it: on a pty, in its own
// session, with the answers typed in. What is asserted:
//
//   - a WRONG password is refused and the prompt comes back. This is
//     the case that matters: a login that accepts everything passes
//     every "can I log in?" test ever written.
//   - a wrong USER NAME is refused the same way, and says the same
//     thing -- naming which half was wrong tells an attacker which
//     accounts exist.
//   - a correct password reaches a shell.
//   - that shell runs as the ACCOUNT'S uid, not as god. This is what
//     proves the privilege drop happened rather than being skipped;
//     without it login would "work" while leaving everyone root.
//   - the password is not echoed while it is typed.

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>
#include <auxv.h>

#define TIOCGPTN  0x80045430
#define TIOCSCTTY 0x540E

extern int ioctl(int fd, unsigned long req, void *arg);

static char transcript[8192];
static int  tlen;

static void drain(int m, int ms) {
    int waited = 0;
    while (waited < ms && tlen < (int)sizeof transcript - 1) {
        struct pollfd pf = { m, POLLIN, 0 };
        int pr = poll(&pf, 1, 50);
        waited += 50;
        if (pr <= 0) { continue; }
        if (pf.revents & POLLIN) {
            int r = (int)read(m, transcript + tlen, sizeof transcript - 1 - tlen);
            if (r <= 0) { break; }
            tlen += r;
            transcript[tlen] = 0;
            waited = 0;
        }
        if (pf.revents & (POLLHUP | POLLERR)) { break; }
    }
    transcript[tlen] = 0;
}

static void type(int m, const char *s) { write(m, s, strlen(s)); }

static int saw_from(int start, const char *needle) {
    for (int i = start; i <= tlen; i++) {
        int j = 0;
        while (needle[j] && transcript[i + j] == needle[j]) { j++; }
        if (!needle[j]) { return 1; }
    }
    return 0;
}

static void dump(void) {
    printf("[logintest] transcript (%d bytes):\n", tlen);
    for (int i = 0; i < tlen; i++) {
        char c = transcript[i];
        if (c == '\n') { printf("\n"); }
        else if (c >= 32 && c < 127) { printf("%c", c); }
        else { printf("<%d>", (int)(unsigned char)c); }
    }
    printf("\n[logintest] end of transcript\n");
}

int main(void) {
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0) { printf("[logintest] FAILED: /dev/ptmx (%d)\n", m); return 1; }

    int n = -1;
    if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) {
        printf("[logintest] FAILED: TIOCGPTN\n"); return 1;
    }
    char pts[20] = "/dev/pts/";
    int k = 9;
    if (n >= 10) { pts[k++] = (char)('0' + n / 10); }
    pts[k++] = (char)('0' + n % 10);
    pts[k] = 0;

    int slave = open(pts, O_RDWR);
    if (slave < 0) { printf("[logintest] FAILED: %s\n", pts); return 1; }

    int child = fork();
    if (child < 0) { printf("[logintest] FAILED: fork\n"); return 1; }
    if (child == 0) {
        close(m);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) { close(slave); }
        setsid();
        ioctl(0, TIOCSCTTY, 0);
        char *argv[] = { (char *)"login", 0 };
        execve("/sbin/login.nex", argv, environ);
        exit(127);
    }
    close(slave);

    drain(m, 1500);
    if (!saw_from(0, "login:")) {
        printf("[logintest] FAILED: no login prompt\n");
        dump(); return 1;
    }

    // 1. Right user, WRONG password.
    int mark = tlen;
    type(m, "neo\n");     drain(m, 1000);
    type(m, "wrongpw\n"); drain(m, 2500);
    if (!saw_from(mark, "incorrect")) {
        printf("[logintest] FAILED: a wrong password was not refused\n");
        dump(); return 1;
    }
    // ...and the password must not appear anywhere in what the terminal
    // echoed back.
    if (saw_from(mark, "wrongpw")) {
        printf("[logintest] FAILED: the password was ECHOED to the terminal\n");
        dump(); return 1;
    }

    // 2. A user that does not exist, refused identically.
    mark = tlen;
    type(m, "nosuchuser\n"); drain(m, 1000);
    type(m, "whatever\n");   drain(m, 2500);
    if (!saw_from(mark, "incorrect")) {
        printf("[logintest] FAILED: an unknown user was not refused\n");
        dump(); return 1;
    }

    // 3. The real thing.
    mark = tlen;
    type(m, "neo\n"); drain(m, 1000);
    type(m, "neo\n"); drain(m, 3000);

    // The shell must be running AS THE USER. `id`-style checks need a
    // program; nsh has no such builtin, so the environment login set is
    // the evidence available on the terminal, and the uid is checked
    // through a program below.
    if (!saw_from(mark, "neoos$") && !saw_from(mark, "nsh$")) {
        printf("[logintest] FAILED: a correct password did not reach a shell\n");
        dump(); return 1;
    }

    // 4. The privilege drop. permtest exits 0 only when it starts as
    //    god, so from this shell -- which must NOT be god -- it has to
    //    fail. That is a positive test of a negative property.
    //
    //    The EXIT STATUS is the evidence, not permtest's message: that
    //    goes to /dev/kmsg, which is the serial log, not this terminal.
    //    nsh reports a non-zero status on the terminal, which is
    //    exactly what is needed and is visible here.
    mark = tlen;
    type(m, "/usr/tests/permtest.nex\n"); drain(m, 5000);
    if (!saw_from(mark, "exit 1")) {
        printf("[logintest] FAILED: permtest did not fail -- the shell after "
               "login is still god\n");
        dump(); return 1;
    }

    printf("[logintest] wrong password refused, unknown user refused the same way, "
           "password never echoed, correct password reached a shell running as the "
           "user rather than god\n");
    printf("[logintest] ALL PASSED\n");
    close(m);
    int st = 0;
    waitpid(child, &st, 0);
    return 0;
}
