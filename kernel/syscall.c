#include "syscall.h"
#include "gdt.h"
#include "serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "errno.h"
#include "lock.h"
#include "signal.h"
#include "timer.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "cpu_local.h"
#include "smp.h"
#include "futex.h"
#include "file.h"
#include "pipe.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define EFER_SCE (1ULL << 0)
#define EFER_NXE (1ULL << 11)

// LSTAR as each CPU actually read it back after programming itself.
// Written once per CPU during bringup, read only by the selftest.
static uint64_t syscall_msr_seen[MAX_CPUS];

#include "syscall_nr.h"

// Mirrors lib/include/fcntl.h's O_* values exactly -- the two trees
// don't share headers, so these must be kept in sync by hand.
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

// Mirrors lib/include/unistd.h's SEEK_* values exactly.
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern void syscall_entry(void); // syscall_entry.asm

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

// The filesystem lock now lives in kernel/fs/vfs.c, because
// kernel/file.c's vnode operations take it too and a pipe's must not.
#define fs_lock_acquire() vfs_lock()
#define fs_lock_release() vfs_unlock()

// Every fd access in this file goes through these three, so the
// backing store is named in exactly one place.

static struct file_descriptor *fd_get(struct process *p, int fd) {
    if (!p) { return 0; }
    return fd_table_get(p->fd_table, fd);
}

static int fd_alloc(struct process *p) {
    if (!p) return -EMFILE;
    return fd_table_alloc(p->fd_table);
}

static int fd_close(struct process *p, int fd) {
    if (!p || fd < 0) return -EBADF;
    if (!fd_table_get(p->fd_table, fd)) return -EBADF;
    fd_table_close(p->fd_table, fd);
    return 0;
}

