#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/uio.h>
#include <termios.h>

static int failures;

static void fail(const char *what, int rc) {
    printf("[tier0test] FAILED: %s rc=%d\n", what, rc);
    failures++;
}

// writev is the ONLY path musl's stdio takes to write. A short or
// misordered result here means a musl program prints nothing or prints
// garbage.
static void check_writev(void) {
    int fd = open("/writev.txt", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail("open for writev", fd); return; }

    struct iovec v[3];
    v[0].iov_base = (void *)"one";   v[0].iov_len = 3;
    v[1].iov_base = (void *)"two";   v[1].iov_len = 3;
    v[2].iov_base = (void *)"three"; v[2].iov_len = 5;

    int64_t n = writev(fd, v, 3);
    if (n != 11) { fail("writev total", (int)n); close(fd); return; }

    if (lseek(fd, 0, SEEK_SET) != 0) { fail("lseek", -1); close(fd); return; }
    char buf[16];
    for (int i = 0; i < 16; i++) { buf[i] = 0; }
    if (read(fd, buf, 11) != 11) { fail("read back", -1); close(fd); return; }
    close(fd);

    const char *want = "onetwothree";
    for (int i = 0; i < 11; i++) {
        if (buf[i] != want[i]) { fail("writev concatenation order", i); return; }
    }
    printf("[tier0test] writev passed\n");
}

static void check_readv(void) {
    int fd = open("/writev.txt", O_RDONLY);
    if (fd < 0) { fail("open for readv", fd); return; }
    char a[3], b[3], c[5];
    struct iovec v[3];
    v[0].iov_base = a; v[0].iov_len = 3;
    v[1].iov_base = b; v[1].iov_len = 3;
    v[2].iov_base = c; v[2].iov_len = 5;
    int64_t n = readv(fd, v, 3);
    close(fd);
    if (n != 11) { fail("readv total", (int)n); return; }
    if (a[0] != 'o' || b[0] != 't' || c[0] != 't') { fail("readv split", 0); return; }
    printf("[tier0test] readv passed\n");
}

static void check_writev_errors(void) {
    int fd = open("/writev.txt", O_RDONLY);
    if (fd < 0) { fail("open for writev errors", fd); return; }
    struct iovec v[1];
    v[0].iov_base = (void *)"x"; v[0].iov_len = 1;
    // More vectors than NeoOS accepts is -EINVAL, not a silent truncation.
    int64_t n = writev(fd, v, IOV_MAX + 1);
    close(fd);
    if (n != -EINVAL) { fail("writev over IOV_MAX should be -EINVAL", (int)n); return; }

    n = writev(4096, v, 1);
    if (n != -EBADF) { fail("writev on a bad fd", (int)n); return; }
    printf("[tier0test] writev error cases passed\n");
}

// stdio uses this to choose buffering, and isatty() is built on it.
// /dev/CONSOLE is a real line-discipline terminal, so TIOCGWINSZ on
// stdout succeeds and isatty(1) is true; the -ENOTTY answer is what a
// non-terminal fd gets.
static void check_ioctl(void) {
    struct winsize ws = { 0, 0, 0, 0 };
    int rc = ioctl(1, TIOCGWINSZ, &ws);
    if (rc != 0) { fail("ioctl(TIOCGWINSZ) on the console", rc); return; }
    // The console follows the active virtual terminal, whose size is
    // the framebuffer's (or 80x25 on VGA text) -- just require a
    // sensible non-zero geometry.
    if (ws.ws_row < 24 || ws.ws_col < 80) { fail("console window size", ws.ws_col); return; }
    if (isatty(1) != 1) { fail("isatty(stdout) should be 1", isatty(1)); return; }

    // A regular file is not a terminal.
    int fd = open("/tier0.tmp", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail("open for the non-tty case", fd); return; }
    rc = ioctl(fd, TIOCGWINSZ, &ws);
    close(fd);
    unlink("/tier0.tmp");
    if (rc != -ENOTTY) { fail("ioctl(TIOCGWINSZ) on a file should be -ENOTTY", rc); return; }

    rc = ioctl(4096, TIOCGWINSZ, 0);
    if (rc != -EBADF) { fail("ioctl on a bad fd", rc); return; }
    printf("[tier0test] ioctl / isatty passed\n");
}

static void check_clock(void) {
    struct timespec a, b;
    if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) { fail("clock_gettime(MONOTONIC)", -1); return; }
    if (a.tv_nsec < 0 || a.tv_nsec >= 1000000000L) { fail("tv_nsec out of range", (int)a.tv_nsec); return; }

    // The clock must ADVANCE, which is the property everything timing
    // related depends on. Spin long enough to cross at least one tick.
    for (volatile long i = 0; i < 40000000L; i++) { }
    if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) { fail("second clock_gettime", -1); return; }

    int64_t da = a.tv_sec * 1000000000LL + a.tv_nsec;
    int64_t db = b.tv_sec * 1000000000LL + b.tv_nsec;
    if (db <= da) { fail("clock did not advance", (int)(db - da)); return; }

    if (clock_gettime(CLOCK_REALTIME, &a) != 0) { fail("clock_gettime(REALTIME)", -1); return; }
    if (clock_gettime(999, &a) != -EINVAL) { fail("unknown clock id should be -EINVAL", -1); return; }
    printf("[tier0test] clock_gettime passed\n");
}

static void check_nanosleep(void) {
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    struct timespec req = { 0, 50000000L };   // 50ms = 5 ticks
    if (nanosleep(&req, 0) != 0) { fail("nanosleep", -1); return; }
    clock_gettime(CLOCK_MONOTONIC, &after);

    int64_t db = before.tv_sec * 1000000000LL + before.tv_nsec;
    int64_t da = after.tv_sec  * 1000000000LL + after.tv_nsec;
    // Must sleep at LEAST the requested time -- sleeping short is the
    // failure a caller cannot defend against.
    if (da - db < 50000000LL) { fail("nanosleep returned early", (int)((da - db) / 1000000)); return; }

    struct timespec bad = { 0, 2000000000L };
    if (nanosleep(&bad, 0) != -EINVAL) { fail("out-of-range tv_nsec should be -EINVAL", -1); return; }
    printf("[tier0test] nanosleep passed\n");
}

static void check_set_tid_address(void) {
    int word = 0;
    int tid = set_tid_address(&word);
    // The return value is the caller's tid, which is what musl uses.
    if (tid <= 0) { fail("set_tid_address should return a tid", tid); return; }
    printf("[tier0test] set_tid_address passed (tid=%d)\n", tid);
}

int main(void) {
    check_writev();
    check_readv();
    check_writev_errors();
    check_ioctl();
    check_clock();
    check_nanosleep();
    check_set_tid_address();

    if (failures) {
        printf("[tier0test] SOME CHECKS FAILED\n");
        return 1;
    }
    printf("[tier0test] ALL PASSED\n");
    // exit_group is what musl's _Exit calls; exercising it here means
    // the normal path proves it works.
    exit_group(0);
}
