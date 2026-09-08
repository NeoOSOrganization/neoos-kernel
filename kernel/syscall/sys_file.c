// kernel/syscall/sys_file.c -- File descriptors, paths and directories.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "drivers/char/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "fs/stat.h"
#include "tty/tty.h"
#include "fs/devfs.h"
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
#include "mm/pmm.h"
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "net/socket.h"

// Bounced through a kernel staging buffer: file_read/file_write's
// underlying vnode/device implementations (fatfs_read, evdev_fop_read,
// ...) dereference their `buf` argument directly with no user-copy
// awareness of their own, so the safe copy has to happen at THIS
// layer. STAGE_MAX caps the staging buffer to one page -- larger
// reads/writes loop, each chunk safely copied to/from user memory
// after the underlying read/write completes. read_to_user is the
// exact call site that crashed on Doom's WAD load (kernel/fs/fatfs.c's
// fat16_read_at_v, confirmed via addr2line against the fault RIP --
// see docs/superpowers/specs/2026-09-06-fault-tolerant-user-copy-design.md's
// Trigger section). Shared by sys_read/sys_write and rw_vectored
// (readv/writev), which is why these are defined up here rather than
// inlined into sys_read/sys_write directly.
#define STAGE_MAX 4096

static int64_t read_to_user(struct file_descriptor *f, uint64_t uptr, uint64_t remaining) {
    uint8_t stage[STAGE_MAX];
    int64_t total = 0;
    while (remaining > 0) {
        uint64_t chunk = remaining < STAGE_MAX ? remaining : STAGE_MAX;
        int64_t rc = file_read(f, stage, chunk);
        if (rc < 0) { return total > 0 ? total : rc; }
        if (rc == 0) { break; }   // EOF
        uint64_t missed = copy_to_user((void *)(uintptr_t)(uptr + (uint64_t)total), stage, (uint64_t)rc);
        if (missed > 0) {
            // Partial copy: report what genuinely reached user memory.
            total += (int64_t)((uint64_t)rc - missed);
            return total > 0 ? total : -EFAULT;
        }
        total += rc;
        remaining -= (uint64_t)rc;
        if ((uint64_t)rc < chunk) { break; }   // short read from the underlying object ends the call
    }
    return total;
}

static int64_t write_from_user(struct file_descriptor *f, uint64_t uptr, uint64_t remaining) {
    uint8_t stage[STAGE_MAX];
    int64_t total = 0;
    while (remaining > 0) {
        uint64_t chunk = remaining < STAGE_MAX ? remaining : STAGE_MAX;
        uint64_t missed = copy_from_user(stage, (const void *)(uintptr_t)(uptr + (uint64_t)total), chunk);
        uint64_t got = chunk - missed;
        if (got == 0) { return total > 0 ? total : -EFAULT; }
        int64_t rc = file_write(f, stage, got);
        if (rc < 0) { return total > 0 ? total : rc; }
        total += rc;
        remaining -= (uint64_t)rc;
        if ((uint64_t)rc < got || missed > 0) { break; }   // short write, or the source had a bad page: end here
    }
    return total;
}

// The four fd operations are now four lines each. Whether the target
// is a file, a pipe or (later) a socket is the file layer's business;
// none of them can grow a special case for one kind of object without
// that being conspicuous.
int64_t sys_write(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return write_from_user(f, (uint64_t)a->a2, (uint64_t)a->a3);
}

