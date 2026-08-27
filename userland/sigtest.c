#include <unistd.h>
#include <stdio.h>
#include <signal.h>

static volatile int got;
static volatile int handler_arg;

static void handler(int sig) { got = 1; handler_arg = sig; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_flags   = 0;
    sa.sa_mask    = 0;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        printf("[sigtest] FAILED: sigaction\n");
        return 1;
    }

    // A value the handler must not disturb: proves sigreturn restored
    // the interrupted context rather than approximating it.
    volatile long canary = 0x0123456789ABCDEFL;
    got = 0;
    raise(SIGUSR1);

    if (!got) { printf("[sigtest] FAILED: handler did not run\n"); return 1; }
    if (handler_arg != SIGUSR1) {
        printf("[sigtest] FAILED: wrong signal %d\n", handler_arg); return 1;
    }
    if (canary != 0x0123456789ABCDEFL) {
        printf("[sigtest] FAILED: context clobbered\n"); return 1;
    }

    printf("[sigtest] handler + sigreturn passed\n");
    printf("[sigtest] ALL PASSED\n");
    return 0;
}
