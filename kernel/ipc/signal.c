#include "ipc/signal.h"
#include "sched/proc.h"
#include "dev/serial.h"
#include "errno.h"
#include "mm/paging.h"
#include "arch/isr.h"
#include "arch/cpu.h"

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

int signal_next_pending_in(struct thread *t, sigset_t_k want) {
    struct process *p = t->proc;
    sigset_t_k ready = (t->pending | (p ? p->pending : 0)) & want;
    if (!ready) { return 0; }
    for (int sig = 1; sig < NSIG; sig++) {
        if (ready & sigmask_of(sig)) { return sig; }
    }
    return 0;
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
    spin_init(&sigqueue_lock, LOCK_RANK_SIGQUEUE, "sigqueue");
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

// A stopped thread never reaches a delivery point, so SIGCONT has to
// act when it is SENT rather than when it would be delivered. A stop
// and a pending continue also cancel each other, as POSIX requires.
static void signal_stop_cont_interlock(struct process *p, int sig) {
    if (sig == SIGCONT) { signal_do_continue(p); }
    if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        sigset_del(&p->pending, SIGCONT);
    }
}

int signal_send_thread(struct thread *t, int sig, struct siginfo *info) {
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    struct process *p = t->proc;
    if (!p) { return -ESRCH; }
    signal_stop_cont_interlock(p, sig);
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
    signal_stop_cont_interlock(p, sig);
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

// A thread with a deliverable signal must reach a delivery point. If it
// is blocked in an interruptible sleep, wake it: waitq_sleep sees the
// pending signal and returns -EINTR.
void signal_wake_for_delivery(struct thread *t) {
    // rt_sigtimedwait sleeps on sig_waiters with the signal BLOCKED, so
    // signal_next_deliverable would not see it. Wake that queue
    // unconditionally and let the sleeper re-check.
    if (t->proc) { waitq_wake_all(&t->proc->sig_waiters); }
    if (t->state == THREAD_BLOCKED && signal_next_deliverable(t)) {
        // waitq_remove first, so the thread is off the wait queue before
        // it can be picked up from a run queue. thread_wake then decides
        // who actually gets to enqueue it: a concurrent waitq_wake_one
        // may be doing the same thing to the same sleeper, and only one
        // of the two may win.
        waitq_remove(t);
        thread_wake(t, THREAD_BLOCKED);
    }
}

int signal_pending_any(struct thread *t) {
    return signal_next_deliverable(t) != 0;
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

// Records which signal killed the process, then terminates it.
// process_exit is idempotent, so a sibling that also takes a fatal
// signal does not clobber the first one's status.
void signal_terminate(struct process *p, int sig) {
    if (!p->exiting) { p->exit_signal = sig; }
    process_exit(0);
}

// Stop is process-wide: SIGSTOP marks every thread. Threads blocked in
// an interruptible sleep are woken so they reach a delivery point, where
// they park here instead of running a handler.
//
// The idle thread introduced by the threads milestone is what makes this
// safe: a process whose every thread stops no longer risks leaving the
// CPU with nothing runnable.
void signal_do_stop(struct thread *t, int sig) {
    struct process *p = t->proc;
    (void)sig;

    for (struct thread *o = p->threads; o; o = o->proc_next) {
        if (o != t && o->state != THREAD_ZOMBIE && o->state != THREAD_STOPPED) {
            sigset_add(&o->pending, SIGSTOP);
            signal_wake_for_delivery(o);
        }
    }

    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    if (!t->stopping) {
        // A SIGCONT overtook us between taking the signal and getting
        // here. Nothing to do: the continue already cleared the stop.
        spin_unlock_irqrestore(&p->sig_lock, f);
        return;
    }
    p->stopped_count++;
    // The process counts as stopped only when EVERY thread has parked;
    // reporting a half-complete group stop to wait4 would be a lie.
    int all_stopped = 1;
    for (struct thread *o = p->threads; o; o = o->proc_next) {
        if (o != t && o->state != THREAD_STOPPED && o->state != THREAD_ZOMBIE) {
            all_stopped = 0;
            break;
        }
    }
    if (all_stopped) { p->stop_reported = 0; }
    spin_unlock_irqrestore(&p->sig_lock, f);

    // BEFORE parking, not after: a thread that has already published
    // THREAD_STOPPED can be preempted before it reaches this line and
    // then never runs again until SIGCONT -- leaving the parent asleep
    // in wait4 waiting for the very stop report that is stuck here.
    if (all_stopped) {
        struct process *parent = proc_find(p->parent_pid);
        if (parent) { waitq_wake_all(&parent->child_waiters); }
    }

    // The commit, and the whole point of `stopping`.
    //
    // THE LOST WAKEUP THIS REPLACES: the old code flipped
    // t->state = THREAD_STOPPED behind a bare cli/sti while
    // signal_do_continue tested that same field from another CPU. A
    // SIGCONT landing after the thread had decided to stop but before
    // the flip saw a RUNNING thread, found nothing to wake, and the
    // thread parked a moment later and never came back (observed as
    // sigtest hanging after "SIGSEGV on sigaltstack"). Ordering the two
    // the other way is no better: the continue then requeues a thread
    // that is still running toward schedule().
    //
    // Publishing THREAD_STOPPED under the same lock that
    // signal_do_continue clears `stopping` under makes the two
    // exclusive. Either the continue gets here first, and this
    // abandons the stop; or this publishes the state, and the continue
    // is guaranteed to see THREAD_STOPPED and wake it. thread_wake()
    // then handles the remaining hazard -- that the thread has not yet
    // finished leaving its CPU.
    f = spin_lock_irqsave(&p->sig_lock);
    if (!t->stopping) {
        spin_unlock_irqrestore(&p->sig_lock, f);
        return;
    }
    t->stopping = 0;
    t->state    = THREAD_STOPPED;
    spin_unlock_irqrestore(&p->sig_lock, f);

    schedule();
    // Resumed by SIGCONT.
}

void signal_do_continue(struct process *p) {
    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    sigset_del(&p->pending, SIGSTOP);
    p->stopped_count = 0;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        // Clearing the pending bit cancels a stop that has not been
        // taken yet; clearing `stopping` cancels one that has been
        // taken but not yet committed. Together they cover every point
        // a thread can be at on its way into signal_do_stop.
        sigset_del(&t->pending, SIGSTOP);
        t->stopping = 0;
    }
    spin_unlock_irqrestore(&p->sig_lock, f);

    // Outside the lock: thread_wake can spin waiting for a thread to
    // finish leaving its CPU, and it takes a run queue lock.
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        thread_wake(t, THREAD_STOPPED);
    }
}