int64_t sys_read(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return read_to_user(f, (uint64_t)a->a2, (uint64_t)a->a3);
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

int64_t sys_dup(struct syscall_args *a) {
    struct process *p = current_proc();
    int oldfd = (int)a->a1;
    if (!fd_get(p, oldfd)) { return -EBADF; }
    int nf = fd_alloc(p);
    if (nf < 0) { return nf; }
    int64_t rc = fd_table_dup2(p->fd_table, oldfd, nf);
    if (rc < 0) { fd_close(p, nf); }
    return rc;
}

// dup2(old, new): if new == old and old is valid, return new unchanged.
int64_t sys_dup2(struct syscall_args *a) {
    struct process *p = current_proc();
    int oldfd = (int)a->a1, newfd = (int)a->a2;
    if (!fd_get(p, oldfd)) { return -EBADF; }
    if (oldfd == newfd) { return newfd; }
    return fd_table_dup2(p->fd_table, oldfd, newfd);
}

// dup3(old, new, flags): like dup2 but new == old is an error, and
// O_CLOEXEC (0x80000) is the only accepted flag (recorded, not acted
// on -- NeoOS does not close fds at exec yet).
int64_t sys_dup3(struct syscall_args *a) {
    struct process *p = current_proc();
    int oldfd = (int)a->a1, newfd = (int)a->a2;
    int flags = (int)a->a3;
    if (flags & ~0x80000) { return -EINVAL; }
    if (oldfd == newfd) { return -EINVAL; }
    if (!fd_get(p, oldfd)) { return -EBADF; }
    return fd_table_dup2(p->fd_table, oldfd, newfd);
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
    uint64_t uptr = a->a2;
    if (!uptr) { return -EFAULT; }

    // Page-capped: file_getdents fills the buffer directly with
    // Linux-shaped dirent records (variable length, so it must pick
    // how many fit itself) -- one bounded staging call gives it a
    // buffer to fill and then a single copy_to_user relays exactly
    // what it wrote, same as every other direct-buffer object here.
    uint64_t chunk = (uint64_t)bytes < STAGE_MAX ? (uint64_t)bytes : STAGE_MAX;
    uint8_t stage[STAGE_MAX];
    int64_t rc = file_getdents(f, stage, (int)chunk);
    if (rc <= 0) { return rc; }
    uint64_t missed = copy_to_user((void *)(uintptr_t)uptr, stage, (uint64_t)rc);
    if (missed > 0) { return -EFAULT; }
    return rc;
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
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        // The lowest free descriptor >= a3, pointing at the same open
        // object. BusyBox's ash does exactly this to move the terminal
        // out of the way of a script's own redirections
        // (`fcntl(fd, F_DUPFD_CLOEXEC, 10)`), and gave up on job
        // control entirely when it came back -EINVAL: "can't access
        // tty; job control turned off".
        //
        // CLOEXEC is accepted and ignored, for the same reason F_SETFD
        // is: nothing walks the descriptor table at exec yet. Recorded
        // in docs/stdlib.md.
        int from = (int)a->a3;
        if (from < 0 || from >= FD_TABLE_MAX) { return -EINVAL; }
        int newfd = fd_table_alloc_from(current_proc()->fd_table, from);
        if (newfd < 0) { return newfd; }
        int rc = (int)fd_table_dup2(current_proc()->fd_table, (int)a->a1, newfd);
        if (rc < 0) { fd_table_close(current_proc()->fd_table, newfd); }
        return rc;
    }
    default:
        // The locking commands and everything else. Refusing is right:
        // a caller that asked for something and got a silent success
        // would act on a result it never received.
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

    uint64_t out = (uint64_t)a->a1;
    if (!out) { return -EFAULT; }
    uint64_t missed = copy_to_user((void *)(uintptr_t)out, cwd, len);
    if (missed > 0) { return -EFAULT; }
    return (int64_t)len;
}

int64_t sys_pipe2(struct syscall_args *a) {
    uint64_t user_fds = a->a1;
    if (!user_fds) { return -EFAULT; }
    int fds[2];
    int rc = pipe_create(fds, (int)a->a2);
    if (rc != 0) { return rc; }
    // Written only after both ends exist, so a failure leaves the
    // caller's array untouched rather than half-filled.
    uint64_t missed = copy_to_user((void *)(uintptr_t)user_fds, fds, sizeof fds);
    if (missed > 0) { return -EFAULT; }
    return 0;
}

// ---- the stat family -------------------------------------------------
//
// All four share this: resolve to a vnode, fill the struct, release the
// vnode. They differ only in HOW the vnode is reached.

