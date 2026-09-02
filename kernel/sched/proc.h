#ifndef NEOOS_PROCESS_H
#define NEOOS_PROCESS_H

#include <stdint.h>
#include "arch/cpu.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "ipc/signal.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "elf.h"

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
                    THREAD_BLOCKED, THREAD_STOPPED, THREAD_ZOMBIE };

struct file_ops;

struct file_descriptor {
    int in_use;
    // What this fd is, and what it is attached to. `ops` is
    // &vnode_file_ops for a file on a filesystem and something else for
    // a pipe or a port; `priv` is that implementation's object. See
    // kernel/file.h.
    const struct file_ops *ops;
    void *priv;
    struct vnode *vn;   // reference held; released by close/process exit
    uint32_t position;  // per-fd, NOT shared across fork -- see docs/stdlib.md
    int writable;
    // Split out from `writable` for pipes, where the two ends of one
    // object differ. A vnode-backed fd is readable regardless of its
    // open mode, which is how NeoOS has always behaved -- O_WRONLY does
    // not currently prevent a read. Recorded in docs/stdlib.md.
    int readable;
    // O_NONBLOCK. Per-DESCRIPTOR, which is where POSIX puts it: it
    // belongs to the open file description, not to the pipe or socket
    // underneath, so two fds on one pipe can disagree about it.
    int nonblock;
};

struct thread;
struct sigqueue;
struct thread_table;  // opaque; defined in thread_table.h
struct fd_table;      // opaque; defined in fd_table.h

// Threads per process. Sixteen was a Phase-2 placeholder, and it was a
// hard wall: pthread_create simply failed at the seventeenth thread.
//
// The number is set by the ADDRESS SPACE, which is the honest
// constraint. Each thread's stack is THREAD_STACK_STRIDE below the last
// (four pages plus a guard page), laid out downward from
// USER_STACK_TOP, and the gap between MMAP_BASE and USER_STACK_TOP is
// 32 TiB -- so 1024 slots occupies 20 MiB of a region with room for
// millions. What actually costs something is the per-process bitmap
// tracking them, at 8 bytes per 64 slots.
#define MAX_THREADS_PER_PROC 1024
#define THREAD_SLOT_WORDS    (MAX_THREADS_PER_PROC / 64)