// ---------------------------------------------------------------- delivery

#define USER_CS 0x3B
#define USER_SS 0x33

// Laid out exactly in sigframe.asm's pop order, ending with the five
// qwords iretq consumes. THESE MUST MATCH; nothing checks it but eyes.
struct iret_ctx {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};
extern void sigreturn_to_user(struct iret_ctx *ctx) __attribute__((noreturn));

// Clears `sig` from whichever pending set holds it and pops any queued
// payload into *out. A standard signal with no queue entry gets a
// synthesised SI_USER.
void signal_take_pending(struct thread *t, int sig, struct siginfo *out) {
    struct process *p = t->proc;
    uint64_t f = spin_lock_irqsave(&p->sig_lock);

    // The atomic point of "this thread is going to stop". Once the
    // signal leaves the pending set, a SIGCONT arriving afterwards can
    // no longer cancel the stop by clearing the pending bit -- there is
    // no bit left to clear -- so it has to cancel this flag instead.
    // Both happen under sig_lock, which is what makes them exclusive.
    if (signal_default_action(sig) == SIGACT_STOP) { t->stopping = 1; }

    struct sigqueue **head = 0;
    if (sigset_test(t->pending, sig)) {
        sigset_del(&t->pending, sig);
        head = &t->queued;
    } else {
        sigset_del(&p->pending, sig);
        head = &p->queued;
    }

    struct sigqueue *found = 0, **pp = head;
    while (*pp) {
        if ((*pp)->info.si_signo == sig) { found = *pp; *pp = found->next; break; }
        pp = &(*pp)->next;
    }
    // An RT signal with more payloads queued stays pending, so the next
    // delivery point picks up the next one.
    if (found && sig >= SIGRTMIN) {
        for (struct sigqueue *q = *head; q; q = q->next) {
            if (q->info.si_signo == sig) {
                if (head == &t->queued) { sigset_add(&t->pending, sig); }
                else                    { sigset_add(&p->pending, sig); }
                break;
            }
        }
    }
    spin_unlock_irqrestore(&p->sig_lock, f);

    if (found) {
        *out = found->info;
        sigqueue_release(found);
    } else {
        siginfo_user(out, sig, 0);
    }
}