static int64_t stat_by_path(int64_t uptr, int64_t ulen, int64_t out_ptr) {
    if (!out_ptr) { return -EFAULT; }

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

    // fs_lock (LOCK_RANK_MOUNTTABLE = 4) is released above, before this
    // touches user memory: copy_to_user's fault path takes mm_lock
    // (LOCK_RANK_MM = 3), and 3 < 4 would be a rank violation held live.
    uint64_t missed = copy_to_user((void *)(uintptr_t)out_ptr, &st, sizeof st);
    if (missed > 0) { return -EFAULT; }
    return 0;
}

int64_t sys_stat(struct syscall_args *a) {
    return stat_by_path(a->a1, a->a2, a->a3);
}

// ---- statx (MSC-3) --------------------------------------------------
//
// The extended stat -- git, cargo, coreutils >= 9 and musl's own stat
// fast path use it. NeoOS fills STATX_BASIC_STATS from the same vnode
// data as stat(2); birth time is left unreported (FAT gives NeoOS no
// timestamps to read -- the same gap stat has, recorded in
// docs/stdlib.md). Layout is Linux's x86_64 struct statx, 256 bytes.

struct statx_timestamp { int64_t tv_sec; uint32_t tv_nsec; int32_t __reserved; };
struct k_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2[13];
};
_Static_assert(sizeof(struct k_statx) == 256, "struct statx must be 256 bytes");

#define STATX_BASIC_STATS 0x000007ffU

int64_t sys_statx(struct syscall_args *a) {
    int      dirfd  = (int)a->a1;
    int      flags  = (int)a->a4;
    uint64_t out_ptr = a->frame->r9;
    if (!out_ptr) { return -EFAULT; }

    struct stat st;
    for (unsigned i = 0; i < sizeof(st); i++) { ((uint8_t *)&st)[i] = 0; }

    if ((flags & AT_EMPTY_PATH) && dirfd >= 0) {
        struct file_descriptor *f = fd_get(current_proc(), dirfd);
        if (!f) { return -EBADF; }
        if (!f->vn) { return -EINVAL; }
        fs_lock_acquire();
        vfs_stat_vnode(f->vn, &st);
        fs_lock_release();
    } else {
        if (dirfd != AT_FDCWD) { return -EBADF; }
        char path[VFS_MAX_PATH];
        int rc = copy_user_path_at(a->a2, a->a3, path);
        if (rc != 0) { return rc; }
        fs_lock_acquire();
        int err = 0;
        struct vnode *vn = vfs_resolve(path, &err);
        if (!vn) { fs_lock_release(); return err; }
        vfs_stat_vnode(vn, &st);
        vnode_put(vn);
        fs_lock_release();
    }

    struct k_statx sx;
    for (unsigned i = 0; i < sizeof(sx); i++) { ((uint8_t *)&sx)[i] = 0; }
    sx.stx_mask     = STATX_BASIC_STATS;   // no btime -> that bit stays clear below
    sx.stx_mask    &= ~0x800U;             // STATX_BTIME
    sx.stx_blksize  = (uint32_t)st.st_blksize;
    sx.stx_nlink    = (uint32_t)st.st_nlink;
    sx.stx_uid      = st.st_uid;
    sx.stx_gid      = st.st_gid;
    sx.stx_mode     = (uint16_t)st.st_mode;
    sx.stx_ino      = st.st_ino;
    sx.stx_size     = (uint64_t)st.st_size;
    sx.stx_blocks   = (uint64_t)st.st_blocks;
    sx.stx_attributes_mask = 0;
    sx.stx_atime.tv_sec = st.st_atime_sec;
    sx.stx_atime.tv_nsec = (uint32_t)st.st_atime_nsec;
    sx.stx_ctime.tv_sec = st.st_ctime_sec;
    sx.stx_ctime.tv_nsec = (uint32_t)st.st_ctime_nsec;
    sx.stx_mtime.tv_sec = st.st_mtime_sec;
    sx.stx_mtime.tv_nsec = (uint32_t)st.st_mtime_nsec;

    if (copy_to_user((void *)(uintptr_t)out_ptr, &sx, sizeof sx) != 0) {
        return -EFAULT;
    }
    return 0;
}

