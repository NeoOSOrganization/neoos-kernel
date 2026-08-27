#include "signal.h"
#include "sched/proc.h"
#include "serial.h"
#include "errno.h"

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

// ---------------------------------------------------------------- sending

static struct sigqueue  sigqueue_pool[SIGQUEUE_POOL];
static struct sigqueue *sigqueue_free;
static struct spinlock  sigqueue_lock;

void signal_queue_init(void) {
    spin_init(&sigqueue_lock, LOCK_RANK_PROCESS, "sigqueue");
    sigqueue_free = 0;
    for (int i = 0; i < SIGQUEUE_POOL; i++) {
        sigqueue_pool[i].next = sigqueue_free;
        sigqueue_free = &sigqueue_pool[i];
    }
}

static struct sigqueue *sigqueue_alloc(void) {
    uint64_t f = spin_lock_irqsave(&sigqueue_lock);
    struct sigqueue *q = sigqueue_free;
    if (q) { sigqueue_free = q->next; q->next = 0; }
    spin_unlock_irqrestore(&sigqueue_lock, f);
    return q;
}

void sigqueue_release(struct sigqueue *q) {
    uint64_t f = spin_lock_irqsave(&sigqueue_lock);
    q->next = sigqueue_free;
    sigqueue_free = q;
    spin_unlock_irqrestore(&sigqueue_lock, f);
}

void siginfo_user(struct siginfo *out, int sig, int sender_pid) {
    for (unsigned i = 0; i < sizeof(*out); i++) { ((uint8_t *)out)[i] = 0; }
    out->si_signo = sig;
    out->si_code  = SI_USER;
    out->fields.kill.si_pid = sender_pid;
}

// Appends `info` in FIFO order. Only RT signals queue.
static int queue_append(struct sigqueue **head, struct siginfo *info) {
    struct sigqueue *q = sigqueue_alloc();
    if (!q) { return -EAGAIN; }
    q->info = *info;
    q->next = 0;
    while (*head) { head = &(*head)->next; }
    *head = q;
    return 0;
}

// An ignored signal is discarded at send time rather than pending
// forever -- except SIGKILL/SIGSTOP, which cannot be ignored.
static int signal_is_ignored(struct process *p, int sig) {
    if (sigmask_of(sig) & SIGSET_UNBLOCKABLE) { return 0; }
    void (*h)(int) = p->actions[sig].handler;
    return h == SIG_IGN ||
           (h == SIG_DFL && signal_default_action(sig) == SIGACT_IGN);
}

int signal_send_thread(struct thread *t, int sig, struct siginfo *info) {
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    struct process *p = t->proc;
    if (!p) { return -ESRCH; }
    if (signal_is_ignored(p, sig)) { return 0; }

    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    if (sig >= SIGRTMIN && info) {
        int rc = queue_append(&t->queued, info);
        if (rc != 0) { spin_unlock_irqrestore(&p->sig_lock, f); return rc; }
    }
    sigset_add(&t->pending, sig);
    spin_unlock_irqrestore(&p->sig_lock, f);

    signal_wake_for_delivery(t);
    return 0;
}

int signal_send_process(struct process *p, int sig, struct siginfo *info) {
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    if (signal_is_ignored(p, sig)) { return 0; }

    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    if (sig >= SIGRTMIN && info) {
        int rc = queue_append(&p->queued, info);
        if (rc != 0) { spin_unlock_irqrestore(&p->sig_lock, f); return rc; }
    }
    sigset_add(&p->pending, sig);
    spin_unlock_irqrestore(&p->sig_lock, f);

    // Deliver to any one thread that does not block it; prefer one that
    // is already awake so a sleeping thread is not woken unnecessarily.
    struct thread *target = 0;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        if (!sigset_test(t->blocked, sig)) {
            target = t;
            if (t->state != THREAD_BLOCKED) { break; }
        }
    }
    if (target) { signal_wake_for_delivery(target); }
    return 0;
}

// Real body arrives with interruptible waits.
void signal_wake_for_delivery(struct thread *t) { (void)t; }

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