// Copies the interrupted context into the frame and pushes a complete
// rt_sigframe onto the user stack. Returns the new user RSP, or 0 if
// the stack could not take the frame.
static uint64_t build_frame(struct thread *t, struct siginfo *info,
                            struct sigcontext_64 *sc, sigset_t_k oldmask,
                            const struct k_sigaction *ka) {
    uint64_t sp = sc->rsp;

    // SA_ONSTACK: run on the alternate stack if one is installed and we
    // are not already on it. This is what makes a SIGSEGV handler
    // survivable after a stack overflow -- without it the handler
    // re-faults on the same guard page.
    uint64_t alt = (uint64_t)(uintptr_t)t->altstack.ss_sp;
    if ((ka->flags & SA_ONSTACK) && t->altstack.ss_size &&
        !(sp >= alt && sp < alt + t->altstack.ss_size)) {
        sp = alt + t->altstack.ss_size;
    }

    // FP area first, on its own 64-byte alignment. Sized at run time:
    // 512 with FXSAVE, 832 with x87+SSE+AVX. The +4 is Linux's trailing
    // FP_XSTATE_MAGIC2.
    uint32_t fpsize = cpu_state_size();
    sp -= fpsize + 4;
    sp &= ~(uint64_t)(SIGFRAME_FPSTATE_ALIGN - 1);
    uint64_t fpaddr = sp;

    // Then the frame below it, placed so the handler sees rsp % 16 == 8
    // exactly as if reached by `call`.
    sp -= sizeof(struct rt_sigframe);
    sp &= ~0xFULL;
    sp -= 8;

    struct rt_sigframe *fr = (struct rt_sigframe *)(uintptr_t)sp;
    if (!user_range_writable(sp, (fpaddr + fpsize + 4) - sp)) {
        return 0;
    }
    for (unsigned i = 0; i < sizeof(*fr); i++) { ((uint8_t *)fr)[i] = 0; }
    for (uint32_t i = 0; i < fpsize + 4; i++) {
        ((uint8_t *)(uintptr_t)fpaddr)[i] = 0;
    }

    fr->pretcode               = (uint64_t)(uintptr_t)ka->restorer;
    fr->uc.uc_mcontext         = *sc;
    fr->uc.uc_sigmask          = oldmask;
    fr->uc.uc_stack            = t->altstack;
    fr->uc.uc_mcontext.fpstate = fpaddr;
    cpu_state_save((void *)(uintptr_t)fpaddr);

    // Linux-shaped software-reserved block, so anything that parses the
    // frame finds what it expects.
    struct fpx_sw_bytes *sw = (struct fpx_sw_bytes *)(uintptr_t)(fpaddr + 464);
    sw->magic1        = FP_XSTATE_MAGIC1;
    sw->xstate_size   = fpsize;
    sw->extended_size = fpsize + 4;
    sw->xfeatures     = cpu_state_xcr0();
    *(uint32_t *)(uintptr_t)(fpaddr + fpsize) = FP_XSTATE_MAGIC2;
    if (info) { fr->info = *info; }

    return sp;
}

