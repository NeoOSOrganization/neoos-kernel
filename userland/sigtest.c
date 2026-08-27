#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <thread.h>
#include <errno.h>

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

static volatile int join_rc;
static volatile int victim_returned;
static volatile int victim_ready;
static thread_t     block_target;

// Never exits, so anything joining it blocks forever.
static void forever(void *arg) { (void)arg; for (;;) { yield(); } }

static void victim(void *arg) {
    (void)arg;
    victim_ready = 1;          // set immediately before blocking
    join_rc = thread_join(block_target, 0);
    victim_returned = 1;
    thread_exit(0);
}

static volatile int usr1_count;
static void counting_handler(int sig) { (void)sig; usr1_count++; }

// Blocks a thread inside thread_join, signals it, and reports whether
// the join returned.
//
// The handshake is a flag rather than a delay: waiting a fixed number of
// spins raced (a diagnostic printf was enough to change the outcome).
// There is still a window between victim_ready and the thread actually
// blocking, but that is harmless -- waitq_sleep checks for a pending
// signal BEFORE it blocks, so a signal that arrives early is not lost.
static int run_interrupt_case(int flags, int expect_return) {
    struct sigaction sa;
    sa.sa_handler = counting_handler;
    sa.sa_flags   = flags;
    sa.sa_mask    = 0;
    sigaction(SIGUSR1, &sa, 0);

    thread_create(&block_target, forever, 0);
    join_rc = 0;
    victim_returned = 0;
    victim_ready = 0;

    thread_t v = -1;
    if (thread_create(&v, victim, 0) != 0) {
        printf("[sigtest] FAILED: thread_create for victim\n");
        return 0;
    }
    while (!victim_ready) { yield(); }

    if (tkill(v, SIGUSR1) != 0) {
        printf("[sigtest] FAILED: tkill\n");
        return 0;
    }

    // Give the victim ample opportunity to run its handler and return.
    for (int i = 0; i < 200 && victim_returned != expect_return; i++) { yield(); }

    return victim_returned == expect_return;
}

static int check_eintr(void) {
    // No SA_RESTART: the interrupted thread_join must return -EINTR.
    usr1_count = 0;
    if (!run_interrupt_case(0, 1)) {
        printf("[sigtest] FAILED: interrupted join did not return\n");
        return 0;
    }
    if (join_rc != -EINTR) {
        printf("[sigtest] FAILED: join returned %d, want -EINTR\n", join_rc);
        return 0;
    }
    printf("[sigtest] EINTR passed\n");

    // With SA_RESTART the call is restarted, so the join stays blocked
    // and the victim never returns.
    if (!run_interrupt_case(SA_RESTART, 0)) {
        printf("[sigtest] FAILED: SA_RESTART did not restart the call\n");
        return 0;
    }
    printf("[sigtest] SA_RESTART passed\n");
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
    ok &= check_eintr();

    printf("[sigtest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
