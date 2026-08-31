// kernel/sched/thread.c -- thread allocation, kernel threads, and
// thread exit. Split out of the former kernel/process.c; the code is
// unchanged, only relocated.

#include "sched/sched.h"
#include "sched/thread_table.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "arch/tss.h"
#include "dev/serial.h"
#include "fs/vfs.h"
#include "elf.h"
#include "arch/cpu.h"
#include "arch/cpu_local.h"
#include "sync/waitq.h"
#include "errno.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);
extern void kernel_thread_trampoline(void);
extern void fork_trampoline(void);

// Wipes a freshly allocated physical block before it becomes a task's
// stack. pmm_alloc() hands back whatever the previous owner left there,
// and at boot that "previous owner" is GRUB, whose own code sits in the
// high end of the memory map it then reports back to us as available --
// so the first task spawned gets stack frames still full of GRUB's
// machine code. Beyond the obvious hygiene problem (a process could read
// it), leaving it there is catastrophic for performance under QEMU's
// TCG: a guest write to a physical page that still holds translated
// blocks takes the slow notdirty path, and QEMU only drops the blocks
// overlapping the bytes actually written -- so a stack whose hot slots
// never overlap the stale code stays on that path forever, running
// 150-2000x slower than normal RAM. Zeroing the whole block once
// evicts every stale block and settles the page for good.
// (elf_load() and paging.c's alloc_table_frame() already do the same
// for the frames they hand out.)
void zero_frames(uint64_t phys, unsigned order) {
    uint64_t *p = (uint64_t *)phys_to_virt(phys);
    uint64_t words = (PMM_FRAME_SIZE << order) / sizeof(uint64_t);
    for (uint64_t i = 0; i < words; i++) {
        p[i] = 0;
    }
}

struct thread  *current_thread(void) { return this_cpu()->current; }
struct process *current_proc(void) {
    struct thread *t = current_thread();
    return t ? t->proc : 0;
}

// thread->xstate is xsave/xrstor'd (or fxsave'd) directly out of its own
// allocation, and those #GP on a misaligned address -- 16 bytes for
// fxsave, 64 for xsave. heap.c's struct heap_page is 64-byte aligned
// specifically so every kmalloc slot satisfies both; see the comment
// there.
struct thread *thread_alloc(struct process *p) {
    struct thread *t = (struct thread *)kmalloc(sizeof(struct thread));
    if (!t) { return 0; }
    for (unsigned i = 0; i < sizeof(struct thread); i++) { ((uint8_t *)t)[i] = 0; }
    // A process's FIRST thread takes the pid as its tid, matching
    // Linux (main thread: tid == pid). Later threads draw fresh ids
    // from the same counter, so a tid never collides with a pid, and a
    // single-threaded process consumes exactly one id -- which is what
    // keeps pids stable across this refactor.
    // The tid is decided BEFORE proc_lock is taken: alloc_id() reaches
    // the pid allocator, whose own lock is rank LOCK_RANK_PROCTABLE --
    // the same rank as proc_lock -- so calling it under proc_lock is an
    // equal-rank inversion. The "first thread takes the pid" test reads
    // p->threads, but the first thread is only ever allocated from
    // spawn()/fork_task() on a process not yet visible to any other
    // CPU, so that read is not racy. Every later thread goes through
    // alloc_id() unconditionally.
    int tid = (p && !p->threads) ? p->pid : alloc_id();
    t->tid        = tid;
    t->proc       = p;
    t->state      = THREAD_READY;
    t->stack_slot = -1;
    t->xstate = kmalloc(cpu_state_size());
    if (!t->xstate) { kfree(t); return 0; }
    cpu_state_init(t->xstate);
    signal_init_thread(t);
    if (p) {
        // p->threads / p->refcount are shared with every other CPU that
        // might be running a sibling of this thread. proc_lock (rank
        // LOCK_RANK_PROCTABLE, the outermost) guards them here and at
        // every other mutation site -- matching what the signal paths in
        // proc.c already do when they walk p->threads.
        uint64_t f = spin_lock_irqsave(&proc_lock);

        // Add to per-process thread table (if initialized)
        if (p->thread_table) {
            thread_table_insert(p->thread_table, tid, t);
        }

        // Keep proc_next for backward compat during transition
        t->proc_next = p->threads;
        p->threads   = t;
        p->refcount++;

        spin_unlock_irqrestore(&proc_lock, f);
    }
    return t;
}

