// kernel/syscall/sys_file.c -- File descriptors, paths and directories.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "dev/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "fs/stat.h"
#include "dev/tty.h"
#include "fs/devfs.h"
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

// The four fd operations are now four lines each. Whether the target
// is a file, a pipe or (later) a socket is the file layer's business;
// none of them can grow a special case for one kind of object without
// that being conspicuous.
int64_t sys_write(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_write(f, (const void *)(uintptr_t)a->a2, (uint64_t)a->a3);
}

int64_t sys_read(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_read(f, (void *)(uintptr_t)a->a2, (uint64_t)a->a3);
}

int64_t sys_open(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }
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
    f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
    // Readable regardless of the open mode, which is what NeoOS has
    // always done -- O_WRONLY does not prevent a read. Recorded in
    // docs/stdlib.md rather than quietly changed here, since tightening
    // it would break existing programs for no benefit this milestone.
    f->readable = 1;
    f->position = (flags & O_APPEND) ? vn->size : 0;

    // Device files use per-device file_ops implementations, not
    // vnode_file_ops; file_bind_vnode_ops picks the right table and runs
    // the device's open hook.
    int64_t brc = file_bind_vnode_ops(f);
    if (brc != 0) {
        vnode_put(vn);
        fd_close(task, slot);
        return brc;
    }

    return slot;
}

int64_t sys_close(struct syscall_args *a) {
    return fd_close(current_proc(), (int)a->a1);
}

int64_t sys_mkdir(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }
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

int64_t sys_unlink(struct syscall_args *a) {
    char path_buf[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, path_buf);
    if (prc != 0) { return prc; }
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

int64_t sys_lseek(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_lseek(f, a->a2, (int)a->a3);
}

// getdents64(fd, buf, byte_count) -> bytes written, 0 at end.
//
// The third argument is a BYTE count now, not an entry count: Linux's
// records are variable length, so an entry count could not describe
// how much room the caller actually has.
int64_t sys_getdents(struct syscall_args *a) {
    int bytes = (int)a->a3;
    if (bytes <= 0) { return -EINVAL; }
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    void *buf = (void *)(uintptr_t)a->a2;
    if (!buf) { return -EFAULT; }
    return file_getdents(f, buf, bytes);
}

int64_t sys_fcntl(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }

    switch ((int)a->a2) {
    case F_GETFL:
        // Only O_NONBLOCK is tracked. The access mode a real F_GETFL
        // also reports is not recorded per-fd here, and reporting a
        // made-up one would be worse than reporting none.
        return f->nonblock ? O_NONBLOCK : 0;
    case F_SETFL:
        // POSIX: only a few flags are settable, and the rest are
        // ignored rather than rejected -- which is what lets
        // `fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)` work
        // without the caller knowing which bits it just passed back.
        f->nonblock = ((int)a->a3 & O_NONBLOCK) ? 1 : 0;
        return 0;
    case F_GETFD:
        return 0;   // FD_CLOEXEC is never set; there is no exec-time walk
    case F_SETFD:
        return 0;   // accepted and ignored, for the same reason
    default:
        // F_DUPFD, the locking commands, and everything else. Refusing
        // is right: a caller that asked for a duplicate and got a
        // silent success would use fd -1 as if it were open.
        return -EINVAL;
    }
}

int64_t sys_mount(struct syscall_args *a) {
    char source[16], target[VFS_MAX_PATH], fstype[16];
    copy_user_string(a->a1, source, sizeof(source));
    copy_user_string(a->a2, target, sizeof(target));
    copy_user_string(a->a3, fstype, sizeof(fstype));
    // The mount point is a path like any other, so a relative one has
    // to mean the same thing here as it does to open().
    char target_abs[VFS_MAX_PATH];
    struct process *task = current_proc();
    int prc = vfs_path_canonicalise(task ? task->cwd : "/", target, target_abs);
    if (prc != 0) { return prc; }
    fs_lock_acquire();
    int rc = vfs_mount_fs(source, target_abs, fstype);
    fs_lock_release();
    return rc;
}