// DIVERGES, harmlessly: identical to stat. lstat differs only on a
// symlink, and no filesystem NeoOS mounts can represent one -- FAT has
// no such entry type. Recorded in docs/stdlib.md.
int64_t sys_lstat(struct syscall_args *a) {
    return stat_by_path(a->a1, a->a2, a->a3);
}

int64_t sys_fstat(struct syscall_args *a) {
    uint64_t out = (uint64_t)a->a2;
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

    uint64_t missed = copy_to_user((void *)(uintptr_t)out, &st, sizeof st);
    if (missed > 0) { return -EFAULT; }
    return 0;
}

// readlink(path, buf, bufsize) -- the path must resolve (a bad path is
// -ENOENT, same as stat), but the answer past that is always -EINVAL:
// no filesystem NeoOS mounts can represent a symlink (the same
// divergence lstat already records above), so nothing this call could
// name is ever one. Found missing getting a real ASP.NET Core app
// running -- the generic host's startup path calls it, tolerates the
// failure, and falls back (unlike inotify_init1's fatal absence, just
// below).
int64_t sys_readlink(struct syscall_args *a) {
    char path[VFS_MAX_PATH];
    int rc = copy_user_path_at(a->a1, a->a2, path);
    if (rc != 0) { return rc; }

    fs_lock_acquire();
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) { fs_lock_release(); return err; }
    vnode_put(vn);
    fs_lock_release();
    return -EINVAL;
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

// statfs(path, buf) -- Linux shape, matching musl's struct statfs
// (include/bits/statfs.h) field-for-field. The path must resolve (a
// bad path is -ENOENT/etc, same as stat) but the filesystem-specific
// fields are a generic, best-effort answer: NeoOS mounts several
// unrelated filesystems (FAT, ramfs, devfs, procfs, embedfs) with no
// single shared free-space or magic-number concept to report, so
// f_type is 0 (no magic this implementation claims to match) and the
// block counts come from kernel/mm/pmm.h's frame counters -- an
// honest number, just not literally "free space on this path's own
// filesystem". Found alongside sysinfo/get_mempolicy, chasing dotnet
// NativeAOT's CoreCLR startup one syscall at a time. See docs/stdlib.md.
struct neoos_statfs {
    uint64_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail;
    uint64_t f_files, f_ffree;
    uint32_t f_fsid[2];
    uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};

int64_t sys_statfs(struct syscall_args *a) {
    int64_t uptr = a->a1, ulen = a->a2, out_ptr = a->a3;
    if (!out_ptr) { return -EFAULT; }

    char path[VFS_MAX_PATH];
    int rc = copy_user_path_at(uptr, ulen, path);
    if (rc != 0) { return rc; }

    fs_lock_acquire();
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) { fs_lock_release(); return err; }
    vnode_put(vn);
    fs_lock_release();

    struct neoos_statfs sf;
    for (unsigned i = 0; i < sizeof(sf); i++) { ((uint8_t *)&sf)[i] = 0; }
    sf.f_bsize   = PMM_FRAME_SIZE;
    sf.f_frsize  = PMM_FRAME_SIZE;
    sf.f_blocks  = pmm_total_frame_count();
    sf.f_bfree   = pmm_free_frame_count();
    sf.f_bavail  = sf.f_bfree;
    sf.f_namelen = 255;

    uint64_t missed = copy_to_user((void *)(uintptr_t)out_ptr, &sf, sizeof sf);
    if (missed > 0) { return -EFAULT; }
    return 0;
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

    uint64_t iov_uptr = a->a2;
    int n = (int)a->a3;
    if (!iov_uptr && n != 0) { return -EFAULT; }
    if (n < 0) { return -EINVAL; }
    // Linux caps this at IOV_MAX (1024) with -EINVAL. NeoOS's cap is
    // lower and reported the same way, rather than silently truncating.
    if (n > IOV_MAX_NEOOS) { return -EINVAL; }

    // The iovec array itself lives in user memory -- indexing it
    // directly (the pre-uaccess code above did exactly that) is the
    // same class of bug as the WAD-load crash, just on the descriptor
    // array instead of the data buffer. Bounced through a stack copy
    // once, up front: IOV_MAX_NEOOS is small enough that this needs no
    // staging loop of its own.
    struct iovec_user iov[IOV_MAX_NEOOS];
    if (n > 0) {
        uint64_t missed = copy_from_user(iov, (const void *)(uintptr_t)iov_uptr,
                                          (uint64_t)n * sizeof(struct iovec_user));
        if (missed > 0) { return -EFAULT; }
    }

    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        uint64_t base = iov[i].iov_base;
        uint64_t len  = iov[i].iov_len;
        if (len == 0) { continue; }
        if (!base) { return total > 0 ? total : -EFAULT; }

        int64_t rc = writing ? write_from_user(f, base, len)
                             : read_to_user(f, base, len);
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

