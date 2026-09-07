// kernel/syscall/sys_proc.c -- Processes and threads: creation, exit, reaping, groups.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "drivers/char/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "errno.h"
#include "sync/lock.h"
#include "ipc/signal.h"
#include "ipc/futex.h"
#include "ipc/pipe.h"
#include "drivers/char/timer.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "smp/membarrier.h"
#include "net/socket.h"
#include "kernel.h"

// exit(2)'s real Linux semantics, now that clone() makes them
// observable: exit() ends ONLY the calling thread if others are still
// alive, reserving "kill everyone regardless" for exit_group(2)
// (sys_exit_group, already separate). Before real threads existed
// this distinction was invisible -- the calling thread was always the
// only one -- so sys_exit always called process_exit() outright.
//
// This is load-bearing for musl's own pthread_create/join, not a
// hypothetical: __pthread_exit()'s normal path (both the joinable
// case and the detached-without-its-own-stack-to-unmap case) ends
// with a plain `__syscall(SYS_exit, 0)`, expecting exactly this
// thread-only behavior. Calling process_exit() there killed the
// WHOLE process the instant any pthread_create'd worker finished --
// observed as pthread_join() hanging forever (the main thread was
// dead too, along with everything else) while working on
// docs/superpowers/plans/2026-09-07-clone-pthread.md Task 3.
int64_t sys_exit(struct syscall_args *a) {
    struct process *p = current_proc();
    if (p && __atomic_load_n(&p->live_threads, __ATOMIC_ACQUIRE) > 1) {
        thread_exit_self((int)a->a1);
    } else {
        process_exit((int)a->a1);
    }
    return 0; // unreachable -- neither call returns
}

int64_t sys_getpid(struct syscall_args *a) {
    (void)a;
    return current_proc()->pid;
}

// BB2. BusyBox's ash asks for this at startup.
//
// The parent pid is recorded at spawn/fork and is NOT re-read from the
// parent process, which is the point: an orphan's parent is gone, and
// Linux answers 1 for it rather than something stale. p->parent_pid is
// already reset to 1 when a parent exits (see the orphan reparenting in
// proc.c), so this reads correctly for both cases.
int64_t sys_getppid(struct syscall_args *a) {
    (void)a;
    return current_proc()->parent_pid;
}

// BB2. `uname -a` printed a blank line before this existed: the shim
// returned -ENOSYS, and BusyBox printed the (zeroed) struct anyway.
//
// The layout is Linux's exactly -- six fields of UTSNAME_LEN bytes,
// NUL-terminated, no padding -- because a program compiled against
// Linux's <sys/utsname.h> indexes into it directly and no shim can
// retrofit a struct layout.
#define UTSNAME_LEN 65
struct utsname_k {
    char sysname[UTSNAME_LEN];
    char nodename[UTSNAME_LEN];
    char release[UTSNAME_LEN];
    char version[UTSNAME_LEN];
    char machine[UTSNAME_LEN];
    char domainname[UTSNAME_LEN];
};

static void uts_put(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < UTSNAME_LEN - 1) { dst[i] = src[i]; i++; }
    while (i < UTSNAME_LEN) { dst[i++] = '\0'; }
}

int64_t sys_uname(struct syscall_args *a) {
    uint64_t uptr = a->a1;
    if (!user_range_writable(uptr, sizeof(struct utsname_k))) { return -EFAULT; }

    struct utsname_k u;
    // `sysname` is "NeoOS", not "Linux". A program that switches on it
    // will take its non-Linux path, which is the honest answer -- NeoOS
    // is Linux-SHAPED, not Linux, and claiming otherwise would send
    // configure scripts down paths this kernel does not implement.
    // Recorded as a divergence in docs/stdlib.md.
    uts_put(u.sysname,  "NeoOS");
    uts_put(u.nodename, "neoos");
    uts_put(u.release,  "0.1.0");
    uts_put(u.version,  "NeoOS x86_64");
    uts_put(u.machine,  "x86_64");
    uts_put(u.domainname, "(none)");

    uint8_t *dst = (uint8_t *)(uintptr_t)uptr;
    const uint8_t *src = (const uint8_t *)&u;
    for (uint64_t i = 0; i < sizeof u; i++) { dst[i] = src[i]; }
    return 0;
}

