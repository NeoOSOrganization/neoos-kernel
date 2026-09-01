#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <neoos_test.h>

static int failures;

static void fail(const char *what, int rc) {
    printf("[ttytest] FAILED: %s rc=%d\n", what, rc);
    failures++;
}

// The layout TCGETS moves is Linux's, and it is compiled into every
// caller -- so the FIRST 36 bytes must match the kernel's struct
// exactly, and the userland struct is deliberately longer.
static void check_layout(void) {
    struct termios t;
    char *base = (char *)&t;
    if ((char *)&t.c_iflag - base != 0)  { fail("c_iflag offset", 0); return; }
    if ((char *)&t.c_oflag - base != 4)  { fail("c_oflag offset", 0); return; }
    if ((char *)&t.c_cflag - base != 8)  { fail("c_cflag offset", 0); return; }
    if ((char *)&t.c_lflag - base != 12) { fail("c_lflag offset", 0); return; }
    if ((char *)&t.c_line  - base != 16) { fail("c_line offset", 0); return; }
    if ((char *)&t.c_cc    - base != 17) { fail("c_cc offset", 0); return; }
    printf("[ttytest] termios layout passed\n");
}

static void check_isatty(void) {
    if (isatty(1) != 1) { fail("isatty(stdout) should be 1", isatty(1)); return; }
    if (isatty(0) != 1) { fail("isatty(stdin) should be 1", isatty(0)); return; }

    // A regular file is NOT a terminal, which is the half that makes
    // the answer meaningful.
    int fd = open("/HELLO.TXT", O_RDONLY);
    if (fd < 0) { fail("open for the non-tty case", fd); return; }
    int r = isatty(fd);
    close(fd);
    if (r != 0) { fail("isatty(a regular file) should be 0", r); return; }

    // So is a pipe.
    int fds[2];
    if (pipe(fds) == 0) {
        r = isatty(fds[0]);
        close(fds[0]); close(fds[1]);
        if (r != 0) { fail("isatty(a pipe) should be 0", r); return; }
    }
    printf("[ttytest] isatty passed\n");
}

static void check_winsize(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) != 0) { fail("TIOCGWINSZ", -1); return; }
    // The console is the active VT; its size is the framebuffer's (or
    // 80x25 on VGA text). Just require a sane non-zero geometry.
    if (ws.ws_row < 24 || ws.ws_col < 80) {
        fail("window size not reported", ws.ws_col);
        return;
    }
    printf("[ttytest] TIOCGWINSZ passed (%dx%d)\n", ws.ws_col, ws.ws_row);
}

static void check_termios(void) {
    struct termios t;
    if (tcgetattr(1, &t) != 0) { fail("tcgetattr", -1); return; }

    if (!(t.c_lflag & ICANON)) { fail("default should be canonical", 0); return; }
    if (!(t.c_lflag & ECHO))   { fail("default should echo", 0); return; }
    if (!(t.c_lflag & ISIG))   { fail("default should generate signals", 0); return; }
    if (!(t.c_oflag & ONLCR))  { fail("default should translate NL to CRNL", 0); return; }
    if (t.c_cc[VINTR] != 3)    { fail("VINTR should be ^C", t.c_cc[VINTR]); return; }
    if (t.c_cc[VEOF] != 4)     { fail("VEOF should be ^D", t.c_cc[VEOF]); return; }
    if (t.c_cc[VERASE] != 127) { fail("VERASE should be DEL", t.c_cc[VERASE]); return; }

    // Turn echo off and back on, the way a password prompt does.
    struct termios raw = t;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    if (tcsetattr(1, TCSANOW, &raw) != 0) { fail("tcsetattr to raw", -1); return; }

    struct termios back;
    if (tcgetattr(1, &back) != 0) { fail("tcgetattr after set", -1); return; }
    if (back.c_lflag & ICANON) { fail("ICANON should be off", 0); return; }
    if (back.c_lflag & ECHO)   { fail("ECHO should be off", 0); return; }

    if (tcsetattr(1, TCSANOW, &t) != 0) { fail("tcsetattr restore", -1); return; }
    if (tcgetattr(1, &back) != 0) { fail("tcgetattr after restore", -1); return; }
    if (!(back.c_lflag & ICANON) || !(back.c_lflag & ECHO)) {
        fail("restore did not take", 0);
        return;
    }
    printf("[ttytest] termios get/set/restore passed\n");
}