// Builds a kernel thread WITHOUT queueing it anywhere. Split out so a
// caller that wants the thread on a specific CPU -- or on no CPU at all
// -- can say so up front. Queueing on one CPU and moving it afterwards
// opens a window in which another CPU picks the thread up, runs it to
// completion and frees it, leaving the mover to push freed memory onto
// a run queue.
static struct thread *thread_build_kernel(void (*entry)(void)) {
    struct thread *t = thread_alloc(0);
    if (!t) {
        return 0;
    }

    uint64_t stack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    zero_frames(stack_phys, KERNEL_STACK_ORDER);
    uint64_t stack_top = (uint64_t)(uintptr_t)phys_to_virt(stack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = (uint64_t)entry;                            // popped by kernel_thread_entry_trampoline
    *(--sp) = (uint64_t)kernel_thread_entry_trampoline;    // context_switch's `ret` lands here
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->saved_rsp = (uint64_t)sp;
    t->kernel_stack_top = stack_top;
    t->kernel_stack_phys = stack_phys;
    return t;
}

struct thread *thread_alloc_kernel(void (*entry)(void)) {
    struct thread *t = thread_build_kernel(entry);
    if (t) { enqueue_ready(t); }
    return t;
}

// A kernel thread that is never placed on ANY run queue. For the idle
// threads: idle_init_for used to allocate one -- which queued it -- and
// then call dequeue_specific to take it off again. Between those two
// points another CPU can pick the idle thread up and run it, after
// which cpus[i].idle names a thread executing somewhere else entirely.
struct thread *thread_alloc_kernel_unqueued(void (*entry)(void)) {
    return thread_build_kernel(entry);
}

// A kernel thread placed directly on `cpu_index`, never visible on any
// other CPU's queue in between.
struct thread *thread_alloc_kernel_on(void (*entry)(void), int cpu_index) {
    struct thread *t = thread_build_kernel(entry);
    if (t) { enqueue_ready_on(cpu_index, t); }
    return t;
}

// Publishes this thread as reapable and then leaves the CPU for good.
//
// The publication necessarily happens while the thread is still running
// on its own kernel stack -- it cannot free that stack itself, which is
// the whole reason a reaper exists. So the contract is on the reaper's
// side instead: every site that frees a thread's kernel stack
// (idle_entry's kzombies drain, proc_reap, thread_join) must call
// thread_wait_off_cpu() first. On one CPU that was free -- no reaper
// could run until this thread had switched away -- but on four it is a
// use-after-free of both the stack and the struct thread.
void thread_exit_self(int code) {
    struct thread *t = current_thread();
    struct process *p = t->proc;

    __asm__ volatile ("cli");

    t->exit_code = code;
    t->state     = THREAD_ZOMBIE;

    if (p) {
        // Remove from per-process thread table (if initialized)
        if (p->thread_table) {
            thread_table_remove(p->thread_table, t);
        }

        // Unlink from the live list, then park on the zombie list. We
        // cannot free our own kernel stack -- we are running on it --
        // so thread_join or wait_for_pid's reap frees it later.
        //
        // proc_lock is scoped to JUST this list move. It must NOT be
        // held across proc_put() (switches CR3, frees the address space,
        // signals the parent) or schedule() (panics if any lock is
        // held). It is also NOT held across waitq_wake_all(): a waker
        // reaches thread_wait_off_cpu(), which spins until the target
        // thread has left its CPU -- and a sibling in process_exit()
        // waiting on proc_lock could be that target, which would
        // deadlock.
        uint64_t f = spin_lock_irqsave(&proc_lock);
        struct thread **pp = &p->threads;
        while (*pp && *pp != t) { pp = &(*pp)->proc_next; }
        if (*pp) { *pp = t->proc_next; }
        t->proc_next = p->zombies;
        p->zombies   = t;
        spin_unlock_irqrestore(&proc_lock, f);

        waitq_wake_all(&p->join_waiters);
        proc_put(p);
    } else {
        // idle_entry drains this same list under proc_lock. The `cli`
        // above is enough on one CPU and no protection at all on four:
        // another CPU's idle thread can be mid-drain right now. Scoped
        // tightly -- schedule() below panics if any lock is still held.
        uint64_t zf = spin_lock_irqsave(&proc_lock);
        t->proc_next = kzombies;
        kzombies     = t;
        spin_unlock_irqrestore(&proc_lock, zf);
    }

    __asm__ volatile ("sti");
    schedule();
    for (;;) {
        __asm__ volatile ("hlt"); // unreachable: schedule() never resumes a ZOMBIE
    }
}

// Terminates `t`. SIGKILL cannot be caught, blocked or ignored, so this
// is now just a send -- the delivery path does the rest. The threads
// milestone's kill_pending flag was a one-signal prototype of exactly
// this, and keeping both would mean two ways to kill a thread and two
// checks on the syscall return path.
void thread_kill(struct thread *t) {
    if (t->state == THREAD_ZOMBIE) { return; }
    struct siginfo info;
    siginfo_user(&info, SIGKILL, 0);
    signal_send_thread(t, SIGKILL, &info);
}

struct thread *thread_create(uint64_t entry, uint64_t arg) {
    struct process *p = current_proc();
    if (!p || p->exiting) { return 0; }