// BB2. musl's allocator probes brk before falling back to mmap.
//
// Deliberately a STUB that reports failure the way Linux does: brk
// returns the resulting break, and a request that cannot be satisfied
// returns the CURRENT one unchanged. Returning the current break for
// every request is therefore a valid "cannot grow the heap", and every
// caller that matters -- musl's included -- reads that and uses mmap
// instead, which NeoOS implements properly.
//
// This is not laziness dressed up: a real brk would be a second heap
// mechanism beside mmap, with its own vma, for the sake of an interface
// Linux itself treats as legacy. If something turns up that genuinely
// needs a growable break, it should get one; nothing has yet. Recorded
// in docs/stdlib.md.
int64_t sys_brk(struct syscall_args *a) {
    (void)a;
    return (int64_t)current_proc()->brk;
}

// getuid / geteuid, and getgid / getegid.
//
// These answered a hardcoded 0 for everyone until N3, which is why the
// change is worth naming: they report the truth now, and a program that
// reads them gets a different answer than it did.
//
// There are no EFFECTIVE ids to differ from the real ones, so geteuid
// is getuid. One function behind both numbers on purpose: two identical
// ones would invite one of them to drift.
int64_t sys_getuid(struct syscall_args *a) {
    (void)a;
    return current_proc()->uid;
}

int64_t sys_getgid(struct syscall_args *a) {
    (void)a;
    return current_proc()->gid;
}

// Only god may change identity, and only DOWNWARD -- a process that has
// dropped to an ordinary uid cannot climb back, which is the whole point
// of dropping. login is the one caller: it authenticates as god and then
// becomes the user.
//
// Linux's setuid does more than this (saved-uids, and a non-root caller
// switching among its real/effective/saved set). NeoOS has none of
// those, so anything but "god lowers itself" is -EPERM rather than a
// half-implemented approximation.
int64_t sys_setuid(struct syscall_args *a) {
    struct process *p = current_proc();
    int uid = (int)a->a1;
    if (uid < 0) { return -EINVAL; }
    if (p->uid != 0) { return -EPERM; }
    p->uid = uid;
    return 0;
}

int64_t sys_setgid(struct syscall_args *a) {
    struct process *p = current_proc();
    int gid = (int)a->a1;
    if (gid < 0) { return -EINVAL; }
    // Guarded by the UID, not the gid: dropping the group is part of
    // dropping privilege, and login must do it BEFORE setuid -- after
    // the uid is gone so is the right to change the group.
    if (p->uid != 0) { return -EPERM; }
    p->gid = gid;
    return 0;
}

// sys_yield moved to kernel/syscall/sys_sched.c (SCH-1 T5): it is now a
// real EEVDF yield -- surrender eligibility and drop behind every other
// runnable task -- not a bare schedule().

int64_t sys_spawn(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }
    struct process *child = spawn(path_buf);
    return child ? child->pid : -1;
}

// Copies a user argument vector into kernel memory.
//
// `uargv` is a NULL-terminated array of user pointers. Both spawn and
// exec must do this BEFORE the new address space is built, because
// building it is what stops the caller's pointers from meaning anything
// -- for exec, the pages holding these very strings are freed partway
// through.
//
// Returns 0, or a negative errno. On success the caller owns the result
// and must spawn_args_free() it. argc == 0 (a null or empty vector) is
// success with nothing allocated; the caller substitutes argv[0] = path.
//
// Three ceilings, all of them Linux-shaped, all of them present so an
// untrusted vector cannot drive an unbounded kernel allocation:
// SPAWN_MAX_ARGS entries, SPAWN_ARG_MAX per string, SPAWN_ARG_TOTAL
// overall. Exceeding any of them is -E2BIG. It is NOT silent truncation:
// a shell handed back a command with its arguments quietly dropped would
// run the wrong thing, which is worse than failing.
static int copy_user_vector(uint64_t uargv_addr, int *out_count,
                            char ***out_vec, char **out_blob);