struct process {
    int pid, parent_pid;
    // One count per LIVE thread. When it reaches zero the address space
    // is freed and the process becomes a zombie carrying only its exit
    // code (see proc_put). This is a liveness count, NOT a reference
    // count -- the struct itself outlives the address space and is
    // freed by wait_for_pid's reap.
    uint32_t live_threads;
    // Reference count on this struct. Starts at 1 (the proc_table's
    // entry). A lookup that outlives its bucket lock -- proc_find --
    // returns the pointer with this raised; the caller drops it with
    // proc_put. The struct + pid are freed when this hits zero.
    uint32_t ref;
    // Set by proc_table_for_each_ref to the current iteration's
    // generation as each process is visited, so a bucket that must be
    // scanned in more than one batch does not double-visit. Starts 0
    // (proc_alloc zeroes); the generation counter starts at 1.
    uint32_t iter_gen;
    uint64_t pml4_phys;             // 0 = shares the kernel address space
    // What pml4_phys was when the address space was torn down.
    // proc_put_live cannot shoot down (it runs with interrupts off on a
    // thread already marked ZOMBIE), so it defers every frame instead
    // and proc_reap performs the shootdown -- which needs to know which
    // address space to name. The value stays unambiguous because those
    // frames, the PML4 frame among them, are still sitting on the
    // deferred queue and cannot have been handed to anyone else yet.
    uint64_t reap_pml4_phys;
    // What the ELF loader learned about the image: where its program
    // headers landed, and its PT_TLS template. Kept for the whole life
    // of the process because every thread created later needs the TLS
    // template again, not just the first one.
    struct elf_info elf;
    // The program's name, as /proc/<pid>/stat's `comm` field and `ps`
    // report it: the basename of the path it was spawned or exec'd
    // from, truncated to Linux's 15 characters plus a NUL. Set at spawn
    // and again at exec, because exec is a new program.
    char comm[16];
    struct vma *vmas;               // sorted by start, non-overlapping
    uint64_t    mmap_next;          // bump hint for unhinted mmap
    // The program break. Never grows -- sys_brk reports it unchanged,
    // which is Linux's way of saying the heap cannot be extended, and
    // sends callers to mmap. Kept per-process so the value a program
    // reads back is stable across calls, as brk's contract requires.
    uint64_t    brk;
    // Guards `vmas`, `mmap_next` and this process's page tables. Rank
    // LOCK_RANK_MM (3) -- low, because a demand-paging fault takes it
    // and then reads through the filesystem, and because such a fault
    // can sleep. Must NOT be held across schedule().
    struct spinlock mm_lock;
    // Indexed DIRECTLY by fd -- /dev/console is a real vnode opened on
    // 0, 1 and 2 at process creation, so the fd IS the index. The table
    // belongs to the PROCESS: threads share it. See fd_table.h for the
    // 2-level layout and the 16,384-fd ceiling.
    struct fd_table *fd_table;
    // The CONTROLLING TERMINAL, or 0 for a process that has none.
    // Claimed with TIOCSCTTY by a session leader, inherited across fork
    // and kept across exec. This is what /dev/tty resolves to.
    //
    // A bare pointer is safe here: ttys are either static (the console,
    // the VTs) or pool entries that are reused but never freed
    // (kernel/tty/pty.c keeps its structs deliberately, see the note
    // there), so one can never dangle.
    struct tty *ctty;
    // Current working directory: always absolute, always canonical
    // (no "." or ".."), never empty, and never with a trailing slash
    // except at the root. EVERY process has one from the moment
    // proc_alloc returns -- "/" if nothing else -- so no path-taking
    // syscall ever has to cope with a process that has none.
    // Inherited by fork and by spawn, as on Linux.
    char cwd[VFS_MAX_PATH];
    struct thread *threads;         // live threads, via thread->proc_next
    struct thread *zombies;         // exited, unjoined; freed at reap
    struct thread_table *thread_table;  // per-process thread hash table (NEW)
    // Bitmap of live thread user stacks, one bit per slot. Was a single
    // uint16_t, which is where the 16-thread ceiling actually lived.
    uint64_t stack_slots[THREAD_SLOT_WORDS];
    int exiting;
    int exit_code;
    // Set under p->lock by the first proc_reap(); a second waiter that
    // also saw the zombie before it was unlinked bails instead of
    // double-freeing.
    int reaped;
    enum { PROC_ALIVE, PROC_ZOMBIE } state;

    // Signal dispositions are per-PROCESS; masks and pending sets are
    // per-thread as well as here. POSIX's model, and the one musl
    // expects.
    struct k_sigaction actions[NSIG];
    // The per-process lock. Guards the signal dispositions, `pending`,
    // `queued` and the stop state below, AND thread membership
    // (`threads`, `zombies`, `live_threads`). Rank LOCK_RANK_PROCESS.
    struct spinlock    lock;
    sigset_t_k         pending;         // process-directed: kill()
    struct sigqueue   *queued;          // RT payloads, process-directed
    int                pgid, sid;
    int                exit_signal;     // 0, or the signal that killed it
    int                stopped_count;   // threads parked in THREAD_STOPPED
    int                stop_reported;   // wait4(WUNTRACED) has seen this stop

    struct waitq sig_waiters;       // sigsuspend sleepers
    struct waitq child_waiters;     // parents blocked in wait4
    struct waitq exit_waiters;      // threads blocked in wait_for_pid
    struct waitq join_waiters;      // threads blocked in thread_join

    struct process *proc_next_hash; // proc_table bucket chain
};

