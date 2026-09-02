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
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "net/socket.h"
#include "kernel.h"

int64_t sys_exit(struct syscall_args *a) {
    process_exit((int)a->a1);
    return 0; // unreachable -- process_exit never returns
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

// getuid / geteuid / getgid / getegid, all four the same function.
//
// NeoOS is single-user and has no credentials at all, so 0 -- root -- is
// the honest answer rather than a placeholder, and it is what makes
// BusyBox's shell stop complaining at startup. FAT has no ownership
// either, so there is nothing for a non-zero value to mean.
//
// One handler behind four numbers on purpose: four identical functions
// would invite one of them to drift. Recorded in docs/stdlib.md.
int64_t sys_getuid(struct syscall_args *a) {
    (void)a;
    return 0;
}

int64_t sys_yield(struct syscall_args *a) {
    (void)a;
    schedule();
    return 0;
}

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
    if (rc > 0 && a->a2) { *(int *)(uintptr_t)a->a2 = st; }
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

int64_t sys_thread_exit(struct syscall_args *a) {
    thread_exit_self((int)a->a1);
    return 0; // unreachable
}

int64_t sys_thread_join(struct syscall_args *a) {
    int code = 0;
    int rc = thread_join((int)a->a1, &code);
    if (rc == 0 && a->a2) {
        *(int *)(uintptr_t)a->a2 = code;
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
    if (!p || p->pid != 1) { return -EPERM; }

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
