#include "signal.h"
#include "sched/proc.h"
#include "serial.h"
#include "errno.h"
#include "mm/paging.h"
#include "cpu.h"

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

// Real body arrives with THREAD_STOPPED. Until then a stop signal
// terminates the process, which is the pre-signals behaviour.
void signal_do_stop(struct thread *t, int sig) {
    (void)sig;
    t->proc->exit_signal = SIGSTOP;
    process_exit(0);
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
static void signal_take_pending(struct thread *t, int sig, struct siginfo *out) {
    struct process *p = t->proc;
    uint64_t f = spin_lock_irqsave(&p->sig_lock);

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

    // FP area first, on its own 64-byte alignment.
    sp -= SIGFRAME_FPSTATE_SIZE;
    sp &= ~(uint64_t)(SIGFRAME_FPSTATE_ALIGN - 1);
    uint64_t fpaddr = sp;

    // Then the frame below it, placed so the handler sees rsp % 16 == 8
    // exactly as if reached by `call`.
    sp -= sizeof(struct rt_sigframe);
    sp &= ~0xFULL;
    sp -= 8;

    struct rt_sigframe *fr = (struct rt_sigframe *)(uintptr_t)sp;
    if (!user_range_writable(sp, (fpaddr + SIGFRAME_FPSTATE_SIZE) - sp)) {
        return 0;
    }
    for (unsigned i = 0; i < sizeof(*fr); i++) { ((uint8_t *)fr)[i] = 0; }
    for (unsigned i = 0; i < SIGFRAME_FPSTATE_SIZE; i++) {
        ((uint8_t *)(uintptr_t)fpaddr)[i] = 0;
    }

    fr->pretcode               = (uint64_t)(uintptr_t)ka->restorer;
    fr->uc.uc_mcontext         = *sc;
    fr->uc.uc_sigmask          = oldmask;
    fr->uc.uc_stack            = t->altstack;
    fr->uc.uc_mcontext.fpstate = fpaddr;
    fpu_save((void *)(uintptr_t)fpaddr);
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
            p->exit_signal = sig;
            process_exit(0);        // never returns
        }
    }

    // SA_RESTORER is mandatory on x86-64, as on Linux: the kernel never
    // injects a trampoline onto the user stack. lib/signal.c fills it in
    // for every sigaction(), and musl supplies its own.
    if (!(ka.flags & SA_RESTORER) || !ka.restorer) {
        p->exit_signal = SIGSEGV;
        process_exit(0);
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
        p->exit_signal = SIGSEGV;
        process_exit(0);
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
        t->proc->exit_signal = SIGSEGV;
        process_exit(0);
    }

    t->blocked  = fr->uc.uc_sigmask & ~SIGSET_UNBLOCKABLE;
    t->altstack = fr->uc.uc_stack;

    struct sigcontext_64 *sc = &fr->uc.uc_mcontext;

    // The frame is user memory and may have been altered; a misaligned
    // or unmapped fpstate would #GP the KERNEL in fxrstor.
    if (sc->fpstate &&
        (sc->fpstate & (SIGFRAME_FPSTATE_ALIGN - 1)) == 0 &&
        user_range_writable(sc->fpstate, SIGFRAME_FPSTATE_SIZE)) {
        fpu_restore((void *)(uintptr_t)sc->fpstate);
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