// Applies `sig` to the context in `sc`, entering the handler. Default
// actions that terminate never return.
static void deliver_one(struct thread *t, int sig, struct siginfo *info,
                        struct sigcontext_64 *sc) {
    struct process *p = t->proc;
    struct k_sigaction ka = p->actions[sig];

    if (ka.handler == SIG_IGN) { return; }
    if (ka.handler == SIG_DFL) {
        switch (signal_default_action(sig)) {
        case SIGACT_IGN:  return;
        case SIGACT_STOP: signal_do_stop(t, sig); return;
        case SIGACT_CONT: return;
        default:
            signal_terminate(p, sig);   // never returns
        }
    }

    // SA_RESTORER is mandatory on x86-64, as on Linux: the kernel never
    // injects a trampoline onto the user stack. lib/signal.c fills it in
    // for every sigaction(), and musl supplies its own.
    if (!(ka.flags & SA_RESTORER) || !ka.restorer) {
        signal_terminate(p, SIGSEGV);
    }

    sigset_t_k oldmask = t->blocked;
    // sigsuspend's temporary mask must not survive into the frame the
    // handler will sigreturn from, or the original mask is lost.
    if (t->in_sigsuspend) {
        oldmask = t->saved_blocked;
        t->in_sigsuspend = 0;
    }

    uint64_t sp = build_frame(t, info, sc, oldmask, &ka);
    if (!sp) {                       // unwritable stack: die, do not fault
        signal_terminate(p, SIGSEGV);
    }

    t->blocked |= ka.mask;
    if (!(ka.flags & SA_NODEFER)) { sigset_add(&t->blocked, sig); }
    t->blocked &= ~SIGSET_UNBLOCKABLE;

    if (ka.flags & SA_RESETHAND) { p->actions[sig].handler = SIG_DFL; }

    sc->rip = (uint64_t)(uintptr_t)ka.handler;
    sc->rsp = sp;
    sc->rdi = (uint64_t)sig;
    if (ka.flags & SA_SIGINFO) {
        struct rt_sigframe *fr = (struct rt_sigframe *)(uintptr_t)sp;
        sc->rsi = (uint64_t)(uintptr_t)&fr->info;
        sc->rdx = (uint64_t)(uintptr_t)&fr->uc;
    }
    sc->eflags &= ~(1ULL << 10);   // DF must be clear on entry to a SysV function
}

static void sc_from_syscall(struct sigcontext_64 *sc, struct syscall_frame *f,
                            int64_t retval) {
    for (unsigned i = 0; i < sizeof(*sc); i++) { ((uint8_t *)sc)[i] = 0; }
    sc->r8  = f->r8;  sc->r9  = f->r9;  sc->r10 = f->r10;
    sc->r12 = f->r12; sc->r13 = f->r13; sc->r14 = f->r14; sc->r15 = f->r15;
    sc->rdi = f->rdi; sc->rsi = f->rsi; sc->rbp = f->rbp;
    sc->rbx = f->rbx; sc->rdx = f->rdx;
    sc->rax = (uint64_t)retval;
    sc->rsp = f->user_rsp;
    sc->rip = f->rcx;       // syscall_entry saved user RIP in rcx
    sc->eflags = f->r11;    // and user RFLAGS in r11
    sc->r11 = f->r11;
    sc->rcx = f->rcx;
    sc->cs = USER_CS; sc->ss = USER_SS;
}

static void sc_to_syscall(struct syscall_frame *f, struct sigcontext_64 *sc) {
    f->r8  = sc->r8;  f->r9  = sc->r9;  f->r10 = sc->r10;
    f->r12 = sc->r12; f->r13 = sc->r13; f->r14 = sc->r14; f->r15 = sc->r15;
    f->rdi = sc->rdi; f->rsi = sc->rsi; f->rbp = sc->rbp;
    f->rbx = sc->rbx; f->rdx = sc->rdx;
    f->rcx = sc->rip;       // sysret takes user RIP from rcx
    f->r11 = sc->eflags;    // and RFLAGS from r11
    f->user_rsp = sc->rsp;
}

