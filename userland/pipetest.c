// Exercises POSIX pipes: the byte stream itself, the two end-of-stream
// rules, blocking in both directions, and inheritance across fork.
//
// The rules are what this is really testing. A ring buffer that moves
// bytes is easy; a pipe that correctly reports EOF to a blocked reader
// when the last writer closes, and correctly kills a writer whose last
// reader went away, is where pipes are actually got wrong.

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>

// Larger than the kernel's 4KiB pipe capacity, so writing it MUST
// block and be drained in pieces. A test that fits in the buffer never
// exercises the blocking path at all.
#define BIG_BYTES 20000

// Deliberately larger than the kernel's pipe capacity, and named here
// rather than including a kernel header: the capacity is an observable
// property of the interface, not a number userland should import.
#define PIPE_FILL 8192

static int check_basic(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        printf("[pipetest] FAILED: pipe()\n");
        return 0;
    }

    const char *msg = "hello through a pipe";
    int64_t n = write(fds[1], msg, strlen(msg));
    if (n != (int64_t)strlen(msg)) {
        printf("[pipetest] FAILED: short write %d\n", (int)n);
        return 0;
    }

    char buf[64];
    n = read(fds[0], buf, sizeof(buf));
    if (n != (int64_t)strlen(msg)) {
        printf("[pipetest] FAILED: short read %d\n", (int)n);
        return 0;
    }
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0) {
        printf("[pipetest] FAILED: got \"%s\"\n", buf);
        return 0;
    }

    // A pipe is not seekable.
    if (lseek(fds[0], 0, SEEK_SET) != -ESPIPE) {
        printf("[pipetest] FAILED: lseek on a pipe did not return ESPIPE\n");
        return 0;
    }
    // Nor is either end usable in the wrong direction.
    if (write(fds[0], "x", 1) != -EBADF) {
        printf("[pipetest] FAILED: wrote to the read end\n");
        return 0;
    }
    if (read(fds[1], buf, 1) != -EBADF) {
        printf("[pipetest] FAILED: read from the write end\n");
        return 0;
    }

    close(fds[0]);
    close(fds[1]);
    printf("[pipetest] basic write/read passed\n");
    return 1;
}

static int check_eof(void) {
    int fds[2];
    if (pipe(fds) != 0) { printf("[pipetest] FAILED: pipe() for eof\n"); return 0; }

    write(fds[1], "ab", 2);
    close(fds[1]);            // the last writer goes away

    char buf[8];
    // Buffered data must still be readable AFTER the writer closed:
    // closing the write end means end-of-stream, not discard.
    if (read(fds[0], buf, sizeof(buf)) != 2) {
        printf("[pipetest] FAILED: buffered bytes lost when the writer closed\n");
        return 0;
    }
    // And only now is it EOF.
    if (read(fds[0], buf, sizeof(buf)) != 0) {
        printf("[pipetest] FAILED: read past the last byte did not return 0\n");
        return 0;
    }
    close(fds[0]);
    printf("[pipetest] eof after the last writer closes passed\n");
    return 1;
}

// ------------------------------------------------------- blocking, both ways

static int  big_fds[2];
static char big_src[BIG_BYTES];

static void *big_writer(void *arg) {
    (void)arg;
    uint64_t sent = 0;
    while (sent < BIG_BYTES) {
        int64_t n = write(big_fds[1], big_src + sent, BIG_BYTES - sent);
        if (n <= 0) { break; }
        sent += (uint64_t)n;
    }
    // Closing is what lets the reader see EOF and stop. Without it the
    // reader below blocks for ever and the harness reports a missing
    // marker -- which is the point: this test proves the wake happens.
    close(big_fds[1]);
    return (void *)sent;
}

static int check_blocking(void) {
    for (int i = 0; i < BIG_BYTES; i++) { big_src[i] = (char)(i & 0x7F); }

    if (pipe(big_fds) != 0) { printf("[pipetest] FAILED: pipe() for big\n"); return 0; }

    pthread_t w;
    if (pthread_create(&w, 0, big_writer, 0) != 0) {
        printf("[pipetest] FAILED: pthread_create for writer\n");
        return 0;
    }

    static char got[BIG_BYTES];
    uint64_t total = 0;
    for (;;) {
        int64_t n = read(big_fds[0], got + total, BIG_BYTES - total);
        if (n == 0) { break; }               // writer closed
        if (n < 0) {
            printf("[pipetest] FAILED: read returned %d\n", (int)n);
            return 0;
        }
        total += (uint64_t)n;
        if (total == BIG_BYTES) {
            // Drain once more so the loop sees the EOF rather than
            // stopping on a byte count -- that is the interesting wake.
            continue;
        }
    }

    void *sent = 0;
    pthread_join(w, &sent);
    close(big_fds[0]);

    if (total != BIG_BYTES || (uint64_t)(long)sent != BIG_BYTES) {
        printf("[pipetest] FAILED: moved %d bytes, wrote %d, want %d\n",
               (int)total, (int)(long)sent, BIG_BYTES);
        return 0;
    }
    for (int i = 0; i < BIG_BYTES; i++) {
        if (got[i] != (char)(i & 0x7F)) {
            printf("[pipetest] FAILED: byte %d corrupted\n", i);
            return 0;
        }
    }
    printf("[pipetest] blocking write/read of %d bytes passed\n", BIG_BYTES);
    return 1;
}

