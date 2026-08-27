#include <unistd.h>
#include <stdio.h>
#include <signal.h>

static volatile int got;
static volatile int handler_arg;

static void handler(int sig) { got = 1; handler_arg = sig; }

static int check_masking(void) {
    struct sigaction sa;
    sa.sa_handler = handler; sa.sa_flags = 0; sa.sa_mask = 0;
    sigaction(SIGUSR2, &sa, 0);

    sigset_t block, old, pend;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, &old);

    got = 0;
    raise(SIGUSR2);
    if (got) { printf("[sigtest] FAILED: blocked signal delivered\n"); return 0; }

    sigemptyset(&pend);
    sigpending(&pend);
    if (!sigismember(&pend, SIGUSR2)) {
        printf("[sigtest] FAILED: sigpending missed it\n"); return 0;
    }

    // Delivery must happen on the sigprocmask syscall's OWN return path:
    // that is the first delivery point reached after unblocking.
    sigprocmask(SIG_SETMASK, &old, 0);
    if (!got) { printf("[sigtest] FAILED: unblock did not deliver\n"); return 0; }

    printf("[sigtest] blocking + sigpending passed\n");
    return 1;
}

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

    int ok = 1;
    ok &= check_masking();

    printf("[sigtest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