int64_t signal_deliver_from_syscall(struct syscall_frame *f, int64_t num,
                                    int64_t retval) {
    struct thread *t = current_thread();
    if (!t || !t->proc) { return retval; }

    int sig;
    // A loop, not an if: several signals can be pending at once, and
    // each handler entry stacks another frame on the user stack.
    while ((sig = signal_next_deliverable(t)) != 0) {
        struct siginfo info;
        signal_take_pending(t, sig, &info);

        struct k_sigaction ka = t->proc->actions[sig];
        int restart = (retval == -EINTR) && (ka.flags & SA_RESTART) &&
                      ka.handler != SIG_DFL && ka.handler != SIG_IGN;

        struct sigcontext_64 sc;
        sc_from_syscall(&sc, f, retval);
        if (restart) {
            sc.rip -= 2;              // back onto the `syscall` instruction (0F 05)
            sc.rax = (uint64_t)num;   // and put the number back in RAX
        }
        deliver_one(t, sig, &info, &sc);
        sc_to_syscall(f, &sc);
        retval = (int64_t)sc.rax;
    }
    return retval;
}

void signal_do_sigreturn(struct syscall_frame *f) {
    struct thread *t = current_thread();

    // The handler's own RSP is 8 past the frame (its `ret` popped
    // pretcode), so the frame starts one qword below.
    uint64_t sp = f->user_rsp - 8;
    struct rt_sigframe *fr = (struct rt_sigframe *)(uintptr_t)sp;
    if (!user_range_writable((uint64_t)(uintptr_t)fr, sizeof(*fr))) {
        signal_terminate(t->proc, SIGSEGV);
    }

    t->blocked  = fr->uc.uc_sigmask & ~SIGSET_UNBLOCKABLE;
    t->altstack = fr->uc.uc_stack;

    struct sigcontext_64 *sc = &fr->uc.uc_mcontext;

    // The frame is user memory and may have been altered; a misaligned
    // or unmapped fpstate would #GP the KERNEL in xrstor/fxrstor.
    if (sc->fpstate &&
        (sc->fpstate & (SIGFRAME_FPSTATE_ALIGN - 1)) == 0 &&
        user_range_writable(sc->fpstate, cpu_state_size())) {

        // XRSTOR also faults on a header naming features outside XCR0,
        // or with nonzero reserved bytes -- both of which a user program
        // can write into its own frame between handler entry and
        // sigreturn. Mask the header to what this kernel enabled before
        // it is ever fed to the CPU; without this any program can #GP
        // the kernel from a signal handler.
        if (cpu_state_size() > 512) {
            struct xstate_header *hdr =
                (struct xstate_header *)(uintptr_t)(sc->fpstate + 512);
            hdr->xstate_bv &= cpu_state_xcr0();
            hdr->xcomp_bv   = 0;          // uncompacted format only
            for (int i = 0; i < 6; i++) { hdr->reserved[i] = 0; }
        }
        cpu_state_restore((void *)(uintptr_t)sc->fpstate);
    }

    struct iret_ctx ctx;
    ctx.r15 = sc->r15; ctx.r14 = sc->r14; ctx.r13 = sc->r13; ctx.r12 = sc->r12;
    ctx.r11 = sc->r11; ctx.r10 = sc->r10; ctx.r9  = sc->r9;  ctx.r8  = sc->r8;
    ctx.rbp = sc->rbp; ctx.rdi = sc->rdi; ctx.rsi = sc->rsi; ctx.rdx = sc->rdx;
    ctx.rcx = sc->rcx; ctx.rbx = sc->rbx; ctx.rax = sc->rax;
    ctx.rip = sc->rip; ctx.cs  = USER_CS;
    // Force IF on and keep only the flags userland may choose.
    ctx.rflags = (sc->eflags & 0x3C7FD7ULL) | (1ULL << 9) | 2ULL;
    ctx.rsp = sc->rsp; ctx.ss = USER_SS;

    // `ctx` is a local on this thread's kernel stack, BELOW the syscall
    // frame at the top -- so the trampoline's `mov rsp, rdi` cannot
    // clobber anything still needed.
    sigreturn_to_user(&ctx);
}