struct thread {
    int tid;                        // 0 == an idle thread (never a pid)
    // References to this struct. Starts at 1 (the process's thread
    // list). A transient holder that keeps a `struct thread *` past
    // releasing p->lock -- signal delivery, process_exit's sibling
    // snapshot -- takes one with thread_get() and drops it with
    // thread_put(). The struct + xstate are freed when this hits zero;
    // the kernel stack is freed earlier, by whoever drains the zombie
    // list, and is NOT gated on this count (that is what on_cpu is for).
    uint32_t ref;
    // set_tid_address's "clear child tid" pointer. Recorded because
    // musl sets it before main; NOT acted on at exit -- see
    // sys_set_tid_address for why, and docs/stdlib.md for the
    // divergence that creates.
    uint64_t clear_child_tid;
    struct process *proc;           // 0 for the pre-process idle thread
    enum thread_state state;
    // Non-zero while SOME CPU is still executing on this thread's
    // kernel stack -- set when a CPU switches to it, cleared only once
    // the switch AWAY from it has completed and saved_rsp is final.
    // Nothing may make the thread runnable, or free its stack, while
    // this is set. See sched_post_switch() and wait_off_cpu() in
    // sched.c for the whole story.
    volatile uint32_t on_cpu;
    uint64_t saved_rsp;
    // IA32_FS_BASE for this thread: the thread pointer, and therefore
    // where all its __thread variables live. Per-THREAD and restored on
    // every context switch -- without that, a thread migrating to
    // another CPU would keep whatever FS base the previous occupant of
    // that CPU left behind. Set only by arch_prctl(ARCH_SET_FS).
    uint64_t fs_base;
    uint64_t kernel_stack_top;
    uint64_t kernel_stack_phys;
    int stack_slot;                 // -1 for kernel-only threads
    int exit_code;
    struct waitq *blocked_on;
    // CS5.2. Set by poll_head_notify for every poller registered on an
    // object that just became ready, cleared by poll_core before each
    // readiness scan. It is what lets a poller tell "nothing has
    // happened, sleep" from "something happened while I was scanning,
    // scan again" without a lost wakeup.
    volatile int  poll_notified;
    // Nonzero while this poller has at least one fd whose object has no
    // poll head yet, and so still needs the global broadcast. Pollers
    // fully covered by poll heads are skipped by it.
    volatile int  poll_wants_broadcast;
    uint64_t      sleep_deadline;   // 0 = none; else a timer_ticks() value
    struct thread *timeout_next;    // list of threads with a deadline

    sigset_t_k    blocked;
    sigset_t_k    pending;         // thread-directed: tkill()
    struct sigqueue *queued;
    sigset_t_k    saved_blocked;    // sigsuspend / sigreturn
    int           in_sigsuspend;
    // Set under p->lock the moment this thread takes a stop signal
    // out of its pending set, cleared under the same lock by SIGCONT
    // and by the stop itself. It is what lets a SIGCONT cancel a stop
    // that has been decided but not yet committed -- see signal_do_stop.
    int           stopping;
    // Set by thread_kill_solo (execve). A fatal signal normally takes
    // the WHOLE process down -- that is what SIGKILL means -- but exec
    // has to end its siblings while the process itself keeps going, so
    // this redirects the fatal default action to thread_exit_self.
    volatile int  solo_kill;
    stack_t_k     altstack;         // sigaltstack; zeroed = none
    // cpu_state_size() bytes, 64-byte aligned. A pointer rather than an
    // inline array because the size is only known at run time -- see
    // the extended-state design spec.
    void *xstate;