// Copies up to out_size-1 bytes from a user-supplied (pointer, len)
// pair into a NUL-terminated kernel buffer. Shared by every syscall
// that takes a path (SPAWN/OPEN/MKDIR/UNLINK).
// Bounded copy of a NUL-terminated user string. Used only by
// SYS_MOUNT: every other path-taking syscall passes an explicit
// (pointer, length) pair via copy_user_path, but mount needs three
// strings and syscall_dispatch has only four argument slots
// (a1-a4, from rdi/rsi/rdx/r10 in syscall_entry.asm). Widening the
// syscall ABI to six arguments for one call was rejected.
static void copy_user_string(int64_t user_ptr, char *out, uint64_t out_size) {
    const char *s = (const char *)(uintptr_t)user_ptr;
    uint64_t i = 0;
    while (i < out_size - 1 && s[i]) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

static void copy_user_path(int64_t user_ptr, int64_t user_len, char *out, uint64_t out_size) {
    uint64_t len = (uint64_t)user_len;
    if (len > out_size - 1) {
        len = out_size - 1;
    }
    const char *user_path = (const char *)(uintptr_t)user_ptr;
    for (uint64_t i = 0; i < len; i++) {
        out[i] = user_path[i];
    }
    out[len] = '\0';
}

// ---------------------------------------------------------- dispatch
//
// One function per syscall, reached through a table indexed by the
// syscall number. The switch this replaces had grown to four hundred
// lines in a single function: every case shared one scope, so a `char
// path_buf[64]` in one arm sat on the stack of every other arm, and
// adding a call meant editing the middle of a function nothing could
// be said about as a whole. A table also makes the syscall set
// enumerable -- the integrity selftest below, and eventually a trace
// facility, need exactly that.
//
// Handlers take one argument rather than the five they would otherwise
// need. That is not only tidier: with five parameters, every handler
// that ignores some of them needs a `(void)` cast to satisfy
// -Wunused-parameter, and forty of those is noise that hides the one
// place it might have meant something.

struct syscall_args {
    int64_t a1, a2, a3, a4;
    // The caller's full saved user context. fork() copies it, exec()
    // and sigreturn() overwrite it in place, and mmap reads arguments
    // 5 and 6 out of frame->r8/r9 -- syscall_entry.asm pushes those
    // before its argument shuffle, so the four-argument calling
    // convention never had to be widened.
    struct syscall_frame *frame;
};

typedef int64_t (*syscall_handler)(struct syscall_args *);

static int64_t sys_exit(struct syscall_args *a) {
    process_exit((int)a->a1);
    return 0; // unreachable -- process_exit never returns
}

// The four fd operations are now four lines each. Whether the target
// is a file, a pipe or (later) a socket is the file layer's business;
// none of them can grow a special case for one kind of object without
// that being conspicuous.
static int64_t sys_write(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_write(f, (const void *)(uintptr_t)a->a2, (uint64_t)a->a3);
}

static int64_t sys_read(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_read(f, (void *)(uintptr_t)a->a2, (uint64_t)a->a3);
}

static int64_t sys_getpid(struct syscall_args *a) {
    (void)a;
    return current_proc()->pid;
}

static int64_t sys_cpu_count(struct syscall_args *a) {
    (void)a;
    return smp_online_count();
}

static int64_t sys_getcpu(struct syscall_args *a) {
    (void)a;
    return (int)(this_cpu() - &cpus[0]);
}

static int64_t sys_yield(struct syscall_args *a) {
    (void)a;
    schedule();
    return 0;
}

static int64_t sys_spawn(struct syscall_args *a) {
    char path_buf[64];
    copy_user_path(a->a1, a->a2, path_buf, sizeof(path_buf));
    struct process *child = spawn(path_buf);
    return child ? child->pid : -1;
}

static int64_t sys_wait(struct syscall_args *a) {
    return wait_for_pid((int)a->a1);
}

static int64_t sys_open(struct syscall_args *a) {
    char path_buf[64];
    copy_user_path(a->a1, a->a2, path_buf, sizeof(path_buf));
    int flags = (int)a->a3;

    struct process *task = current_proc();
    int slot = fd_alloc(task);
    if (slot < 0) { return -EMFILE; }

    // Every failure exit from here on must hand the reserved slot back,
    // or a process that fails N opens loses N fds for good. fd_close is
    // safe on a slot that never got a vnode: it only drops a reference
    // if one is there.
    fs_lock_acquire();

    int err = 0;
    struct vnode *vn = vfs_resolve(path_buf, &err);
    if (!vn && (flags & O_CREAT)) {
        char name[VFS_NAME_MAX];
        struct vnode *dir = vfs_resolve_parent(path_buf, name, &err);
        if (!dir) { fs_lock_release(); fd_close(task, slot); return err; }
        uint64_t new_id;
        int rc = dir->mount->ops->create(dir, name, &new_id);
        if (rc != 0) { vnode_put(dir); fs_lock_release(); fd_close(task, slot); return rc; }
        vn = vnode_get(dir->mount, new_id);
        vnode_put(dir);
        if (!vn) { fs_lock_release(); fd_close(task, slot); return -ENFILE; }
    }
    if (!vn) { fs_lock_release(); fd_close(task, slot); return err; }
    if (vn->type == VNODE_DIR && (flags & (O_WRONLY | O_RDWR))) {
        vnode_put(vn);
        fs_lock_release();
        fd_close(task, slot);
        return -EISDIR;
    }
    if (flags & O_TRUNC) {
        vn->mount->ops->truncate(vn);
    }

    fs_lock_release();

    struct file_descriptor *f = fd_get(task, slot);
    if (!f) {
        vnode_put(vn);
        fd_close(task, slot);
        return -EBADF;
    }
    f->vn = vn;   // the reference vfs_resolve/vnode_get took is now the fd's
    f->ops = &vnode_file_ops;
    f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
    // Readable regardless of the open mode, which is what NeoOS has
    // always done -- O_WRONLY does not prevent a read. Recorded in
    // docs/stdlib.md rather than quietly changed here, since tightening
    // it would break existing programs for no benefit this milestone.
    f->readable = 1;
    f->position = (flags & O_APPEND) ? vn->size : 0;
    return slot;
}

static int64_t sys_close(struct syscall_args *a) {
    return fd_close(current_proc(), (int)a->a1);
}

static int64_t sys_mkdir(struct syscall_args *a) {
    char path_buf[64];
    copy_user_path(a->a1, a->a2, path_buf, sizeof(path_buf));
    char name[VFS_NAME_MAX];
    int err = 0;
    fs_lock_acquire();
    struct vnode *dir = vfs_resolve_parent(path_buf, name, &err);
    if (!dir) { fs_lock_release(); return err; }
    int rc = dir->mount->ops->mkdir(dir, name);
    vnode_put(dir);
    fs_lock_release();
    return rc;
}

static int64_t sys_unlink(struct syscall_args *a) {
    char path_buf[64];
    copy_user_path(a->a1, a->a2, path_buf, sizeof(path_buf));
    char name[VFS_NAME_MAX];
    int err = 0;
    fs_lock_acquire();
    struct vnode *dir = vfs_resolve_parent(path_buf, name, &err);
    if (!dir) { fs_lock_release(); return err; }
    int rc = dir->mount->ops->unlink(dir, name);
    vnode_put(dir);
    fs_lock_release();
    return rc;
}

static int64_t sys_lseek(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_lseek(f, a->a2, (int)a->a3);
}

static int64_t sys_fork(struct syscall_args *a) {
    struct thread *child = fork_task(a->frame);
    return child ? child->proc->pid : -1;
}

static int64_t sys_exec(struct syscall_args *a) {
    char path_buf[64];
    copy_user_path(a->a1, a->a2, path_buf, sizeof(path_buf));
    return exec_task(path_buf, a->frame) ? 0 : -1;
}

static int64_t sys_mount(struct syscall_args *a) {
    char source[16], target[VFS_MAX_PATH], fstype[16];
    copy_user_string(a->a1, source, sizeof(source));
    copy_user_string(a->a2, target, sizeof(target));
    copy_user_string(a->a3, fstype, sizeof(fstype));
    fs_lock_acquire();
    int rc = vfs_mount_fs(source, target, fstype);
    fs_lock_release();
    return rc;
}

static int64_t sys_umount(struct syscall_args *a) {
    char target[VFS_MAX_PATH];
    copy_user_path(a->a1, a->a2, target, sizeof(target));
    fs_lock_acquire();
    int rc = vfs_umount(target);
    fs_lock_release();
    return rc;
}

static int64_t sys_getdents(struct syscall_args *a) {
    int count = (int)a->a3;
    if (count <= 0) { return -EBADF; }
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_getdents(f, (struct dirent *)(uintptr_t)a->a2, count);
}

static int64_t sys_pipe2(struct syscall_args *a) {
    int *user_fds = (int *)(uintptr_t)a->a1;
    if (!user_fds) { return -EFAULT; }
    if (!user_range_writable((uint64_t)(uintptr_t)user_fds, 2 * sizeof(int))) {
        return -EFAULT;
    }
    int fds[2];
    int rc = pipe_create(fds, (int)a->a2);
    if (rc != 0) { return rc; }
    // Written only after both ends exist, so a failure leaves the
    // caller's array untouched rather than half-filled.
    user_fds[0] = fds[0];
    user_fds[1] = fds[1];
    return 0;
}

static int64_t sys_thread_create(struct syscall_args *a) {
    struct thread *t = thread_create(a->a1, a->a2);
    return t ? t->tid : -EAGAIN;
}

static int64_t sys_thread_exit(struct syscall_args *a) {
    thread_exit_self((int)a->a1);
    return 0; // unreachable
}

static int64_t sys_thread_join(struct syscall_args *a) {
    int code = 0;
    int rc = thread_join((int)a->a1, &code);
    if (rc == 0 && a->a2) {
        *(int *)(uintptr_t)a->a2 = code;
    }
    return rc;
}

static int64_t sys_thread_self(struct syscall_args *a) {
    (void)a;
    return current_thread()->tid;
}

static int64_t sys_rt_sigaction(struct syscall_args *a) {
    int sig = (int)a->a1;
    const struct k_sigaction *act = (const struct k_sigaction *)(uintptr_t)a->a2;
    struct k_sigaction *old = (struct k_sigaction *)(uintptr_t)a->a3;
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    // SIGKILL and SIGSTOP can be neither caught nor ignored.
    if (act && (sig == SIGKILL || sig == SIGSTOP)) { return -EINVAL; }

    struct process *p = current_proc();
    uint64_t fl = spin_lock_irqsave(&p->sig_lock);
    if (old) { *old = p->actions[sig]; }
    if (act) {
        p->actions[sig] = *act;
        p->actions[sig].mask &= ~SIGSET_UNBLOCKABLE;
    }
    spin_unlock_irqrestore(&p->sig_lock, fl);
    return 0;
}

static int64_t sys_rt_sigprocmask(struct syscall_args *a) {
    int how = (int)a->a1;
    const sigset_t_k *set = (const sigset_t_k *)(uintptr_t)a->a2;
    sigset_t_k *old = (sigset_t_k *)(uintptr_t)a->a3;
    struct thread *t = current_thread();
    if (old) { *old = t->blocked; }
    if (set) {
        sigset_t_k v = *set;
        if (how == SIG_BLOCK)        { t->blocked |= v;  }
        else if (how == SIG_UNBLOCK) { t->blocked &= ~v; }
        else if (how == SIG_SETMASK) { t->blocked = v;   }
        else                         { return -EINVAL;   }
        // SIGKILL and SIGSTOP are silently dropped from any mask, as
        // POSIX requires -- not an error.
        t->blocked &= ~SIGSET_UNBLOCKABLE;
    }
    return 0;
}

static int64_t sys_rt_sigpending(struct syscall_args *a) {
    sigset_t_k *out = (sigset_t_k *)(uintptr_t)a->a1;
    struct thread *t = current_thread();
    // Pending AND blocked: an unblocked pending signal would already
    // have been delivered.
    if (out) { *out = (t->pending | t->proc->pending) & t->blocked; }
    return 0;
}

static int64_t sys_rt_sigsuspend(struct syscall_args *a) {
    const sigset_t_k *mask = (const sigset_t_k *)(uintptr_t)a->a1;
    struct thread *t = current_thread();
    t->saved_blocked = t->blocked;
    t->in_sigsuspend = 1;
    t->blocked = (*mask) & ~SIGSET_UNBLOCKABLE;
    while (signal_next_deliverable(t) == 0) {
        if (waitq_sleep(&t->proc->sig_waiters, 0) == -EINTR) { break; }
    }
    // The delivery path restores saved_blocked into the frame it
    // builds, so sigsuspend always reports interruption.
    return -EINTR;
}

static int64_t sys_rt_sigtimedwait(struct syscall_args *a) {
    const sigset_t_k *set = (const sigset_t_k *)(uintptr_t)a->a1;
    struct siginfo *out   = (struct siginfo *)(uintptr_t)a->a2;
    const struct k_timespec *ts = (const struct k_timespec *)(uintptr_t)a->a3;
    if (!set) { return -EINVAL; }
    struct thread *t = current_thread();

    // Accept the wanted signals for the duration, so they become
    // pending rather than being delivered to a handler.
    sigset_t_k want = (*set) & ~SIGSET_UNBLOCKABLE;
    sigset_t_k saved = t->blocked;
    t->blocked |= want;

    uint64_t deadline = 0;
    if (ts) {
        uint64_t ticks = (uint64_t)ts->tv_sec * TIMER_HZ
                       + (uint64_t)ts->tv_nsec / (1000000000UL / TIMER_HZ);
        deadline = timer_ticks() + (ticks ? ticks : 1);
    }

    int64_t rc = -EAGAIN;
    for (;;) {
        int sig = signal_next_pending_in(t, want);
        if (sig) {
            struct siginfo info;
            signal_take_pending(t, sig, &info);
            if (out) { *out = info; }
            rc = sig;
            break;
        }
        int wrc = ts ? waitq_sleep_timeout(&t->proc->sig_waiters, 0, deadline)
                     : waitq_sleep(&t->proc->sig_waiters, 0);
        if (wrc == -ETIMEDOUT) { rc = -EAGAIN; break; }
        if (wrc == -EINTR)     { rc = -EINTR;  break; }
    }
    t->blocked = saved;
    return rc;
}

static int64_t sys_rt_sigqueueinfo(struct syscall_args *a) {
    int pid = (int)a->a1, sig = (int)a->a2;
    const struct siginfo *user = (const struct siginfo *)(uintptr_t)a->a3;
    struct siginfo info;
    siginfo_user(&info, sig, current_proc()->pid);
    if (user) {
        info.si_code = SI_QUEUE;
        info.fields.rt.si_value = user->fields.rt.si_value;
    }
    return signal_kill(pid, sig, &info);
}

static int64_t sys_rt_sigreturn(struct syscall_args *a) {
    signal_do_sigreturn(a->frame);
    return 0; // unreachable
}

static int64_t sys_sigaltstack(struct syscall_args *a) {
    const stack_t_k *ss = (const stack_t_k *)(uintptr_t)a->a1;
    stack_t_k *old = (stack_t_k *)(uintptr_t)a->a2;
    struct thread *t = current_thread();
    if (old) { *old = t->altstack; }
    if (ss) {
        if (ss->ss_size < 2048) { return -ENOMEM; }
        t->altstack = *ss;
    }
    return 0;
}

static int64_t sys_mmap(struct syscall_args *a) {
    // mmap takes SIX arguments. A handler receives only a1-a4, but
    // struct syscall_frame begins r9, r8 and syscall_entry.asm pushes
    // those BEFORE its argument shuffle -- so args 5 and 6 are
    // frame->r8 and frame->r9, and the ABI needs no extending.
    uint64_t addr = a->a1, len = a->a2;
    uint32_t prot = (uint32_t)a->a3, flags = (uint32_t)a->a4;
    int64_t  fd   = (int64_t)a->frame->r8;
    // Anonymous only this milestone; the dynamic linker adds
    // file-backed mappings when it needs them.
    if (!(flags & MAP_ANONYMOUS) || fd >= 0) { return -ENOSYS; }
    return vma_mmap(current_proc(), addr, len, prot, flags);
}

static int64_t sys_munmap(struct syscall_args *a) {
    return vma_munmap(current_proc(), a->a1, a->a2);
}

static int64_t sys_mprotect(struct syscall_args *a) {
    return vma_mprotect(current_proc(), a->a1, a->a2, (uint32_t)a->a3);
}

static int64_t sys_wait4(struct syscall_args *a) {
    int st = 0;
    int64_t rc = wait4((int)a->a1, &st, (int)a->a3);
    if (rc > 0 && a->a2) { *(int *)(uintptr_t)a->a2 = st; }
    return rc;
}

static int64_t sys_setpgid(struct syscall_args *a) {
    int pid  = (int)a->a1 ? (int)a->a1 : current_proc()->pid;
    int pgid = (int)a->a2 ? (int)a->a2 : pid;
    struct process *p = proc_find(pid);
    if (!p) { return -ESRCH; }
    p->pgid = pgid;
    return 0;
}

static int64_t sys_getpgid(struct syscall_args *a) {
    struct process *p = (int)a->a1 ? proc_find((int)a->a1) : current_proc();
    return p ? p->pgid : -ESRCH;
}

static int64_t sys_setsid(struct syscall_args *a) {
    (void)a;
    struct process *p = current_proc();
    p->sid  = p->pid;
    p->pgid = p->pid;
    return p->sid;
}

static int64_t sys_getsid(struct syscall_args *a) {
    struct process *p = (int)a->a1 ? proc_find((int)a->a1) : current_proc();
    return p ? p->sid : -ESRCH;
}

static int64_t sys_kill(struct syscall_args *a) {
    struct siginfo info;
    siginfo_user(&info, (int)a->a2, current_proc()->pid);
    return signal_kill((int)a->a1, (int)a->a2, &info);
}

static int64_t sys_tkill(struct syscall_args *a) {
    struct siginfo info;
    siginfo_user(&info, (int)a->a2, current_proc()->pid);
    return signal_tkill(0, (int)a->a1, (int)a->a2, &info);
}

static int64_t sys_tgkill(struct syscall_args *a) {
    struct siginfo info;
    siginfo_user(&info, (int)a->a3, current_proc()->pid);
    return signal_tkill((int)a->a1, (int)a->a2, (int)a->a3, &info);
}

static int64_t sys_futex(struct syscall_args *a) {
    // Linux's argument order, unchanged: uaddr, op, val, timeout. The
    // fifth and sixth (uaddr2, val3) belong to REQUEUE and the BITSET
    // operations, neither of which is implemented, so they are not read.
    return futex_op((uint32_t *)(uintptr_t)a->a1, (int)a->a2, (uint32_t)a->a3,
                    (const struct k_timespec *)(uintptr_t)a->a4);
}

// The table. Designated initialisers, so the number beside each entry
// IS the index -- a reordering of these lines cannot renumber a
// syscall, which a positional array would allow. Entries omitted here
// are null and dispatch as -ENOSYS.
//
// The name is not decoration: syscall_table_selftest reports by it, and
// an unimplemented number is far easier to diagnose from a log line
// naming the neighbours than from a bare number.
struct syscall_desc {
    syscall_handler fn;
    const char     *name;
};

static const struct syscall_desc syscall_table[SYS_MAX] = {
    [SYS_EXIT]            = { sys_exit,            "exit" },
    [SYS_WRITE]           = { sys_write,           "write" },
    [SYS_YIELD]           = { sys_yield,           "yield" },
    [SYS_GETPID]          = { sys_getpid,          "getpid" },
    [SYS_SPAWN]           = { sys_spawn,           "spawn" },
    [SYS_WAIT]            = { sys_wait,            "wait" },
    [SYS_READ]            = { sys_read,            "read" },
    [SYS_OPEN]            = { sys_open,            "open" },
    [SYS_CLOSE]           = { sys_close,           "close" },
    [SYS_MKDIR]           = { sys_mkdir,           "mkdir" },
    [SYS_UNLINK]          = { sys_unlink,          "unlink" },
    [SYS_LSEEK]           = { sys_lseek,           "lseek" },
    [SYS_FORK]            = { sys_fork,            "fork" },
    [SYS_EXEC]            = { sys_exec,            "exec" },
    [SYS_MOUNT]           = { sys_mount,           "mount" },
    [SYS_UMOUNT]          = { sys_umount,          "umount" },
    [SYS_GETDENTS]        = { sys_getdents,        "getdents" },
    [SYS_THREAD_CREATE]   = { sys_thread_create,   "thread_create" },
    [SYS_THREAD_EXIT]     = { sys_thread_exit,     "thread_exit" },
    [SYS_THREAD_JOIN]     = { sys_thread_join,     "thread_join" },
    [SYS_THREAD_SELF]     = { sys_thread_self,     "thread_self" },
    [SYS_RT_SIGACTION]    = { sys_rt_sigaction,    "rt_sigaction" },
    [SYS_RT_SIGPROCMASK]  = { sys_rt_sigprocmask,  "rt_sigprocmask" },
    [SYS_RT_SIGRETURN]    = { sys_rt_sigreturn,    "rt_sigreturn" },
    [SYS_RT_SIGPENDING]   = { sys_rt_sigpending,   "rt_sigpending" },
    [SYS_RT_SIGSUSPEND]   = { sys_rt_sigsuspend,   "rt_sigsuspend" },
    [SYS_RT_SIGTIMEDWAIT] = { sys_rt_sigtimedwait, "rt_sigtimedwait" },
    [SYS_RT_SIGQUEUEINFO] = { sys_rt_sigqueueinfo, "rt_sigqueueinfo" },
    [SYS_SIGALTSTACK]     = { sys_sigaltstack,     "sigaltstack" },
    [SYS_KILL]            = { sys_kill,            "kill" },
    [SYS_TKILL]           = { sys_tkill,           "tkill" },
    [SYS_TGKILL]          = { sys_tgkill,          "tgkill" },
    [SYS_WAIT4]           = { sys_wait4,           "wait4" },
    [SYS_SETPGID]         = { sys_setpgid,         "setpgid" },
    [SYS_GETPGID]         = { sys_getpgid,         "getpgid" },
    [SYS_SETSID]          = { sys_setsid,          "setsid" },
    [SYS_GETSID]          = { sys_getsid,          "getsid" },
    [SYS_MMAP]            = { sys_mmap,            "mmap" },
    [SYS_MUNMAP]          = { sys_munmap,          "munmap" },
    [SYS_MPROTECT]        = { sys_mprotect,        "mprotect" },
    [SYS_CPU_COUNT]       = { sys_cpu_count,       "cpu_count" },
    [SYS_GETCPU]          = { sys_getcpu,          "getcpu" },
    [SYS_FUTEX]           = { sys_futex,           "futex" },
    [SYS_PIPE2]           = { sys_pipe2,           "pipe2" },
};

// Asserts what the table's shape is supposed to guarantee. Cheap, and
// it catches the one mistake designated initialisers still allow:
// SYS_MAX left behind when a syscall is added, which truncates the
// table silently and turns the new call into -ENOSYS at run time.
void syscall_table_selftest(void) {
    int implemented = 0;
    for (int i = 0; i < SYS_MAX; i++) {
        if (syscall_table[i].fn && !syscall_table[i].name) {
            serial_write_string("[syscall] table selftest FAILED: unnamed handler at ");
            serial_write_hex64((uint64_t)i);
            serial_write_string("\n");
            return;
        }
        if (syscall_table[i].fn) { implemented++; }
    }
    // The highest number in use must be the last slot. A gap at the end
    // means SYS_MAX is larger than the table's contents (harmless), but
    // a MISSING last slot means SYS_MAX is too small and the syscall
    // that should live there was silently dropped.
    if (!syscall_table[SYS_MAX - 1].fn) {
        serial_write_string("[syscall] table selftest FAILED: SYS_MAX overshoots the last handler\n");
        return;
    }
    serial_write_string("[syscall] table selftest passed, implemented=");
    serial_write_hex64((uint64_t)implemented);
    serial_write_string(" of ");
    serial_write_hex64((uint64_t)SYS_MAX);
    serial_write_string("\n");
}

// Called only from syscall_entry.asm's `call syscall_dispatch`, via
// syscall_dispatch below.
static int64_t syscall_dispatch_inner(int64_t num, struct syscall_args *args) {
    // One unsigned compare covers both ends: a negative number becomes
    // enormous and fails the same test.
    if ((uint64_t)num >= SYS_MAX || !syscall_table[num].fn) {
        // -ENOSYS, not -1. Linux's answer for an unimplemented call,
        // and musl's feature probes depend on being able to tell "this
        // kernel lacks the call" from "the call failed".
        return -ENOSYS;
    }
    return syscall_table[num].fn(args);
}

// Single exit point for every syscall. A thread killed by a sibling's
// exit() unwinds to here: whatever it was doing has finished, and it
// must not return to user mode.
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3,
                         int64_t a4, struct syscall_frame *frame) {
    struct syscall_args args = { a1, a2, a3, a4, frame };
    int64_t ret = syscall_dispatch_inner(num, &args);
    return signal_deliver_from_syscall(frame, num, ret);
}

