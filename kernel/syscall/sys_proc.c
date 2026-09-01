// kernel/syscall/sys_proc.c -- Processes and threads: creation, exit, reaping, groups.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "dev/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "errno.h"
#include "sync/lock.h"
#include "ipc/signal.h"
#include "ipc/futex.h"
#include "ipc/pipe.h"
#include "dev/timer.h"
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

// spawn with an argument vector. `argv` is a NULL-terminated array of
// user pointers, copied into the kernel before the new address space is
// built -- because building it is what stops the caller's pointers from
// being meaningful.
int64_t sys_spawnv(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }

    struct spawn_args *args = (struct spawn_args *)kmalloc(sizeof(*args));
    if (!args) { return -ENOMEM; }
    args->argc = 0;

    const char *const *uargv = (const char *const *)(uintptr_t)a->a3;
    if (uargv) {
        if (!user_range_writable((uint64_t)(uintptr_t)uargv, sizeof(void *))) {
            kfree(args);
            return -EFAULT;
        }
        while (args->argc < SPAWN_MAX_ARGS && uargv[args->argc]) {
            copy_user_string((int64_t)(uintptr_t)uargv[args->argc],
                             args->argv[args->argc], SPAWN_ARG_MAX);
            args->argc++;
        }
    }
    // No vector, or an empty one, means the same as spawn(): argv[0] is
    // the path. A program with no argv[0] at all is a shape nothing
    // expects.
    if (args->argc == 0) {
        kfree(args);
        struct process *child = spawn(path_buf);
        return child ? child->pid : -1;
    }

    struct process *child = spawn_argv(path_buf, args);
    kfree(args);
    return child ? child->pid : -1;
}

int64_t sys_wait(struct syscall_args *a) {
    return wait_for_pid((int)a->a1);
}

int64_t sys_fork(struct syscall_args *a) {
    struct thread *child = fork_task(a->frame);
    return child ? child->proc->pid : -1;
}

int64_t sys_exec(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }
    return exec_task(path_buf, a->frame) ? 0 : -1;
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