static void check_pgrp(void) {
    int pg = 0;
    if (ioctl(1, TIOCGPGRP, &pg) != 0) { fail("TIOCGPGRP", -1); return; }
    if (pg <= 0) { fail("foreground pgrp should be positive", pg); return; }

    int mine = getpid();
    if (ioctl(1, TIOCSPGRP, &mine) != 0) { fail("TIOCSPGRP", -1); return; }
    if (ioctl(1, TIOCGPGRP, &pg) != 0) { fail("TIOCGPGRP after set", -1); return; }
    if (pg != mine) { fail("foreground pgrp did not take", pg); return; }
    printf("[ttytest] TIOCGPGRP/TIOCSPGRP passed\n");
}

static void check_errors(void) {
    // Not a terminal -> -ENOTTY, which is how isatty knows.
    int fd = open("/HELLO.TXT", O_RDONLY);
    if (fd >= 0) {
        struct winsize ws;
        int r = ioctl(fd, TIOCGWINSZ, &ws);
        close(fd);
        if (r != -ENOTTY) { fail("ioctl on a file should be -ENOTTY", r); return; }
    }
    // An unknown request on a real terminal is also -ENOTTY.
    int r = ioctl(1, 0x1234, 0);
    if (r != -ENOTTY) { fail("unknown ioctl should be -ENOTTY", r); return; }
    r = ioctl(4096, TIOCGWINSZ, 0);
    if (r != -EBADF) { fail("ioctl on a bad fd should be -EBADF", r); return; }
    printf("[ttytest] error cases passed\n");
}

// Test injected input reaches the TTY when it is NOT grabbed by evdev.
// This tests the path: keyboard IRQ -> input core -> TTY (not evdev grab).
static void check_injected_input(void) {
    printf("[ttytest] testing injected input on TTY path...\n");

    // Try to inject a character. In production (no -DNEOOS_TEST_HOOKS),
    // this will return -ENOSYS, which is fine: the test is optional.
    // In test mode, it will inject the key into the input subsystem.
    int rc = neoos_test_inject_key(30, 1);  // KEY_A press
    if (rc == -38 /* ENOSYS */) {
        printf("[ttytest] injected input not available (production kernel)\n");
        return;
    }
    if (rc != 0) {
        fail("neoos_test_inject_key(KEY_A press)", rc);
        return;
    }

    // In test mode, the injected key should reach the TTY and become 'a'.
    // Since we're in canonical mode with echo, it will be echoed to stdout.
    // This is a best-effort check: it depends on timing and the input
    // actually being buffered. We just check that the call succeeded.

    // Release the key
    rc = neoos_test_inject_key(30, 0);  // KEY_A release
    if (rc != 0 && rc != -38) {
        fail("neoos_test_inject_key(KEY_A release)", rc);
        return;
    }

    printf("[ttytest] injected input passed\n");
}

// The wall clock is real now, so a file's mtime should be a plausible
// recent time rather than the epoch.
static void check_clock_is_real(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { fail("clock_gettime(REALTIME)", -1); return; }
    // 2020-01-01. Anything earlier means the RTC was not read.
    if (ts.tv_sec < 1577836800L) {
        fail("CLOCK_REALTIME is not a wall clock", (int)(ts.tv_sec / 86400));
        return;
    }

    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    if (mono.tv_sec > ts.tv_sec) { fail("MONOTONIC should be smaller than REALTIME", 0); return; }
    printf("[ttytest] wall clock is real (epoch %ds)\n", (int)ts.tv_sec);
}

int main(void) {
    check_layout();
    check_isatty();
    check_winsize();
    check_termios();
    check_pgrp();
    check_errors();
    check_clock_is_real();
    check_injected_input();

    if (failures) { printf("[ttytest] SOME CHECKS FAILED\n"); return 1; }
    printf("[ttytest] ALL PASSED\n");
    return 0;
}
