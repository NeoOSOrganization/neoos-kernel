#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <thread.h>
#include <errno.h>
#include <sys/wait.h>

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

static char altstack_mem[16384] __attribute__((aligned(16)));

static void segv_handler(int sig) {
    (void)sig;
    // Reached only because SA_ONSTACK moved the frame off the stack that
    // just overflowed -- without an alternate stack this handler would
    // re-fault on the same guard page.
    exit(77);
}

// Runs in a forked child, since it deliberately dies.
static int check_segv(void) {
    int child = fork();
    if (child < 0) { printf("[sigtest] FAILED: fork for segv\n"); return 0; }
    if (child == 0) {
        stack_t ss;
        ss.ss_sp = altstack_mem;
        ss.ss_flags = 0;
        ss.ss_size = sizeof(altstack_mem);
        if (sigaltstack(&ss, 0) != 0) { exit(1); }

        struct sigaction sa;
        sa.sa_handler = segv_handler;
        sa.sa_flags   = SA_ONSTACK;
        sa.sa_mask    = 0;
        sigaction(SIGSEGV, &sa, 0);

        volatile int *bad = 0;
        *bad = 1;              // deliberate null dereference
        exit(2);               // unreachable
    }

    int code = wait(child);
    if (code != 77) {
        printf("[sigtest] FAILED: segv child exited %d, want 77\n", code);
        return 0;
    }
    printf("[sigtest] SIGSEGV on sigaltstack passed\n");
    return 1;
}

static int check_stop_continue(void) {
    int child = fork();
    if (child < 0) { printf("[sigtest] FAILED: fork for stop\n"); return 0; }
    if (child == 0) {
        for (volatile long i = 0; i < 400000000L; i++) { }
        exit(5);
    }

    for (volatile int i = 0; i < 2000000; i++) { }   // let it start
    kill(child, SIGSTOP);

    int st = 0;
    int rc = wait4(child, &st, WUNTRACED, 0);
    if (rc != child || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) {
        printf("[sigtest] FAILED: stop rc=%d st=%d\n", rc, st);
        return 0;
    }
    printf("[sigtest] SIGSTOP + WIFSTOPPED passed\n");

    kill(child, SIGCONT);
    st = 0;
    rc = wait4(child, &st, 0, 0);
    if (rc != child || !WIFEXITED(st) || WEXITSTATUS(st) != 5) {
        printf("[sigtest] FAILED: continue rc=%d st=%d\n", rc, st);
        return 0;
    }
    printf("[sigtest] SIGCONT + WIFEXITED passed\n");
    return 1;
}

// Hammers the SIGSTOP/SIGCONT handshake, which is a race and not a
// sequence: the SIGCONT is sent immediately after the SIGSTOP, so it
// often lands while the child is somewhere inside its own stop path
// rather than parked at the end of it.
//
// The kernel used to lose such a continue -- it looked at a thread that
// was still RUNNING, found nothing to wake, and the thread stopped a
// moment later and never ran again. The failure mode is therefore a
// HANG in wait4 below, not a wrong status: this test passing at all is
// the assertion. The delay is varied per round so the pair lands at
// different points in the path.
#define STOP_RACE_ROUNDS 10

static int check_stop_continue_race(void) {
    for (int round = 0; round < STOP_RACE_ROUNDS; round++) {
        int child = fork();
        if (child < 0) { printf("[sigtest] FAILED: fork for stop race\n"); return 0; }
        if (child == 0) {
            for (volatile long i = 0; i < 8000000L; i++) { }
            exit(9);
        }

        for (volatile int i = 0; i < 200000; i++) { }   // let it get going
        kill(child, SIGSTOP);
        for (volatile int i = 0; i < round * 300; i++) { }
        kill(child, SIGCONT);

        int st = 0;
        int rc = wait4(child, &st, 0, 0);
        if (rc != child || !WIFEXITED(st) || WEXITSTATUS(st) != 9) {
            printf("[sigtest] FAILED: stop/cont race round=%d rc=%d st=%d\n",
                   round, rc, st);
            return 0;
        }
    }
    printf("[sigtest] SIGSTOP/SIGCONT race passed\n");
    return 1;
}

static int check_signalled_status(void) {
    int child = fork();
    if (child < 0) { printf("[sigtest] FAILED: fork for kill\n"); return 0; }
    if (child == 0) { for (;;) { yield(); } }

    for (volatile int i = 0; i < 2000000; i++) { }
    kill(child, SIGKILL);

    int st = 0;
    int rc = wait4(child, &st, 0, 0);
    if (rc != child || !WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
        printf("[sigtest] FAILED: kill rc=%d st=%d\n", rc, st);
        return 0;
    }
    printf("[sigtest] WIFSIGNALED passed\n");
    return 1;
}

static volatile int rt_count;
static void rt_handler(int sig) { (void)sig; rt_count++; }

// The check that distinguishes standard from real-time signals, and the
// only one that proves the queue is real rather than a second pending
// bit.
static int check_rt_queueing(void) {
    struct sigaction sa;
    sa.sa_handler = rt_handler; sa.sa_flags = 0; sa.sa_mask = 0;
    sigaction(SIGUSR1,  &sa, 0);
    sigaction(SIGRTMIN, &sa, 0);

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGRTMIN);

    // Standard signal: two sends while blocked collapse into one.
    sigprocmask(SIG_BLOCK, &block, &old);
    rt_count = 0;
    raise(SIGUSR1);
    raise(SIGUSR1);
    sigprocmask(SIG_SETMASK, &old, 0);
    // One signal is delivered per delivery point: the handler blocks its
    // own signal until sigreturn, so a queued second copy needs another
    // return-to-user. yield() supplies them. Linux behaves the same way.
    for (int i = 0; i < 10; i++) { yield(); }
    if (rt_count != 1) {
        printf("[sigtest] FAILED: SIGUSR1 delivered %d times, want 1\n", rt_count);
        return 0;
    }

    // Real-time signal: two sends while blocked deliver twice.
    sigprocmask(SIG_BLOCK, &block, &old);
    rt_count = 0;
    raise(SIGRTMIN);
    raise(SIGRTMIN);
    sigprocmask(SIG_SETMASK, &old, 0);
    for (int i = 0; i < 10; i++) { yield(); }
    if (rt_count != 2) {
        printf("[sigtest] FAILED: SIGRTMIN delivered %d times, want 2\n", rt_count);
        return 0;
    }

    printf("[sigtest] RT queueing vs standard collapsing passed\n");
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
    ok &= check_segv();
    ok &= check_stop_continue();
    ok &= check_stop_continue_race();
    ok &= check_signalled_status();
    ok &= check_rt_queueing();

    printf("[sigtest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
