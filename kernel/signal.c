#include "signal.h"
#include "sched/proc.h"
#include "serial.h"

int signal_default_action(int sig) {
    switch (sig) {
    case SIGCHLD:
        return SIGACT_IGN;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return SIGACT_STOP;
    case SIGCONT:
        return SIGACT_CONT;
    case SIGQUIT: case SIGILL: case SIGABRT: case SIGFPE:
    case SIGSEGV: case SIGBUS: case SIGSYS: case SIGTRAP:
        return SIGACT_CORE;   // NeoOS has no core dumps; behaves as TERM
    default:
        return SIGACT_TERM;   // includes every RT signal
    }
}

void signal_init_process(struct process *p) {
    spin_init(&p->sig_lock, LOCK_RANK_PROCESS, "signal");
    for (int i = 0; i < NSIG; i++) {
        p->actions[i].handler  = SIG_DFL;
        p->actions[i].flags    = 0;
        p->actions[i].restorer = 0;
        p->actions[i].mask     = 0;
    }
    p->pending       = 0;
    p->queued        = 0;
    p->exit_signal   = 0;
    p->stopped_count = 0;
}

void signal_init_thread(struct thread *t) {
    t->blocked       = 0;
    t->pending       = 0;
    t->queued        = 0;
    t->saved_blocked = 0;
    t->in_sigsuspend = 0;
    t->altstack.ss_sp    = 0;
    t->altstack.ss_flags = 0;
    t->altstack.ss_size  = 0;
}

int signal_next_deliverable(struct thread *t) {
    struct process *p = t->proc;
    // Thread-directed first, then process-directed; lowest number wins
    // within each. SIGKILL and SIGSTOP are never blockable, so they are
    // always visible here.
    sigset_t_k ready = t->pending & ~t->blocked;
    if (!ready && p) { ready = p->pending & ~t->blocked; }
    if (!ready) { return 0; }
    for (int sig = 1; sig < NSIG; sig++) {
        if (ready & sigmask_of(sig)) { return sig; }
    }
    return 0;
}

// ---------------------------------------------------------------- selftest

void signal_selftest(void) {
    sigset_t_k s = 0;
    sigset_add(&s, SIGSEGV);
    sigset_add(&s, SIGRTMIN);
    if (!sigset_test(s, SIGSEGV) || !sigset_test(s, SIGRTMIN)) {
        serial_write_string("[signal] selftest FAILED: add/test\n");
        return;
    }
    if (sigset_test(s, SIGKILL)) {
        serial_write_string("[signal] selftest FAILED: spurious member\n");
        return;
    }
    sigset_del(&s, SIGSEGV);
    if (sigset_test(s, SIGSEGV)) {
        serial_write_string("[signal] selftest FAILED: del\n");
        return;
    }
    // Bit 63 must be reachable: SIGRTMAX is signal 64.
    if (sigmask_of(SIGRTMAX) != (1ULL << 63)) {
        serial_write_string("[signal] selftest FAILED: SIGRTMAX bit\n");
        return;
    }
    if (signal_default_action(SIGCHLD)  != SIGACT_IGN  ||
        signal_default_action(SIGSTOP)  != SIGACT_STOP ||
        signal_default_action(SIGCONT)  != SIGACT_CONT ||
        signal_default_action(SIGTERM)  != SIGACT_TERM ||
        signal_default_action(SIGRTMIN) != SIGACT_TERM) {
        serial_write_string("[signal] selftest FAILED: default actions\n");
        return;
    }

    // Selection order, exercised on the current thread.
    struct thread *t = current_thread();
    sigset_t_k save_p = t->pending, save_b = t->blocked;
    t->pending = 0; t->blocked = 0;
    sigset_add(&t->pending, SIGUSR2);
    sigset_add(&t->pending, SIGUSR1);
    if (signal_next_deliverable(t) != SIGUSR1) {
        serial_write_string("[signal] selftest FAILED: lowest-first\n");
        t->pending = save_p; t->blocked = save_b; return;
    }
    sigset_add(&t->blocked, SIGUSR1);
    if (signal_next_deliverable(t) != SIGUSR2) {
        serial_write_string("[signal] selftest FAILED: blocked not skipped\n");
        t->pending = save_p; t->blocked = save_b; return;
    }
    sigset_add(&t->blocked, SIGUSR2);
    if (signal_next_deliverable(t) != 0) {
        serial_write_string("[signal] selftest FAILED: all blocked\n");
        t->pending = save_p; t->blocked = save_b; return;
    }
    t->pending = save_p; t->blocked = save_b;

    serial_write_string("[signal] selftest passed\n");
}

// kmain has no saved context -- schedule() abandons its stack -- so the
// selftest runs as a kernel thread, like waitq's.
static void signal_selftest_thread(void) {
    signal_selftest();
    thread_exit_self(0);
}

void signal_selftest_start(void) {
    thread_alloc_kernel(signal_selftest_thread);
}