// argv into `out`. The environment is a separate call; see below.
static int copy_user_argv(uint64_t uargv_addr, struct spawn_args *out) {
    out->argc = 0; out->argv = 0; out->blob = 0;
    out->envc = 0; out->envp = 0; out->env_blob = 0;
    return copy_user_vector(uargv_addr, &out->argc, &out->argv, &out->blob);
}

// envp into `out`, using the same walk and the same ceilings. BB3.
// Called after copy_user_argv, so a failure here must leave argv for
// spawn_args_free to release -- which it does, since the two blocks are
// separate fields.
static int copy_user_envp(uint64_t uenvp_addr, struct spawn_args *out) {
    return copy_user_vector(uenvp_addr, &out->envc, &out->envp, &out->env_blob);
}

static int copy_user_vector(uint64_t uargv_addr, int *out_count,
                            char ***out_vec, char **out_blob) {
    *out_count = 0; *out_vec = 0; *out_blob = 0;

    const char *const *uargv = (const char *const *)(uintptr_t)uargv_addr;
    if (!uargv) { return 0; }

    // Count first, validating each slot of the vector as it is reached.
    // The old code checked only the FIRST pointer and then walked the
    // array unbounded -- a vector spanning into an unmapped page faulted
    // in the kernel.
    int argc = 0;
    for (;;) {
        uint64_t slot = (uint64_t)(uintptr_t)&uargv[argc];
        if (!user_range_readable(slot, sizeof(void *))) { return -EFAULT; }
        if (!uargv[argc]) { break; }
        if (++argc > SPAWN_MAX_ARGS) { return -E2BIG; }
    }
    if (argc == 0) { return 0; }

    // Measure before allocating: one blob holds every string, so its
    // size has to be known up front, and the total budget has to be
    // enforced before any of it is committed.
    uint64_t total = 0;
    for (int i = 0; i < argc; i++) {
        uint64_t p = (uint64_t)(uintptr_t)uargv[i];
        uint64_t len = 0;
        while (len < SPAWN_ARG_MAX) {
            if (!user_range_readable(p + len, 1)) { return -EFAULT; }
            if (!((const char *)(uintptr_t)p)[len]) { break; }
            len++;
        }
        if (len >= SPAWN_ARG_MAX) { return -E2BIG; }
        total += len + 1;
        if (total > SPAWN_ARG_TOTAL) { return -E2BIG; }
    }

    (*out_blob) = kmalloc(total);
    (*out_vec) = kmalloc((uint64_t)argc * sizeof(char *));
    if (!(*out_blob) || !(*out_vec)) { if (*out_blob) { kfree(*out_blob); *out_blob = 0; }
        if (*out_vec) { kfree(*out_vec); *out_vec = 0; }
        return -ENOMEM; }
    (*out_count) = argc;

    char *w = (*out_blob);
    for (int i = 0; i < argc; i++) {
        const char *src = (const char *)(uintptr_t)uargv[i];
        (*out_vec)[i] = w;
        // Bounded by `total`, which was measured from these same
        // strings a moment ago. A concurrent thread lengthening one
        // cannot push past the blob: the copy stops at SPAWN_ARG_MAX
        // and at the end of the space this string was measured to need.
        uint64_t room = (uint64_t)((*out_blob) + total - w);
        uint64_t n = 0;
        while (n + 1 < room && src[n]) { *w++ = src[n]; n++; }
        *w++ = '\0';
    }
    return 0;
}

// spawn with an argument vector.
int64_t sys_spawnv(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }

    struct spawn_args args;
    int rc = copy_user_argv(a->a3, &args);
    if (rc != 0) { return rc; }
    // a4 is the environment (BB3). spawnv's older two-argument form
    // passes 0, which is an empty environment rather than an error.
    rc = copy_user_envp(a->a4, &args);
    if (rc != 0) { spawn_args_free(&args); return rc; }

    // No vector, or an empty one, means the same as spawn(): argv[0] is
    // the path. A program with no argv[0] at all is a shape nothing
    // expects.
    struct process *child = args.argc ? spawn_argv(path_buf, &args)
                                      : spawn(path_buf);
    spawn_args_free(&args);
    return child ? child->pid : -1;
}

