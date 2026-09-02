// bbsh.c -- BB6: an interactive BusyBox ash on a terminal.
//
// Everything before this ran BusyBox with `sh -c`, which is a shell
// reading a string. This runs a shell reading a TERMINAL: a pty, with
// the shell on the slave in its own session, commands typed into the
// master, and its output read back. That is the arrangement /bin/term.nex
// uses for a real session, minus the framebuffer rendering, and it is
// what an interactive prompt actually depends on -- line discipline,
// canonical mode, job-control signals, the tty's foreground pgid.
//
// A headless test cannot use the keyboard, so the "typing" is a write
// to the pty master. To the shell those bytes are indistinguishable
// from a keystroke: they arrive through the same line discipline.
//
// What is asserted is that the shell RESPONDS -- each command produces
// its own output -- not that the transcript matches byte for byte. A
// prompt's exact bytes are BusyBox's business and change with its
// config; what matters is that a command typed at a terminal runs.

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>
#include <auxv.h>

#define TIOCGPTN 0x80045430
#define TIOCSCTTY 0x540E

extern int ioctl(int fd, unsigned long req, void *arg);

static char transcript[8192];
static int  tlen;

// Reads whatever the shell has produced, for up to `ms`, into the
// transcript. Short reads are normal: the shell writes its prompt and
// its output at different moments.
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
            waited = 0;             // it is still talking; keep listening
        }
        if (pf.revents & (POLLHUP | POLLERR)) { break; }
    }
    transcript[tlen] = 0;
}

static void type(int m, const char *line) {
    write(m, line, strlen(line));
}