    struct thread *tid_next_hash;   // thread_table bucket chain (secondary index)
    struct thread *proc_next;       // p->threads / p->zombies list link
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
struct thread *thread_alloc_kernel_on(void (*entry)(void), int cpu_index);
struct thread *thread_alloc_kernel_unqueued(void (*entry)(void));

// Run-queue insertion, shared with waitq.c's wake path.
void thread_enqueue_ready(struct thread *t);

// Moves `t` from `from` (THREAD_BLOCKED or THREAD_STOPPED) to
// THREAD_READY and queues it, waiting first for it to finish leaving
// whatever CPU it was on. Returns 1 if this caller won the transition,
// 0 if someone else already did. EVERY wake goes through here.
int thread_wake(struct thread *t, enum thread_state from);

// Waits until no CPU is executing on `t`'s kernel stack. Needed before
// freeing that stack; thread_wake() does it for the wake path.
void thread_wait_off_cpu(struct thread *t);

#define USER_STACK_PAGES 4
#define USER_STACK_TOP 0x0000700000000000ULL



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

// Arguments for a new process. Copied out of the caller's address space
// -- which spawn is about to stop looking at -- into one heap block, and
// pointed at from a vector.
//
// This used to be `char argv[8][128]` by value: 8 arguments of 128 bytes,
// a 1 KiB struct on the kernel stack. A shell cannot live inside that.
// `ls -la`, a compiler invocation, or `find . -exec` all exceed it, and
// the excess was silently TRUNCATED rather than reported.
//
// The ceilings are now Linux-shaped: a per-argument length and a total
// byte budget, rather than a slot count. They exist so an untrusted
// syscall argument cannot drive an unbounded kernel allocation, which
// was the real point of the old fixed array.
#define SPAWN_MAX_ARGS   1024            // argc ceiling
#define SPAWN_ARG_MAX    4096            // one argument, Linux MAX_ARG_STRLEN-ish
#define SPAWN_ARG_TOTAL  (256 * 1024)    // all of argv together, ARG_MAX-ish

struct spawn_args {
    int    argc;
    char **argv;   // argc entries, each pointing into `blob`
    char  *blob;   // every string, NUL-separated; freed with the args

    // The environment, carried the same way and under the same ceilings
    // (BB3). Separate from argv rather than appended to it because the
    // two are separate vectors on the entry stack, with their own NULL
    // terminators, and a program reads envp by walking past argv's.
    int    envc;
    char **envp;
    char  *env_blob;
};

// Builds a one-argument vector (argv[0] = path), which is what a spawn
// with no vector gets. The environment is left empty. Returns 0 on
// allocation failure.
int  spawn_args_single(struct spawn_args *out, const char *path);
// Frees what spawn_args_single or the syscall layer allocated. Safe on a
// zeroed struct.
void spawn_args_free(struct spawn_args *args);

struct process *spawn(const char *path);

// spawn with an argument vector. `args` may be null, in which case the
// process gets argv[0] = path and nothing else -- which is exactly what
// spawn() does.
struct process *spawn_argv(const char *path, const struct spawn_args *args);

// Duplicates the calling THREAD into a new single-threaded process,
// sharing user frames copy-on-write. Returns the child's thread, or 0
// on failure (parent unaffected).
struct thread *fork_task(struct syscall_frame *frame);

// `args` may be null, in which case the new image gets argv[0] = path.
int exec_task(const char *path, struct syscall_frame *frame,
              const struct spawn_args *args);

// Ends the calling thread only. When it is the last live thread of its
// process, the address space is freed and waiters are woken.
void thread_exit_self(int code) __attribute__((noreturn));

// Ends the whole process.
void process_exit(int code) __attribute__((noreturn));

#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

int64_t wait4(int pid, int *status, int options);
int64_t wait_for_pid(int pid);

// Starts a thread in the current process at user RIP `entry` with
// RDI = `arg`. Returns the new thread, or 0 if the process is exiting,
// out of stack slots, or out of memory.
void thread_kill(struct thread *t);
void thread_kill_solo(struct thread *t);
struct thread *thread_create(uint64_t entry, uint64_t arg);

// Reference counting for `struct thread` (see the `ref` field). Take a
// ref before keeping a thread pointer past the lock that pins it;
// thread_put frees the struct + xstate on the last drop.
void thread_get(struct thread *t);
void thread_put(struct thread *t);

// Waits for `tid` (a thread of the calling process) to exit, reclaims
// its stacks and struct, and stores its exit code. Returns 0, -ESRCH,
// -EDEADLK for a self-join, or -EINTR.
int thread_join(int tid, int *out_code);

// Looks up a process by pid. Returns it with a reference held (see
// proc_get / proc_put) or NULL -- EVERY caller must proc_put the
// result. A pointer from current_proc() is NOT ref-counted (you are
// running in it) and must not be proc_put.
struct process *proc_find(int pid);

// Reference count on the struct itself (p->ref). proc_put frees the
// struct + pid on the last drop.
void proc_get(struct process *p);
void proc_put(struct process *p);

void proc_put_live(struct process *p);

#endif