int64_t sys_wait(struct syscall_args *a) {
    return wait_for_pid((int)a->a1);
}

int64_t sys_fork(struct syscall_args *a) {
    struct thread *child = fork_task(a->frame);
    return child ? child->proc->pid : -1;
}

// execve. a3 is the user argv and a4 the environment, both
// NULL-terminated; 0 for either means "none", which is what the older
// one-argument exec() wrapper passes.
//
// Both vectors are copied into the kernel HERE, before exec_task runs,
// and deliberately so: exec_task frees the calling image's address space
// partway through, and these strings live in it.
int64_t sys_exec(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }

    struct spawn_args args;
    int rc = copy_user_argv(a->a3, &args);
    if (rc != 0) { return rc; }
    rc = copy_user_envp(a->a4, &args);
    if (rc != 0) { spawn_args_free(&args); return rc; }

    int ok = exec_task(path_buf, a->frame,
                       (args.argc || args.envc) ? &args : 0);
    spawn_args_free(&args);
    return ok ? 0 : -1;
}

int64_t sys_wait4(struct syscall_args *a) {
    int st = 0;
    int64_t rc = wait4((int)a->a1, &st, (int)a->a3);
    if (rc > 0 && a->a2) {
        uint64_t missed = copy_to_user((void *)(uintptr_t)a->a2, &st, sizeof st);
        if (missed > 0) { return -EFAULT; }
    }
    return rc;
}

int64_t sys_setpgid(struct syscall_args *a) {
    int pid  = (int)a->a1 ? (int)a->a1 : current_proc()->pid;
    int pgid = (int)a->a2 ? (int)a->a2 : pid;
    struct process *p = proc_find(pid);
    if (!p) { return -ESRCH; }
    p->pgid = pgid;
    proc_put(p);
    return 0;
}

int64_t sys_getpgid(struct syscall_args *a) {
    int q = (int)a->a1;
    struct process *p = q ? proc_find(q) : current_proc();
    if (!p) { return -ESRCH; }
    int pgid = p->pgid;
    if (q) { proc_put(p); }
    return pgid;
}

int64_t sys_setsid(struct syscall_args *a) {
    (void)a;
    struct process *p = current_proc();
    p->sid  = p->pid;
    p->pgid = p->pid;
    return p->sid;
}

int64_t sys_getsid(struct syscall_args *a) {
    int q = (int)a->a1;
    struct process *p = q ? proc_find(q) : current_proc();
    if (!p) { return -ESRCH; }
    int sid = p->sid;
    if (q) { proc_put(p); }
    return sid;
}

int64_t sys_thread_create(struct syscall_args *a) {
    struct thread *t = thread_create(a->a1, a->a2);
    return t ? t->tid : -EAGAIN;
}

// clone(flags, child_stack, ptid, ctid, tls) -- raw Linux argument
// order, matching musl's own __clone asm exactly (see
// third_party/shim/clone.s and docs/superpowers/specs/
// 2026-09-07-clone-pthread-design.md). tls is NOT in struct
// syscall_args's a1..a4 -- it is the 5th syscall argument, which this
// codebase's convention (see sys_mmap) reads out of frame->r8
// directly.
//
// Scope: EXACTLY the flag combination musl's pthread_create.c sends.
// Anything else is -EINVAL, not approximated -- every other clone(2)
// use (namespaces, CLONE_VFORK, selective-sharing process creation) is
// out of scope for this primitive; fork() already covers process
// creation.
//
// These cross the syscall boundary, so their VALUES are Linux's,
// exactly like the other flag blocks in this file. From
// neoos-musl/upstream/include/sched.h.
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED       0x00400000

#define NEOOS_CLONE_FLAGS_SUPPORTED \
    (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD \
     | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID \
     | CLONE_CHILD_CLEARTID | CLONE_DETACHED)

