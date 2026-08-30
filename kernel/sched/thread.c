// kernel/sched/thread.c -- thread allocation, kernel threads, and
// thread exit. Split out of the former kernel/process.c; the code is
// unchanged, only relocated.

#include "sched.h"
#include "thread_table.h"
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
    t->tid        = (p && !p->threads) ? p->pid : alloc_id();
    t->proc       = p;
    t->state      = THREAD_READY;
    t->stack_slot = -1;
    t->xstate = kmalloc(cpu_state_size());
    if (!t->xstate) { kfree(t); return 0; }
    cpu_state_init(t->xstate);
    signal_init_thread(t);
    if (p) {
        // Add to per-process thread table (if initialized)
        if (p->thread_table) {
            thread_table_insert(p->thread_table, t->tid, t);
        }

        // Keep proc_next for backward compat during transition
        t->proc_next = p->threads;
        p->threads   = t;
        p->refcount++;
    }
    return t;
}

struct thread *thread_alloc_kernel(void (*entry)(void)) {
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

    enqueue_ready(t);
    return t;
}

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
        struct thread **pp = &p->threads;
        while (*pp && *pp != t) { pp = &(*pp)->proc_next; }
        if (*pp) { *pp = t->proc_next; }
        t->proc_next = p->zombies;
        p->zombies   = t;

        waitq_wake_all(&p->join_waiters);
        proc_put(p);
    } else {
        t->proc_next = kzombies;
        kzombies     = t;
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
        // Already exited? Reclaim it here -- nothing is running on it.
        struct thread **pp = &p->zombies;
        while (*pp && (*pp)->tid != tid) { pp = &(*pp)->proc_next; }
        if (*pp) {
            struct thread *z = *pp;
            *pp = z->proc_next;
            if (out_code) { *out_code = z->exit_code; }
            thread_stack_free(p, z->stack_slot);
            pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
            kfree(z->xstate);
            kfree(z);
            return 0;
        }

        // Still running?
        struct thread *t = p->threads;
        while (t && t->tid != tid) { t = t->proc_next; }
        if (!t) { return -ESRCH; }

        if (waitq_sleep(&p->join_waiters, 0) == -EINTR) { return -EINTR; }
    }
}