void syscall_init(void) {
    syscall_init_this_cpu();
    serial_write_string("[syscall] SYSCALL/SYSRET configured\n");
}

// EFER, STAR, LSTAR and SFMASK are all PER-CPU MSRs -- writing them on
// the BSP configures the BSP and nothing else. Every CPU that may ever
// return to ring 3 has to program them for itself, so ap_main calls
// this too. A user thread that reaches an AP whose MSRs were never
// written SYSCALLs into an unconfigured LSTAR; observed as every
// userland suite producing no output at all, with no exception logged.
//
// Reachable without work stealing: enqueue_ready() targets this_cpu(),
// so a user thread woken by a kernel thread running on an AP is queued
// -- and then run -- there.
void syscall_init_this_cpu(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    // EFER_NXE: elf_load (Task 5) is the first code in NeoOS to
    // actually set PAGE_NO_EXECUTE (bit 63) on a real PTE -- without
    // NXE enabled, that bit is reserved and setting it faults the
    // moment the page is walked. Grouped here since it's the same MSR
    // as EFER_SCE, not because it's conceptually part of SYSCALL setup.
    wrmsr(MSR_EFER, efer | EFER_SCE | EFER_NXE);

    // STAR[47:32] = kernel CS (kernel SS = that + 8, matches
    // GDT_KERNEL_DATA_SELECTOR at 0x10); STAR[63:48] = the SYSRET
    // base (user data at that+8, user code64 at that+16 -- see
    // Task 1's GDT layout).
    uint64_t star = ((uint64_t)GDT_USER_CODE32_SELECTOR << 48) | ((uint64_t)GDT_KERNEL_CODE_SELECTOR << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200); // mask IF (bit 9) on syscall entry

    // Read BACK, so the selftest below asserts what the hardware holds
    // rather than what we believe we wrote.
    syscall_msr_seen[this_cpu() - &cpus[0]] = rdmsr(MSR_LSTAR);
}

// Asserts the invariant the split above exists to maintain: every
// online CPU has its own LSTAR pointing at syscall_entry. Silent
// breakage here shows up as a userland suite that emits nothing at all
// and logs no exception, which is close to undiagnosable from the
// serial log -- hence a direct check.
void syscall_msr_selftest(void) {
    int online = smp_online_count();
    for (int i = 0; i < online; i++) {
        if (syscall_msr_seen[i] != (uint64_t)(uintptr_t)syscall_entry) {
            serial_write_string("[syscall] msr selftest FAILED: cpu=");
            serial_write_hex64((uint64_t)i);
            serial_write_string(" lstar=");
            serial_write_hex64(syscall_msr_seen[i]);
            serial_write_string("\n");
            return;
        }
    }
    serial_write_string("[syscall] per-cpu msr selftest passed, cpus=");
    serial_write_hex64((uint64_t)online);
    serial_write_string("\n");
}