int64_t sys_umount(struct syscall_args *a) {
    char target[VFS_MAX_PATH];
    int prc = copy_user_path_at(a->a1, a->a2, target);
    if (prc != 0) { return prc; }
    fs_lock_acquire();
    int rc = vfs_umount(target);
    fs_lock_release();
    return rc;
}

int64_t sys_chdir(struct syscall_args *a) {
    char path[VFS_MAX_PATH];
    int rc = copy_user_path_at(a->a1, a->a2, path);
    if (rc != 0) { return rc; }

    // The directory must EXIST and BE a directory before it becomes the
    // cwd. Skipping the check would let a process carry a cwd that
    // every later relative path then fails against, with the error
    // surfacing far from the chdir that caused it.
    fs_lock_acquire();
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) { fs_lock_release(); return err; }
    if (vn->type != VNODE_DIR) {
        vnode_put(vn);
        fs_lock_release();
        return -ENOTDIR;
    }
    vnode_put(vn);
    fs_lock_release();

    struct process *task = current_proc();
    uint64_t i = 0;
    for (; i < VFS_MAX_PATH - 1 && path[i]; i++) { task->cwd[i] = path[i]; }
    task->cwd[i] = '\0';
    return 0;
}

// Linux's getcwd returns the LENGTH INCLUDING the NUL on success, and
// -ERANGE if the buffer is too small. musl's getcwd checks for exactly
// that, so both are reproduced here rather than the more obvious
// "return 0 on success".
int64_t sys_getcwd(struct syscall_args *a) {
    struct process *task = current_proc();
    const char *cwd = task ? task->cwd : "/";

    uint64_t len = 0;
    while (cwd[len]) { len++; }
    len++;   // the NUL is part of what Linux counts

    uint64_t size = (uint64_t)a->a2;
    if (size < len) { return -ERANGE; }

    char *out = (char *)(uintptr_t)a->a1;
    if (!out) { return -EFAULT; }
    for (uint64_t i = 0; i < len; i++) { out[i] = cwd[i]; }
    return (int64_t)len;
}

int64_t sys_pipe2(struct syscall_args *a) {
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

// ---- the stat family -------------------------------------------------
//
// All four share this: resolve to a vnode, fill the struct, release the
// vnode. They differ only in HOW the vnode is reached.

static int64_t stat_by_path(int64_t uptr, int64_t ulen, int64_t out_ptr) {
    struct stat *out = (struct stat *)(uintptr_t)out_ptr;
    if (!out) { return -EFAULT; }

    char path[VFS_MAX_PATH];
    int rc = copy_user_path_at(uptr, ulen, path);
    if (rc != 0) { return rc; }

    fs_lock_acquire();
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) { fs_lock_release(); return err; }

    struct stat st;
    vfs_stat_vnode(vn, &st);
    vnode_put(vn);
    fs_lock_release();

    *out = st;
    return 0;
}

int64_t sys_stat(struct syscall_args *a) {
    return stat_by_path(a->a1, a->a2, a->a3);
}

// DIVERGES, harmlessly: identical to stat. lstat differs only on a
// symlink, and no filesystem NeoOS mounts can represent one -- FAT has
// no such entry type. Recorded in docs/stdlib.md.
int64_t sys_lstat(struct syscall_args *a) {
    return stat_by_path(a->a1, a->a2, a->a3);
}

int64_t sys_fstat(struct syscall_args *a) {
    struct stat *out = (struct stat *)(uintptr_t)a->a2;
    if (!out) { return -EFAULT; }

    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    // A pipe or a socket has no vnode. Reporting one would mean
    // inventing an inode; -EINVAL says plainly that this fd is not a
    // file, which is what the caller needs to know.
    if (!f->vn) { return -EINVAL; }

    fs_lock_acquire();
    struct stat st;
    vfs_stat_vnode(f->vn, &st);
    fs_lock_release();

    *out = st;
    return 0;
}

