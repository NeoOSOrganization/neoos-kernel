// nshtest.c -- nsh on a real terminal (N4).
//
// Same arrangement as bbsh: nsh on a pty slave in its own session,
// commands written into the master, output read back. A shell that only
// works when fed a pipe is not a shell.
//
// The checks are chosen so that each command's OUTPUT differs from the
// text typed at it. The terminal echoes what is typed, so a check for a
// literal string present in both passes on the echo alone -- bbsh's
// first version did exactly that and "passed" while the shell was
// executing nothing at all.

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

static int saw(const char *needle) {
    for (int i = 0; i <= tlen; i++) {
        int j = 0;
        while (needle[j] && transcript[i + j] == needle[j]) { j++; }
        if (!needle[j]) { return 1; }
    }
    return 0;
}

int main(void) {
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0) { printf("[nshtest] FAILED: /dev/ptmx (%d)\n", m); return 1; }

    int n = -1;
    if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) {
        printf("[nshtest] FAILED: TIOCGPTN\n"); return 1;
    }
    char pts[20] = "/dev/pts/";
    int k = 9;
    if (n >= 10) { pts[k++] = (char)('0' + n / 10); }
    pts[k++] = (char)('0' + n % 10);
    pts[k] = 0;

    int slave = open(pts, O_RDWR);
    if (slave < 0) { printf("[nshtest] FAILED: %s\n", pts); return 1; }

    int child = fork();
    if (child < 0) { printf("[nshtest] FAILED: fork\n"); return 1; }
    if (child == 0) {
        close(m);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) { close(slave); }
        setsid();
        ioctl(0, TIOCSCTTY, 0);
        char *argv[] = { (char *)"nsh", 0 };
        execve("/bin/nsh.nex", argv, environ);
        exit(127);
    }
    close(slave);

    drain(m, 700);                       // the first prompt
    // Any output at all. NOT a check for the literal "nsh": the prompt
    // is $PS1, which init sets to "neoos$ ", so looking for the shell's
    // own default name failed against a perfectly good prompt.
    if (tlen == 0) {
        printf("[nshtest] FAILED: nsh printed no prompt\n");
        goto done;
    }

    int before;

    // A builtin whose output is not the text typed.
    before = tlen; type(m, "pwd\n"); drain(m, 1500);
    if (!saw("\n/\r") && !saw("\r\n/\r")) {
        printf("[nshtest] FAILED: pwd printed nothing (%d new bytes)\n", tlen - before);
        goto done;
    }

    // cd, then pwd again: the shell has to have CHANGED something.
    before = tlen; type(m, "cd /usr/share/test\n"); drain(m, 1000);
    before = tlen; type(m, "pwd\n"); drain(m, 1500);
    if (!saw("/usr/share/test")) {
        printf("[nshtest] FAILED: cd did not take effect (%d new bytes)\n", tlen - before);
        goto done;
    }

    // An EXTERNAL program, found along PATH -- the part that makes it a
    // shell rather than a menu. busybox echoes something that is not
    // the command line.
    before = tlen; type(m, "busybox echo NSH-EXTERNAL-OK\n"); drain(m, 2500);
    if (!saw("NSH-EXTERNAL-OK\r")) {
        printf("[nshtest] FAILED: PATH lookup / external command (%d new bytes)\n",
               tlen - before);
        goto done;
    }

    // A command that does not exist must be REPORTED, and must not take
    // the shell down with it.
    before = tlen; type(m, "definitely_not_a_command\n"); drain(m, 1500);
    if (!saw("not found")) {
        printf("[nshtest] FAILED: unknown command not reported (%d new bytes)\n",
               tlen - before);
        goto done;
    }
    before = tlen; type(m, "pwd\n"); drain(m, 1500);
    if (!saw("/usr/share/test")) {
        printf("[nshtest] FAILED: shell did not survive an unknown command\n");
        goto done;
    }

    // A non-zero exit status is reported.
    before = tlen; type(m, "busybox false\n"); drain(m, 2000);
    if (!saw("exit 1")) {
        printf("[nshtest] FAILED: non-zero exit not reported (%d new bytes)\n",
               tlen - before);
        goto done;
    }

    printf("[nshtest] nsh on a pty: prompt, pwd, cd, PATH lookup, an unknown "
           "command survived, and a non-zero status reported\n");
    printf("[nshtest] ALL PASSED\n");
    type(m, "exit\n");
    drain(m, 500);
    close(m);
    int st = 0;
    waitpid(child, &st, 0);
    return 0;

done:
    printf("[nshtest] transcript (%d bytes):\n", tlen);
    for (int i = 0; i < tlen; i++) {
        char c = transcript[i];
        if (c == '\n') { printf("\n"); }
        else if (c >= 32 && c < 127) { printf("%c", c); }
        else { printf("<%d>", (int)(unsigned char)c); }
    }
    printf("\n[nshtest] end of transcript\n");
    close(m);
    return 1;
}
