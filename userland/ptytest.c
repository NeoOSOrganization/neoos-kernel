// ptytest.c -- /dev/ptmx + /dev/pts/N: allocation, the master<->slave
// byte path, TCGETS distinguishing a tty, and ^C -> SIGINT.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/wait.h>

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGPTN 0x80045430
#define ECHO_BIT 0000010   // c_lflag ECHO

extern int ioctl(int fd, unsigned long req, void *arg);

// kernel struct termios_k: 4 x u32, then c_line + c_cc[19] = 36 bytes.
struct termios_k { unsigned iflag, oflag, cflag, lflag; unsigned char line, cc[19]; };

static int streq4(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) { if (a[i] != b[i]) { return 0; } }
    return 1;
}

int main(void) {
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0) { printf("[ptytest] FAILED: open ptmx %d\n", m); return 1; }

    int n = -1;
    if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) { printf("[ptytest] FAILED: TIOCGPTN\n"); return 1; }

    char path[16] = "/dev/pts/";
    int k = 9;
    if (n >= 10) { path[k++] = '0' + n / 10; }
    path[k++] = '0' + n % 10;
    path[k] = 0;

    int s = open(path, O_RDWR);
    if (s < 0) { printf("[ptytest] FAILED: open %s -> %d\n", path, s); return 1; }

    struct termios_k tio;
    if (ioctl(s, TCGETS, &tio) != 0) { printf("[ptytest] FAILED: slave not a tty\n"); return 1; }

    int pp[2];
    if (pipe(pp) == 0) {
        struct termios_k junk;
        if (ioctl(pp[0], TCGETS, &junk) == 0) { printf("[ptytest] FAILED: a pipe claims to be a tty\n"); return 1; }
        close(pp[0]); close(pp[1]);
    }
    printf("[ptytest] TCGETS distinguishes tty from pipe passed\n");

    // Quiet the echo so the master only sees the child's reply.
    tio.lflag &= ~(unsigned)ECHO_BIT;
    ioctl(s, TCSETS, &tio);

    int kid = fork();
    if (kid == 0) {
        char b[64];
        int r = read(s, b, sizeof b);        // one canonical line
        if (r > 0) { write(s, b, r); }
        exit(0);
    }

    write(m, "hi\n", 3);
    char out[16]; int got = 0;
    while (got < 4) {
        int r = read(m, out + got, sizeof out - got);
        if (r <= 0) { break; }
        got += r;
    }
    wait(kid);
    if (got < 4 || !streq4(out, "hi\r\n")) {
        out[got < 15 ? got : 15] = 0;
        printf("[ptytest] FAILED: master read '%s' want 'hi\\r\\n'\n", out);
        return 1;
    }
    printf("[ptytest] master<->slave round-trip passed\n");

    // (^C / SIGINT-to-the-foreground-group is job control -- M2's, not
    // exercised here: this process is its own foreground group and would
    // just kill itself.)

    close(s); close(m);
    printf("[ptytest] ALL PASSED\n");
    return 0;
}