// ---- fsync / fdatasync / fallocate / access (MSC-3) ------------------

// NeoOS's block cache writes through -- there is no dirty-writeback
// list to flush (kernel/fs/blkcache.c's own comment). fsync/fdatasync
// therefore only need to validate the fd and succeed; the data a
// successful write() returned from is already on the device.
// Recorded in docs/stdlib.md.
// ftruncate(fd, length). The first size-setting operation the vnode
// layer has: vfs_ops.truncate only ever meant "to zero" (O_TRUNC), which
// is why fallocate below still answers -EOPNOTSUPP.
//
// Linux takes an off_t, so a negative length is -EINVAL rather than a
// nine-exabyte file. Growing zero-fills; shrinking discards.
int64_t sys_ftruncate(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }

    int64_t len = (int64_t)a->a2;
    if (len < 0) { return -EINVAL; }

    // An object with a length of its own (memfd) answers first; only a
    // vnode-backed file falls through to its filesystem.
    int64_t orc = file_truncate(f, (uint64_t)len);
    if (orc != -EINVAL) { return orc; }

    if (!f->vn) { return -EINVAL; }          // pipe, socket, tty: no size
    if (f->vn->type == VNODE_DIR) { return -EISDIR; }
    if (!f->vn->mount || !f->vn->mount->ops->truncate_to) { return -EINVAL; }

    int rc = f->vn->mount->ops->truncate_to(f->vn, (uint64_t)len);
    if (rc == 0 && f->vn->mount->ops->sync_inode) {
        f->vn->mount->ops->sync_inode(f->vn);
    }
    return rc;
}

int64_t sys_fsync(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return 0;
}

// fallocate(fd, mode, offset, len). NeoOS's filesystems cannot
// preallocate or punch holes. The vnode layer does now have a
// size-setting operation (truncate_to, added for ftruncate), so the
// plain mode-0 "make sure this range exists" case could be built on it
// -- but the point of fallocate is to reserve space, and ramfs
// allocates on write regardless, so a success here would be a promise
// nothing keeps. Honest -EOPNOTSUPP; callers (SQLite, .NET FileStream)
// fall back to writing zeros. Recorded in docs/stdlib.md.
int64_t sys_fallocate(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return -EOPNOTSUPP;
}

// access(path, mode) / faccessat2(dirfd, path, mode, flags). NeoOS has
// no permission model: the check is pure existence. R_OK/W_OK/X_OK on
// a path that resolves all succeed; a path that does not is -ENOENT
// (etc, from vfs_resolve). Recorded in docs/stdlib.md.
static int64_t access_by_path(int64_t uptr, int64_t ulen) {
    char path[VFS_MAX_PATH];
    int rc = copy_user_path_at(uptr, ulen, path);
    if (rc != 0) { return rc; }
    fs_lock_acquire();
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) { fs_lock_release(); return err; }
    vnode_put(vn);
    fs_lock_release();
    return 0;
}

int64_t sys_access(struct syscall_args *a) {
    return access_by_path(a->a1, a->a2);
}

int64_t sys_faccessat(struct syscall_args *a) {
    if ((int)a->a1 != AT_FDCWD) { return -EBADF; }
    return access_by_path(a->a2, a->a3);
}