int64_t sys_clone(struct syscall_args *a) {
    uint64_t flags       = (uint64_t)a->a1;
    uint64_t child_stack = (uint64_t)a->a2;
    uint64_t ptid        = (uint64_t)a->a3;
    uint64_t ctid        = (uint64_t)a->a4;
    uint64_t tls         = a->frame->r8;

    if (flags != NEOOS_CLONE_FLAGS_SUPPORTED) { return -EINVAL; }
    if (!child_stack) { return -EINVAL; }

    struct thread *t = clone_task(a->frame, child_stack, tls);
    if (!t) { return -EAGAIN; }

    t->clear_child_tid = ctid;   // acted on by thread_exit_self

    // *ptid must land, and thread_alloc()'s zeroing must be done,
    // before this thread can possibly run -- clone_task() deliberately
    // leaves it off the ready queue for exactly this reason. See its
    // own comment.
    if (ptid) {
        int tid = t->tid;
        uint64_t missed = copy_to_user((void *)(uintptr_t)ptid, &tid, sizeof tid);
        if (missed > 0) { return -EFAULT; }
    }

    int tid = t->tid;
    thread_enqueue_ready(t);
    return tid;
}

int64_t sys_thread_exit(struct syscall_args *a) {
    thread_exit_self((int)a->a1);
    return 0; // unreachable
}

int64_t sys_thread_join(struct syscall_args *a) {
    int code = 0;
    int rc = thread_join((int)a->a1, &code);
    if (rc == 0 && a->a2) {
        uint64_t missed = copy_to_user((void *)(uintptr_t)a->a2, &code, sizeof code);
        if (missed > 0) { return -EFAULT; }
    }
    return rc;
}

int64_t sys_thread_self(struct syscall_args *a) {
    (void)a;
    return current_thread()->tid;
}

// set_tid_address(ptr) -- musl's __init_tls calls this unconditionally
// before main, and uses only the RETURN VALUE (the caller's tid).
//
// The pointer is the "clear child tid" address: Linux writes 0 there
// and futex-wakes it when the thread exits, which is how a joiner
// notices. NeoOS's threads are joined through thread_join instead, so
// the address is RECORDED AND NOT ACTED ON.
//
// DIVERGENCE, and it matters for exactly one thing: musl's
// pthread_join spins on that word. Nothing uses musl's pthreads on
// NeoOS yet -- when something does, this is where the wake belongs
// rather than in a shim. Recorded in docs/stdlib.md.
int64_t sys_set_tid_address(struct syscall_args *a) {
    struct thread *t = current_thread();
    if (!t) { return -ESRCH; }
    t->clear_child_tid = (uint64_t)a->a1;
    return t->tid;
}

// exit_group(status) -- ends EVERY thread in the process, which is what
// musl's _Exit calls. NeoOS's exit already had process-wide semantics,
// so this is the same operation under Linux's name; sys_exit stays as
// the NeoOS-native spelling.
int64_t sys_exit_group(struct syscall_args *a) {
    process_exit((int)a->a1);
    return 0;   // not reached
}

// reboot(2). Linux's magic-2 command words; Linux gates on
// CAP_SYS_BOOT, NeoOS on "caller is PID 1" -- see docs/stdlib.md. Only
// the three real commands are accepted; none of them return.
#define REBOOT_RESTART   0x01234567u
#define REBOOT_HALT      0xcdef0123u
#define REBOOT_POWER_OFF 0x4321fedcu

int64_t sys_reboot(struct syscall_args *a) {
    struct process *p = current_proc();
    // god, rather than PID 1. It was pid-1-only before N3 because there
    // was no notion of a privileged user to check instead -- which is
    // why `make shell` had no way to power the machine off.
    if (!p || p->uid != 0) { return -EPERM; }

    switch ((uint32_t)a->a1) {
    case REBOOT_POWER_OFF:
        kernel_shutdown();                 // ACPI S5, never returns
        break;
    case REBOOT_HALT:
        __asm__ volatile ("cli");
        for (;;) { __asm__ volatile ("hlt"); }
    case REBOOT_RESTART: {
        // 8042 CPU-reset pulse; if the platform ignores it, fall through
        // to a triple fault via a zero-length IDT.
        __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)0xFE),
                          "Nd"((uint16_t)0x64));
        for (volatile int i = 0; i < 1000000; i++) { }
        struct { uint16_t limit; uint64_t base; } __attribute__((packed))
            idt0 = { 0, 0 };
        __asm__ volatile ("lidt %0; int3" :: "m"(idt0));
        for (;;) { __asm__ volatile ("hlt"); }
    }
    default:
        return -EINVAL;
    }
    return 0;   // unreachable for the three real commands
}