static void sc_from_registers(struct sigcontext_64 *sc, struct registers *r) {
    for (unsigned i = 0; i < sizeof(*sc); i++) { ((uint8_t *)sc)[i] = 0; }
    sc->r8  = r->r8;  sc->r9  = r->r9;  sc->r10 = r->r10; sc->r11 = r->r11;
    sc->r12 = r->r12; sc->r13 = r->r13; sc->r14 = r->r14; sc->r15 = r->r15;
    sc->rdi = r->rdi; sc->rsi = r->rsi; sc->rbp = r->rbp; sc->rbx = r->rbx;
    sc->rdx = r->rdx; sc->rax = r->rax; sc->rcx = r->rcx;
    sc->rsp = r->rsp; sc->rip = r->rip; sc->eflags = r->rflags;
    sc->cs = USER_CS; sc->ss = USER_SS;
    sc->err = r->error_code; sc->trapno = r->vector_number;
}

static void sc_to_registers(struct registers *r, struct sigcontext_64 *sc) {
    r->r8  = sc->r8;  r->r9  = sc->r9;  r->r10 = sc->r10; r->r11 = sc->r11;
    r->r12 = sc->r12; r->r13 = sc->r13; r->r14 = sc->r14; r->r15 = sc->r15;
    r->rdi = sc->rdi; r->rsi = sc->rsi; r->rbp = sc->rbp; r->rbx = sc->rbx;
    r->rdx = sc->rdx; r->rax = sc->rax; r->rcx = sc->rcx;
    r->rsp = sc->rsp; r->rip = sc->rip; r->rflags = sc->eflags;
}

// Raises a synchronous fault signal and delivers it immediately: unlike
// an asynchronous signal there is nowhere to return to, since
// re-executing the faulting instruction would just fault again.
void signal_raise_fault(struct registers *regs, int sig, int code, uint64_t addr) {
    struct thread *t = current_thread();
    struct process *p = t ? t->proc : 0;
    if (!p) { return; }   // caller falls back to the kernel dump

    struct siginfo info;
    for (unsigned i = 0; i < sizeof(info); i++) { ((uint8_t *)&info)[i] = 0; }
    info.si_signo = sig;
    info.si_code  = code;
    info.fields.fault.si_addr = (void *)(uintptr_t)addr;

    // A fault signal that is blocked or ignored cannot be honoured:
    // POSIX leaves it undefined and every real kernel force-delivers the
    // default action. Otherwise the process spins re-faulting forever.
    if (sigset_test(t->blocked, sig) || p->actions[sig].handler == SIG_IGN) {
        signal_terminate(p, sig);
    }

    struct sigcontext_64 sc;
    sc_from_registers(&sc, regs);
    deliver_one(t, sig, &info, &sc);
    sc_to_registers(regs, &sc);
}

// Asynchronous delivery on the way back to ring 3. Without this a signal
// could never interrupt a compute loop -- only a thread that happened to
// make a syscall would notice one.
void signal_deliver_from_interrupt(struct registers *regs) {
    struct thread *t = current_thread();
    if (!t || !t->proc) { return; }

    int sig;
    while ((sig = signal_next_deliverable(t)) != 0) {
        struct siginfo info;
        signal_take_pending(t, sig, &info);
        struct sigcontext_64 sc;
        sc_from_registers(&sc, regs);
        deliver_one(t, sig, &info, &sc);
        sc_to_registers(regs, &sc);
    }
}