    uint64_t user_stack_top;
    int slot = thread_stack_alloc(p, &user_stack_top);
    if (slot < 0) { return 0; }

    struct thread *t = thread_alloc(p);
    if (!t) { thread_stack_free(p, slot); return 0; }
    t->stack_slot = slot;

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    if (!kstack_phys) {
        thread_stack_free(p, slot);
        return 0;
    }
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys)
                        + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    // Same layout spawn() builds, including the arg slot.
    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = arg;
    *(--sp) = user_stack_top;
    *(--sp) = entry;
    *(--sp) = (uint64_t)kernel_thread_trampoline;
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->saved_rsp         = (uint64_t)sp;
    t->kernel_stack_top  = kstack_top;
    t->kernel_stack_phys = kstack_phys;

    enqueue_ready(t);
    return t;
}

int thread_join(int tid, int *out_code) {
    struct process *p = current_proc();
    if (!p) { return -ESRCH; }
    if (tid == current_thread()->tid) { return -EDEADLK; }

    for (;;) {
        // Both list scans happen under proc_lock: without it, a sibling
        // exiting on another CPU can be mid-flight between the live list
        // and the zombie list -- unlinked from one, not yet linked to
        // the other -- and this scan misses it in both and wrongly
        // returns -ESRCH. (Observed as `[smptest] FAILED: thread_join`.)
        uint64_t f = spin_lock_irqsave(&proc_lock);

        // Already exited? Reclaim it here -- nothing is running on it.
        struct thread **pp = &p->zombies;
        while (*pp && (*pp)->tid != tid) { pp = &(*pp)->proc_next; }
        if (*pp) {
            struct thread *z = *pp;
            *pp = z->proc_next;
            int code = z->exit_code;
            spin_unlock_irqrestore(&proc_lock, f);

            if (out_code) { *out_code = code; }
            // Reaching p->zombies does not mean the thread has finished
            // with its kernel stack: thread_exit_self publishes it there
            // before calling schedule(). See idle_entry's drain. This --
            // and the frees -- must happen with proc_lock released:
            // thread_wait_off_cpu() spins on another CPU, and
            // thread_stack_free() takes p->mm_lock.
            thread_wait_off_cpu(z);
            thread_stack_free(p, z->stack_slot);
            pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
            kfree(z->xstate);
            kfree(z);
            return 0;
        }

        // Still running?
        struct thread *t = p->threads;
        while (t && t->tid != tid) { t = t->proc_next; }
        if (!t) { spin_unlock_irqrestore(&proc_lock, f); return -ESRCH; }

        // waitq_sleep() drops proc_lock atomically with the block and
        // re-acquires it before returning (see kernel/ipc/futex.c). The
        // saved flags `f` still describe the pre-acquire state.
        int rc = waitq_sleep(&p->join_waiters, &proc_lock);
        spin_unlock_irqrestore(&proc_lock, f);
        if (rc == -EINTR) { return -EINTR; }
    }
}