// sched_getaffinity(pid, cpusetsize, mask) -- Linux shape. `pid` is
// ignored (NeoOS has nothing resembling per-thread CPU pinning to
// report differently per pid): every thread is affine to every online
// CPU, so the answer is the same regardless of which one asked.
// Found missing via dotnet NativeAOT's CoreCLR startup querying it
// during RhInitialize. See docs/stdlib.md.
int64_t sys_sched_getaffinity(struct syscall_args *a) {
    uint64_t cpusetsize = a->a2;
    uint64_t mask_ptr   = a->a3;
    if (!mask_ptr) { return -EFAULT; }

    int online = smp_online_count();
    uint64_t need = (uint64_t)((online + 7) / 8);
    if (need == 0) { need = 1; }
    // Real Linux returns -EINVAL when the caller's buffer is too small
    // to hold the whole mask -- it never truncates.
    if (cpusetsize < need) { return -EINVAL; }

    uint8_t mask[(MAX_CPUS + 7) / 8];
    for (uint64_t i = 0; i < need; i++) { mask[i] = 0; }
    for (int i = 0; i < online; i++) { mask[i >> 3] |= (uint8_t)(1u << (i & 7)); }

    uint64_t missed = copy_to_user((void *)(uintptr_t)mask_ptr, mask, need);
    if (missed > 0) { return -EFAULT; }
    // The return value is the number of bytes actually written into
    // the mask, per Linux's sched_getaffinity(2) -- musl's wrapper
    // reads it, not just the success/failure of the call.
    return (int64_t)need;
}

// membarrier(cmd, flags) -- Linux's command bitmask (kernel/smp/
// membarrier.h). Real cross-CPU synchronisation, not a lie: NeoOS is
// SMP-capable, and membarrier_global() (kernel/smp/membarrier.c) IPIs
// every online CPU but this one and waits for each to acknowledge --
// on a single-core box that is simply nothing to wait for.
//
// Every command below is serviced by the SAME global barrier: Linux's
// PRIVATE_EXPEDITED variants only promise to reach the CALLING
// process's own registered threads, which a global barrier already
// does (and more) -- correctness never suffers from a barrier that
// reaches more CPUs than strictly required, only efficiency does, and
// NeoOS has no per-process CPU registration to make the distinction
// meaningful yet. SYNC_CORE is included because IPI delivery is
// already a serializing event on x86-64 (Intel SDM: an interrupt
// forces the processor to complete every prior instruction before the
// handler runs) -- the exact guarantee SYNC_CORE documents needing
// beyond ordinary memory ordering.
//
// Found missing via dotnet NativeAOT's CoreCLR startup. See
// docs/stdlib.md.
int64_t sys_membarrier(struct syscall_args *a) {
    int cmd   = (int)a->a1;
    uint64_t flags = (uint64_t)a->a2;

    if (cmd == MEMBARRIER_CMD_QUERY) {
        return MEMBARRIER_CMD_GLOBAL
             | MEMBARRIER_CMD_GLOBAL_EXPEDITED
             | MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED
             | MEMBARRIER_CMD_PRIVATE_EXPEDITED
             | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED
             | MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE
             | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE;
    }
    if (flags != 0) { return -EINVAL; }

    switch (cmd) {
    case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
        // No per-process bookkeeping needed: the barrier below already
        // reaches every CPU unconditionally, registered or not.
        return 0;
    case MEMBARRIER_CMD_GLOBAL:
    case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
        membarrier_global();
        return 0;
    default:
        return -EINVAL;
    }
}