// ---------------------------------------------------------------- SIGPIPE

static volatile int got_sigpipe;
static void sigpipe_handler(int sig) { (void)sig; got_sigpipe = 1; }

static int check_sigpipe(void) {
    struct sigaction sa;
    sa.sa_handler = sigpipe_handler;
    sa.sa_flags   = 0;
    sa.sa_mask    = 0;
    if (sigaction(SIGPIPE, &sa, 0) != 0) {
        printf("[pipetest] FAILED: sigaction(SIGPIPE)\n");
        return 0;
    }

    int fds[2];
    if (pipe(fds) != 0) { printf("[pipetest] FAILED: pipe() for sigpipe\n"); return 0; }
    close(fds[0]);            // the last reader goes away

    got_sigpipe = 0;
    int64_t n = write(fds[1], "x", 1);
    if (!got_sigpipe) {
        printf("[pipetest] FAILED: writing to a readerless pipe raised no SIGPIPE\n");
        return 0;
    }
    if (n != -EPIPE) {
        printf("[pipetest] FAILED: write returned %d, want -EPIPE\n", (int)n);
        return 0;
    }
    close(fds[1]);

    sa.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &sa, 0);
    printf("[pipetest] SIGPIPE on a readerless pipe passed\n");
    return 1;
}

// ------------------------------------------------------------- O_NONBLOCK

static int check_nonblock(void) {
    int fds[2];
    if (pipe2(fds, O_NONBLOCK) != 0) {
        printf("[pipetest] FAILED: pipe2(O_NONBLOCK)\n");
        return 0;
    }
    char buf[8];
    if (read(fds[0], buf, sizeof(buf)) != -EAGAIN) {
        printf("[pipetest] FAILED: nonblocking read of an empty pipe did not return EAGAIN\n");
        return 0;
    }
    // Fill it. The pipe holds one page, so this must stop short of what
    // was asked for rather than blocking.
    static char filler[PIPE_FILL];
    int64_t n = write(fds[1], filler, sizeof(filler));
    if (n <= 0 || n >= (int64_t)sizeof(filler)) {
        printf("[pipetest] FAILED: nonblocking write moved %d of %d bytes\n",
               (int)n, (int)sizeof(filler));
        return 0;
    }
    close(fds[0]);
    close(fds[1]);
    printf("[pipetest] O_NONBLOCK passed, partial write=%d\n", (int)n);
    return 1;
}

// ----------------------------------------------------------- across fork

static int check_fork(void) {
    int fds[2];
    if (pipe(fds) != 0) { printf("[pipetest] FAILED: pipe() for fork\n"); return 0; }

    int child = fork();
    if (child < 0) { printf("[pipetest] FAILED: fork\n"); return 0; }
    if (child == 0) {
        close(fds[0]);                    // the child writes only
        write(fds[1], "from the child", 14);
        close(fds[1]);
        exit(0);
    }

    close(fds[1]);                        // the parent reads only
    char buf[32];
    int64_t n = read(fds[0], buf, sizeof(buf));
    if (n != 14) {
        printf("[pipetest] FAILED: read %d bytes from the child, want 14\n", (int)n);
        return 0;
    }
    buf[n] = '\0';
    if (strcmp(buf, "from the child") != 0) {
        printf("[pipetest] FAILED: child sent \"%s\"\n", buf);
        return 0;
    }
    // The parent closed its write end and the child closed its own on
    // exit, so this must be EOF. If fork had not given the child its
    // own reference on the pipe -- or if the parent's close had
    // miscounted the ends -- this would either block for ever or have
    // reported EOF before the data arrived.
    if (read(fds[0], buf, sizeof(buf)) != 0) {
        printf("[pipetest] FAILED: no EOF after the child exited\n");
        return 0;
    }
    close(fds[0]);

    int st = 0;
    wait4(child, &st, 0, 0);
    printf("[pipetest] inherited across fork passed\n");
    return 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int ok = 1;
    ok &= check_basic();
    ok &= check_eof();
    ok &= check_blocking();
    ok &= check_sigpipe();
    ok &= check_nonblock();
    ok &= check_fork();

    printf("[pipetest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
