// kernel/sched/sched.c -- run queue, context switching, and the idle
// thread. Split out of the former kernel/process.c; the code is
// unchanged, only relocated.

#include "sched.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../mm/heap.h"
#include "../tss.h"
#include "../serial.h"
#include "../fs/vfs.h"
#include "../elf.h"
#include "../cpu.h"
#include "../cpu_local.h"
#include "../waitq.h"
#include "../errno.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);
extern void kernel_thread_trampoline(void);
extern void fork_trampoline(void);

struct thread *ready_head;
struct thread *ready_tail;

void enqueue_ready(struct thread *t) {
    t->next = 0;
    if (ready_tail) {
        ready_tail->next = t;
    } else {
        ready_head = t;
    }
    ready_tail = t;
}

struct thread *dequeue_ready(void) {
    struct thread *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (!ready_head) {
            ready_tail = 0;
        }
        t->next = 0;
    }
    return t;
}

void thread_enqueue_ready(struct thread *t) { enqueue_ready(t); }

// Removes `t` from the ready queue wherever it sits. Only used by
// idle_init, which has to un-enqueue the idle thread that
// thread_alloc_kernel just queued.
void dequeue_specific(struct thread *t) {
    struct thread **pp = &ready_head;
    struct thread *prev = 0;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->next; }
    if (*pp) {
        *pp = t->next;
        if (ready_tail == t) { ready_tail = prev; }
    }
    t->next = 0;
}

// Runs whenever no other thread is ready. Having a real idle thread
// removes schedule()'s old "nothing ready, keep running whatever's
// current" special case for the blocked/dead-current cases.
// Kernel threads (proc == 0) have no parent to reap them and cannot
// free the stack they are running on, so they park here and the idle
// thread reclaims them. Without this every selftest thread would leak
// its 16KiB kernel stack for the life of the boot.
struct thread *kzombies;

static void idle_entry(void) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&proc_lock);
        struct thread *z = kzombies;
        kzombies = 0;
        spin_unlock_irqrestore(&proc_lock, f);
        while (z) {
            struct thread *next = z->proc_next;
            pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
            kfree(z->xstate);
            kfree(z);
            z = next;
        }
        __asm__ volatile ("sti; hlt");
    }
}

void idle_init(void) {
    struct thread *t = thread_alloc_kernel(idle_entry);
    // Reserved: idle threads are never a valid pid. thread_alloc()
    // already handed out id 0 here, since idle_init() runs before any
    // other allocation -- see next_id's initialiser.
    t->tid = 0;
    dequeue_specific(t);   // never on the ready queue; schedule() falls back to it
    this_cpu()->idle = t;
}

// Restores EFLAGS.IF to whatever it was on entry to schedule(). Split
// out because schedule() has three exits (no-task, same-task, and the
// far side of a context switch, possibly milliseconds later in a
// different task).
static inline void schedule_restore_if(uint64_t saved_flags) {
    if (saved_flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

void schedule(void) {
    // schedule() is NOT reentrant, and until this cli it ran with
    // interrupts enabled. Between `current = next` and the
    // context_switch() below, `current` already names the incoming
    // task while execution is still on the OUTGOING task's stack -- so
    // a timer interrupt landing in that window re-enters schedule()
    // with prev == the incoming task, and context_switch's
    // `mov [rdi], rsp` stamps the outgoing task's RSP into the
    // incoming task's saved_rsp. That task is then resumed on a stack
    // that isn't its own (observed: pid 6 resumed with an RSP pointing
    // into pid 5's kernel stack, faulting in syscall_dispatch's
    // epilogue with a garbage RBP, escalating to a double fault).
    //
    // The window was always there, but nothing hit it until fork()
    // made it easy to have several tasks doing nothing but yield(),
    // which keeps schedule() executing a large fraction of the time.
    //
    // IF is restored rather than unconditionally set because
    // timer_handler() calls schedule() from an interrupt gate with
    // IF already 0, and must return to the ISR with it still 0 -- the
    // iretq there is what re-enables it. `flags` is a local, so it
    // lives on this task's own kernel stack and is still correct
    // whenever this task is eventually resumed.
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    struct cpu *c = this_cpu();

    struct thread *next = dequeue_ready();
    if (!next) {
        struct thread *cur = c->current;
        if (cur && cur->state == THREAD_RUNNING) {
            schedule_restore_if(flags);
            return; // nothing else ready; keep running whatever's current
        }
        next = c->idle; // current is blocked or dead -- park on idle
    }

    struct thread *prev = c->current;
    if (prev && prev->state == THREAD_RUNNING && prev != c->idle) {
        prev->state = THREAD_READY;
        enqueue_ready(prev);
    }

    next->state = THREAD_RUNNING;
    c->current = next;
    c->tss->rsp0    = next->kernel_stack_top;
    c->kernel_stack = next->kernel_stack_top;

    // Always establish a definite CR3, even for a kernel-mode-only task
    // (pml4_phys == 0 -- falls back to the kernel's own never-freed
    // p4_table). Leaving CR3 unchanged in that case used to be harmless
    // (an exited process's now-zombie PML4 just leaked, unused-but-
    // intact memory), but now that task_exit() actually frees a
    // process's PML4 frame back to the allocator, a stale CR3 left
    // pointing at it could get silently reused and overwritten by the
    // very next pmm_alloc() -- corrupting the page table the CPU is
    // still actively translating through.
    uint64_t next_cr3 = (next->proc && next->proc->pml4_phys)
                      ? next->proc->pml4_phys
                      : (uint64_t)(uintptr_t)p4_table;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(next_cr3) : "memory");

    if (prev == next) {
        schedule_restore_if(flags);
        return;
    }

    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    if (prev) {
        cpu_state_save(prev->xstate);
    }
    cpu_state_restore(next->xstate);
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);

    // Reached only when THIS task is scheduled back in, which may be
    // much later; `flags` is the IF state from its own entry above.
    schedule_restore_if(flags);
}