// newfstatat(dirfd, path_ptr, path_len, statbuf, flags).
//
// FIVE arguments, so `flags` arrives in frame->r8 -- the same route
// mmap's fifth argument takes. The path is a (pointer, length) pair
// here as in every other NeoOS path syscall, which is what pushes the
// count past four.
//
// Only AT_FDCWD is accepted for dirfd. There is no openat family yet,
// so nothing in userland can hold a directory fd to pass here, and a
// real dirfd would need resolution to start somewhere other than a
// mount root -- the one thing vfs_path_canonicalise deliberately does
// not do. -EBADF is the honest answer rather than silently treating an
// arbitrary fd as the cwd.
int64_t sys_newfstatat(struct syscall_args *a) {
    int dirfd = (int)a->a1;
    int flags = (int)a->frame->r8;

    // AT_EMPTY_PATH with a real fd is Linux's spelling of fstat.
    if ((flags & AT_EMPTY_PATH) && dirfd >= 0) {
        struct syscall_args fa = { dirfd, a->a4, 0, 0, a->frame };
        return sys_fstat(&fa);
    }
    if (dirfd != AT_FDCWD) { return -EBADF; }

    // AT_SYMLINK_NOFOLLOW is accepted and ignored: with no symlinks to
    // follow, following and not following give the same answer.
    return stat_by_path(a->a2, a->a3, a->a4);
}

// ---- scatter/gather I/O ---------------------------------------------
//
// musl's stdio writes through writev and NOTHING else: __stdio_write
// builds a two-element iovec (the buffer it holds, plus whatever the
// caller passed) and issues one call. Without this, a musl program
// produces no output at all.
//
// This is a real scatter/gather call, not a loop dressed up as one from
// the caller's side, but it IS implemented as a loop over the vectors
// -- the underlying file ops are single-buffer, and a partial write of
// one vector correctly ends the whole call.

#define IOV_MAX_NEOOS 16

struct iovec_user {
    uint64_t iov_base;
    uint64_t iov_len;
};

static int64_t rw_vectored(struct syscall_args *a, int writing) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }

    const struct iovec_user *iov = (const struct iovec_user *)(uintptr_t)a->a2;
    int n = (int)a->a3;
    if (!iov && n != 0) { return -EFAULT; }
    if (n < 0) { return -EINVAL; }
    // Linux caps this at IOV_MAX (1024) with -EINVAL. NeoOS's cap is
    // lower and reported the same way, rather than silently truncating.
    if (n > IOV_MAX_NEOOS) { return -EINVAL; }

    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        uint64_t base = iov[i].iov_base;
        uint64_t len  = iov[i].iov_len;
        if (len == 0) { continue; }
        if (!base) { return total > 0 ? total : -EFAULT; }

        int64_t rc = writing ? file_write(f, (const void *)(uintptr_t)base, len)
                             : file_read(f, (void *)(uintptr_t)base, len);
        if (rc < 0) {
            // Bytes already transferred are reported; the error surfaces
            // on the next call. That is Linux's rule, and stdio depends
            // on it to keep its buffer consistent.
            return total > 0 ? total : rc;
        }
        total += rc;
        if ((uint64_t)rc < len) { break; }   // short write/read ends the call
    }
    return total;
}

int64_t sys_writev(struct syscall_args *a) { return rw_vectored(a, 1); }
int64_t sys_readv(struct syscall_args *a)  { return rw_vectored(a, 0); }

// ---- ioctl -----------------------------------------------------------
//
// Dispatches through the file_ops table, so the syscall layer is
// indifferent to whether the fd points to a TTY, a regular file, a pipe,
// or anything else. Each implementation supplies truthful responses:
// TTY answers ioctl requests; regular files/pipes/sockets return -ENOTTY.

int64_t sys_ioctl(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_ioctl(f, (uint64_t)a->a2, (void *)(uintptr_t)a->a3);
}
