#include "syscall.h"
#include "gdt.h"
#include "serial.h"
#include "sched/proc.h"
#include "fs/vfs.h"
#include "errno.h"
#include "lock.h"
#include "signal.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define EFER_SCE (1ULL << 0)
#define EFER_NXE (1ULL << 11)

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5
#define SYS_READ   6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_MKDIR  9
#define SYS_UNLINK 10
#define SYS_LSEEK  11
#define SYS_FORK   12
#define SYS_EXEC   13
#define SYS_MOUNT  14
#define SYS_UMOUNT 15
#define SYS_GETDENTS 16
#define SYS_THREAD_CREATE 17
#define SYS_THREAD_EXIT   18
#define SYS_THREAD_JOIN   19
#define SYS_THREAD_SELF   20
#define SYS_KILL          29
#define SYS_TKILL         30
#define SYS_TGKILL        31

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

// A sleeping mutex, not a spinlock: every critical section it guards
// performs disk I/O, and spinning through that would burn the whole
// time slice. Rank MOUNTTABLE per the roadmap's lock hierarchy.
static struct mutex fs_lock;
#define fs_lock_acquire() mutex_lock(&fs_lock)
#define fs_lock_release() mutex_unlock(&fs_lock)

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

// Called only from syscall_entry.asm's `call syscall_dispatch`.
static int64_t syscall_dispatch_inner(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4, struct syscall_frame *frame) {
    (void)a4;
    (void)frame; // unused until fork()/exec() land
    switch (num) {
        case SYS_EXIT:
            process_exit((int)a1);
            return 0; // unreachable -- task_exit never returns
        case SYS_WRITE: {
            int fd = (int)a1;
            const char *buf = (const char *)(uintptr_t)a2;
            uint64_t len = (uint64_t)a3;
            if (fd < 0 || fd >= MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_proc()->files[fd];
            if (!f->in_use || !f->writable) {
                return -EBADF;
            }
            fs_lock_acquire();
            int64_t n = f->vn->mount->ops->write(f->vn, f->position, buf, (uint32_t)len);
            if (n < 0) {
                fs_lock_release();
                return n;
            }
            f->position += (uint32_t)n;
            fs_lock_release();
            return n;
        }
        case SYS_READ: {
            int fd = (int)a1;
            char *buf = (char *)(uintptr_t)a2;
            uint64_t len = (uint64_t)a3;
            if (fd < 0 || fd >= MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_proc()->files[fd];
            if (!f->in_use) {
                return -EBADF;
            }
            int64_t n = f->vn->mount->ops->read(f->vn, f->position, buf, (uint32_t)len);
            if (n < 0) { return n; }
            f->position += (uint32_t)n;
            return n;
        }
        case SYS_GETPID:
            return current_proc()->pid;
        case SYS_YIELD:
            schedule();
            return 0;
        case SYS_SPAWN: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            struct process *child = spawn(path_buf);
            return child ? child->pid : -1;
        }
        case SYS_WAIT:
            return wait_for_pid((int)a1);
        case SYS_OPEN: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            int flags = (int)a3;

            struct process *task = current_proc();
            // Slots 0-2 are the standard streams, opened at process
            // creation; ordinary opens start above them.
            int slot = -1;
            for (int i = 3; i < MAX_OPEN_FILES; i++) {
                if (!task->files[i].in_use) { slot = i; break; }
            }
            if (slot < 0) { return -EMFILE; }

            fs_lock_acquire();

            int err = 0;
            struct vnode *vn = vfs_resolve(path_buf, &err);
            if (!vn && (flags & O_CREAT)) {
                char name[VFS_NAME_MAX];
                struct vnode *dir = vfs_resolve_parent(path_buf, name, &err);
                if (!dir) { fs_lock_release(); return err; }
                uint64_t new_id;
                int rc = dir->mount->ops->create(dir, name, &new_id);
                if (rc != 0) { vnode_put(dir); fs_lock_release(); return rc; }
                vn = vnode_get(dir->mount, new_id);
                vnode_put(dir);
                if (!vn) { fs_lock_release(); return -ENFILE; }
            }
            if (!vn) { fs_lock_release(); return err; }
            if (vn->type == VNODE_DIR && (flags & (O_WRONLY | O_RDWR))) {
                vnode_put(vn);
                fs_lock_release();
                return -EISDIR;
            }
            if (flags & O_TRUNC) {
                vn->mount->ops->truncate(vn);
            }

            fs_lock_release();

            struct file_descriptor *f = &task->files[slot];
            f->in_use = 1;
            f->vn = vn;   // the reference vfs_resolve/vnode_get took is now the fd's
            f->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
            f->position = (flags & O_APPEND) ? vn->size : 0;
            return slot;
        }
        case SYS_CLOSE: {
            int fd = (int)a1;
            if (fd < 0 || fd >= MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_proc()->files[fd];
            if (!f->in_use) {
                return -EBADF;
            }
            vnode_put(f->vn);
            f->vn = 0;
            f->in_use = 0;
            return 0;
        }
        case SYS_MKDIR: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
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
        case SYS_UNLINK: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
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
        case SYS_LSEEK: {
            int fd = (int)a1;
            int64_t offset = a2;
            int whence = (int)a3;
            if (fd < 0 || fd >= MAX_OPEN_FILES) {
                return -EBADF;
            }
            struct file_descriptor *f = &current_proc()->files[fd];
            if (!f->in_use) {
                return -EBADF;
            }
            int64_t base;
            switch (whence) {
                case SEEK_SET: base = 0; break;
                case SEEK_CUR: base = (int64_t)f->position; break;
                case SEEK_END: base = (int64_t)f->vn->size; break;
                default: return -EINVAL;
            }
            int64_t new_position = base + offset;
            if (new_position < 0) {
                return -EINVAL;
            }
            f->position = (uint32_t)new_position;
            return new_position;
        }
        case SYS_FORK: {
            struct thread *child = fork_task(frame);
            return child ? child->proc->pid : -1;
        }
        case SYS_EXEC: {
            char path_buf[64];
            copy_user_path(a1, a2, path_buf, sizeof(path_buf));
            return exec_task(path_buf, frame) ? 0 : -1;
        }
        case SYS_MOUNT: {
            char source[16], target[VFS_MAX_PATH], fstype[16];
            copy_user_string(a1, source, sizeof(source));
            copy_user_string(a2, target, sizeof(target));
            copy_user_string(a3, fstype, sizeof(fstype));
            fs_lock_acquire();
            int rc = vfs_mount_fs(source, target, fstype);
            fs_lock_release();
            return rc;
        }
        case SYS_UMOUNT: {
            char target[VFS_MAX_PATH];
            copy_user_path(a1, a2, target, sizeof(target));
            fs_lock_acquire();
            int rc = vfs_umount(target);
            fs_lock_release();
            return rc;
        }
        case SYS_GETDENTS: {
            int fd = (int)a1;
            struct dirent *out = (struct dirent *)(uintptr_t)a2;
            int count = (int)a3;
            if (fd < 0 || fd >= MAX_OPEN_FILES || count <= 0) { return -EBADF; }

            struct file_descriptor *f = &current_proc()->files[fd];
            if (!f->in_use) { return -EBADF; }
            if (f->vn->type != VNODE_DIR) { return -ENOTDIR; }

            // position doubles as the directory cursor for a dir fd,
            // so repeated calls walk forward exactly like read() does
            // on a file.
            fs_lock_acquire();
            int written = 0;
            while (written < count) {
                if (f->vn->mount->ops->readdir(f->vn, f->position, &out[written]) != 0) {
                    break; // past the last entry
                }
                f->position++;
                written++;
            }
            fs_lock_release();
            return written;
        }
        case SYS_THREAD_CREATE: {
            struct thread *t = thread_create(a1, a2);
            return t ? t->tid : -EAGAIN;
        }
        case SYS_THREAD_EXIT:
            thread_exit_self((int)a1);
            return 0; // unreachable
        case SYS_THREAD_JOIN: {
            int code = 0;
            int rc = thread_join((int)a1, &code);
            if (rc == 0 && a2) {
                *(int *)(uintptr_t)a2 = code;
            }
            return rc;
        }
        case SYS_THREAD_SELF:
            return current_thread()->tid;
        case SYS_KILL: {
            struct siginfo info;
            siginfo_user(&info, (int)a2, current_proc()->pid);
            return signal_kill((int)a1, (int)a2, &info);
        }
        case SYS_TKILL: {
            struct siginfo info;
            siginfo_user(&info, (int)a2, current_proc()->pid);
            return signal_tkill(0, (int)a1, (int)a2, &info);
        }
        case SYS_TGKILL: {
            struct siginfo info;
            siginfo_user(&info, (int)a3, current_proc()->pid);
            return signal_tkill((int)a1, (int)a2, (int)a3, &info);
        }

        default:
            serial_write_string("[syscall] unknown syscall number\n");
            return -1;
    }
}

// Single exit point for every syscall. A thread killed by a sibling's
// exit() unwinds to here: whatever it was doing has finished, and it
// must not return to user mode.
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3,
                         int64_t a4, struct syscall_frame *frame) {
    int64_t ret = syscall_dispatch_inner(num, a1, a2, a3, a4, frame);
    struct thread *t = current_thread();
    if (t && t->kill_pending) {
        thread_exit_self(current_proc()->exit_code);
    }
    return ret;
}

void syscall_init(void) {
    mutex_init(&fs_lock, LOCK_RANK_MOUNTTABLE, "fs");
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

    serial_write_string("[syscall] SYSCALL/SYSRET configured\n");
}