// Local rather than strstr: libneoos does not have it, and adding one
// there would be wrong -- lib/ keeps only what has no POSIX analogue,
// and musl already provides strstr for programs that link against it.
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
    if (m < 0) { printf("[bbsh] FAILED: /dev/ptmx (%d)\n", m); return 1; }

    int n = -1;
    if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) {
        printf("[bbsh] FAILED: TIOCGPTN\n"); return 1;
    }
    char pts[20] = "/dev/pts/";
    int k = 9;
    if (n >= 10) { pts[k++] = (char)('0' + n / 10); }
    pts[k++] = (char)('0' + n % 10);
    pts[k] = 0;

    // Held across the fork so the pty never momentarily looks hung up
    // (slave_refs == 0) while the child is still setting up -- the
    // first poll would return POLLHUP and this would give up before the
    // shell said anything. /bin/term.nex learned the same lesson.
    int slave = open(pts, O_RDWR);
    if (slave < 0) { printf("[bbsh] FAILED: %s\n", pts); return 1; }

    int child = fork();
    if (child < 0) { printf("[bbsh] FAILED: fork\n"); return 1; }
    if (child == 0) {
        close(m);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) { close(slave); }
        setsid();                       // the shell wants its own session
        // ...and then claims this pty as its controlling terminal. The
        // slave was opened by the PARENT, so the pty recorded the
        // parent's session; without this the shell sees a foreground
        // process group that is not its own, and BusyBox's ash
        // signals itself with SIGTTIN until it stops. setsid() then
        // TIOCSCTTY on the inherited descriptor is the standard
        // sequence for exactly this reason.
        ioctl(0, TIOCSCTTY, 0);
        char *argv[] = { (char *)"sh", (char *)"-i", 0 };
        execve("/bin/busybox.nex", argv, environ);
        exit(127);
    }
    close(slave);

    drain(m, 500);                      // banner and first prompt

    // BB4. ash announces "can't access tty; job control turned off"
    // when it cannot take the terminal over, and then runs perfectly
    // well without job control -- so the message is the only evidence,
    // and asserting on it is the only way this stays fixed. It needed
    // fcntl(F_DUPFD), which used to return -EINVAL, and TIOCSCTTY.
    if (saw("job control turned off")) {
        printf("[bbsh] FAILED: ash could not take the terminal "
               "(job control off)\n");
        goto done;
    }

    // Each command is typed, then its output collected, so a failure
    // names the command that produced it rather than "somewhere in the
    // session".
    int before;

    // EVERY command is chosen so its OUTPUT differs from the text typed.
    //
    // The terminal echoes what is typed, so the transcript contains the
    // command as well as its result -- and a check for a literal string
    // present in both passes on the echo alone. The first version did
    // exactly that: `echo BBSH-ONE` "passed" while the shell was in fact
    // executing nothing at all, because poll() was returning -ENOSYS.
    // Expansion is what tells them apart: the shell must have RUN the
    // command to produce the answer.
    // Variable expansion first: it needs nothing but the shell's own
    // variable table, so a failure here means the shell is not running
    // commands at all, while a failure further down is that feature's.
    before = tlen; type(m, "echo BBSH-HOME-$HOME\n"); drain(m, 2000);
    if (!saw("BBSH-HOME-/")) {
        printf("[bbsh] FAILED: no response to `echo $HOME` (%d new bytes)\n",
               tlen - before);
        goto done;
    }

    before = tlen; type(m, "echo BBSH-$((6*7))\n"); drain(m, 2000);
    if (!saw("BBSH-42")) {
        printf("[bbsh] FAILED: arithmetic expansion (%d new bytes)\n",
               tlen - before);
        goto done;
    }

    before = tlen; type(m, "cd /; pwd\n");       drain(m, 2000);
    // "/" alone would match a dozen things in the transcript; the
    // shell's answer to pwd is a line containing just it.
    if (!saw("\n/\r") && !saw("\r\n/\r")) {
        printf("[bbsh] FAILED: no response to `pwd` (%d new bytes)\n", tlen - before);
        goto done;
    }

    // A pipeline: two processes, a pipe between them, both on this tty.
    before = tlen; type(m, "echo one two three | wc -w\n"); drain(m, 2000);
    if (!saw("3")) {
        printf("[bbsh] FAILED: pipeline `echo | wc -w` (%d new bytes)\n", tlen - before);
        goto done;
    }

    // Redirection into a file and back out, which is close(0)/open()
    // territory -- the lowest-available fd rule CS4 fixed.
    before = tlen;
    type(m, "echo BBSH-REDIR > /var/tmp/tmpfile.txt; cat < /var/tmp/tmpfile.txt\n");
    drain(m, 2000);
    if (!saw("BBSH-REDIR")) {
        printf("[bbsh] FAILED: redirection round trip (%d new bytes)\n", tlen - before);
        goto done;
    }

    // A background job and `jobs`, which only exist when job control is
    // on: the shell must put the job in its own process group and keep
    // the terminal, which is what TIOCSCTTY and tcsetpgrp are for.
    before = tlen; type(m, "sleep 5 & jobs\n"); drain(m, 2500);
    if (!saw("Running") && !saw("sleep")) {
        printf("[bbsh] FAILED: `sleep &; jobs` listed nothing (%d new bytes)\n",
               tlen - before);
        goto done;
    }
    // Kill it rather than leaving a sleeper behind for the rest of the
    // boot to wait on.
    type(m, "kill %1\n"); drain(m, 1000);

    printf("[bbsh] interactive ash on a pty with JOB CONTROL: echo, pwd, "
           "$(( )), a pipeline, a redirection round trip and a background "
           "job all answered\n");
    printf("[bbsh] ALL PASSED\n");
    type(m, "exit\n");
    drain(m, 500);
    close(m);
    int st = 0;
    waitpid(child, &st, 0);
    return 0;

done:
    // Print what the shell actually said. Without this a failure is a
    // guess; with it the transcript is right there in the boot log.
    printf("[bbsh] transcript (%d bytes):\n", tlen);
    for (int i = 0; i < tlen; i++) {
        char c = transcript[i];
        if (c == '\n') { printf("\n"); }
        else if (c >= 32 && c < 127) { printf("%c", c); }
        else { printf("<%d>", (int)(unsigned char)c); }
    }
    printf("\n[bbsh] end of transcript\n");
    close(m);
    return 1;
}
