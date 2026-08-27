#ifndef NEOOS_PROCESS_H
#define NEOOS_PROCESS_H

#include <stdint.h>
#include "../cpu.h"
#include "../lock.h"
#include "../waitq.h"
#include "../fs/vfs.h"
#include "../mm/pmm.h"

// 16 entries indexed DIRECTLY by fd. /dev/CONSOLE is a real vnode
// opened on 0, 1 and 2 at process creation, so the fd IS the index.
// The table belongs to the PROCESS: threads share it.
#define MAX_OPEN_FILES 16
#define KERNEL_STACK_ORDER 2 // 4 frames = 16KiB

// Mirrors syscall_entry.asm's saved-register block exactly, in
// increasing-address order (the reverse of push order, since the last
// register pushed ends up at the lowest address). A pointer to the
// base of this block is passed into syscall_dispatch as its 6th
// argument, letting fork() copy a caller's full user-mode context and
// exec() overwrite its own return RIP/RSP in place.
struct syscall_frame {
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11;       // user RFLAGS
    uint64_t rcx;       // user RIP
    uint64_t user_rsp;
};

enum thread_state { THREAD_UNUSED, THREAD_READY, THREAD_RUNNING,
                    THREAD_BLOCKED, THREAD_ZOMBIE };

struct file_descriptor {
    int in_use;
    struct vnode *vn;   // reference held; released by close/process exit
    uint32_t position;  // per-fd, NOT shared across fork -- see docs/stdlib.md
    int writable;
};

struct thread;

struct process {
    int pid, parent_pid;
    // One reference per LIVE thread. When it reaches zero the address
    // space is freed and the process becomes a zombie carrying only
    // its exit code; the struct itself outlives the address space and
    // is freed by wait_for_pid's reap.
    uint32_t refcount;
    uint64_t pml4_phys;             // 0 = shares the kernel address space
    struct file_descriptor files[MAX_OPEN_FILES];
    struct thread *threads;         // live threads, via thread->proc_next
    struct thread *zombies;         // exited, unjoined; freed at reap
    uint16_t stack_slots;           // bitmap of live thread user stacks
    int exiting;
    int exit_code;
    enum { PROC_ALIVE, PROC_ZOMBIE } state;
    struct waitq exit_waiters;      // threads blocked in wait_for_pid
    struct waitq join_waiters;      // threads blocked in thread_join
    struct process *next;           // global process list
};

struct thread {
    int tid;                        // 0 == an idle thread (never a pid)
    struct process *proc;           // 0 for the pre-process idle thread
    enum thread_state state;
    uint64_t saved_rsp;
    uint64_t kernel_stack_top;
    uint64_t kernel_stack_phys;
    int stack_slot;                 // -1 for kernel-only threads
    int kill_pending;
    int exit_code;
    struct waitq *blocked_on;
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    struct thread *proc_next;       // sibling / zombie list link
    struct thread *next;            // ready-queue link
};

void process_init(void);
void schedule(void);

struct thread  *current_thread(void);
struct process *current_proc(void);

// Creates a thread that starts executing `entry` directly in ring 0,
// sharing the kernel's own address space. Used by selftests and by the
// idle thread; real processes come from spawn() instead.
struct thread *thread_alloc_kernel(void (*entry)(void));

// Run-queue insertion, shared with waitq.c's wake path.
void thread_enqueue_ready(struct thread *t);

#define USER_STACK_PAGES 4
#define USER_STACK_TOP 0x0000700000000000ULL

#define MAX_THREADS_PER_PROC 16

// Each thread's user stack is USER_STACK_PAGES pages, followed (at
// LOWER addresses) by one unmapped guard page, so a stack overflow
// faults instead of silently writing into the next thread's stack.
// Slot 0 is the main thread at USER_STACK_TOP, so the single-threaded
// layout is unchanged -- except that the main thread now gains a guard
// page it never had.
#define THREAD_STACK_STRIDE ((uint64_t)(USER_STACK_PAGES + 1) * PMM_FRAME_SIZE)

static inline uint64_t thread_stack_top_for(int slot) {
    return USER_STACK_TOP - (uint64_t)slot * THREAD_STACK_STRIDE;
}

// Maps a fresh user stack for a new thread and marks its slot used.
// Returns the slot index and stores its top in *out_top, or -1 if the
// process is out of slots or memory.
int  thread_stack_alloc(struct process *p, uint64_t *out_top);
void thread_stack_free(struct process *p, int slot);

struct process *spawn(const char *path);

// Duplicates the calling THREAD into a new single-threaded process,
// sharing user frames copy-on-write. Returns the child's thread, or 0
// on failure (parent unaffected).
struct thread *fork_task(struct syscall_frame *frame);

int exec_task(const char *path, struct syscall_frame *frame);

// Ends the calling thread only. When it is the last live thread of its
// process, the address space is freed and waiters are woken.
void thread_exit_self(int code) __attribute__((noreturn));

// Ends the whole process.
void process_exit(int code) __attribute__((noreturn));

int64_t wait_for_pid(int pid);

// Starts a thread in the current process at user RIP `entry` with
// RDI = `arg`. Returns the new thread, or 0 if the process is exiting,
// out of stack slots, or out of memory.
void thread_kill(struct thread *t);
struct thread *thread_create(uint64_t entry, uint64_t arg);

// Waits for `tid` (a thread of the calling process) to exit, reclaims
// its stacks and struct, and stores its exit code. Returns 0, -ESRCH,
// -EDEADLK for a self-join, or -EINTR.
int thread_join(int tid, int *out_code);

void proc_get(struct process *p);
void proc_put(struct process *p);

#endif
