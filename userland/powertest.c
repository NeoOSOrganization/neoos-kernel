// powertest -- asks init to power off (default) or reboot (--reboot)
// the way the desktop shell does: kill(1, SIGUSR2) / kill(1, SIGTERM).
// First forks a child that IGNORES SIGTERM, so init's SIGKILL fallback
// after the grace period is exercised too. `make powertest` and
// `make powertest-reboot` read the result from the serial log.
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int reboot = argc > 1 && !strcmp(argv[1], "--reboot");
    int p[2];
    pipe(p);
    if (fork() == 0) {
        signal(SIGTERM, SIG_IGN);
        write(p[1], "r", 1);
        for (;;) { pause(); }                   // only SIGKILL ends this
    }
    char c;
    read(p[0], &c, 1);                          // the child is ignoring SIGTERM now
    printf("powertest: asking init to %s\n", reboot ? "reboot" : "power off");
    fflush(stdout);
    if (kill(1, reboot ? SIGTERM : SIGUSR2) != 0) { printf("powertest: FAILED kill(1)\n"); return 1; }
    for (;;) { pause(); }                       // init's SIGTERM ends us
}
