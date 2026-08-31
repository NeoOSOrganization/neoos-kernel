// kernel/file.c -- the vnode-backed implementation of struct file_ops,
// plus the dispatch helpers every fd goes through.
//
// This is the code that used to live inline in syscall.c's read/write/
// lseek/getdents cases. Moving it behind an ops table is what lets a
// pipe -- and later a port or a socket -- be read and written by the
// same four syscalls without any of them learning what it is holding.

#include "fs/file.h"
#include "fs/devfs.h"
#include "ipc/pipe.h"
#include "net/socket.h"
#include "sched/proc.h"
#include "fs/vfs.h"
#include "errno.h"
#include "dev/serial.h"

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static int64_t vnode_read(struct file_descriptor *f, void *buf, uint64_t len) {
    if (!f->vn) { return -EBADF; }
    // The filesystem lock is taken HERE rather than around the whole
    // syscall, so that a read from a pipe -- which may block for an
    // unbounded time -- never holds it.
    vfs_lock();
    int64_t n = f->vn->mount->ops->read(f->vn, f->position, buf, (uint32_t)len);
    if (n > 0) { f->position += (uint32_t)n; }
    vfs_unlock();
    return n;
}

static int64_t vnode_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    if (!f->vn) { return -EBADF; }
    vfs_lock();
    int64_t n = f->vn->mount->ops->write(f->vn, f->position, buf, (uint32_t)len);
    if (n > 0) { f->position += (uint32_t)n; }
    vfs_unlock();
    return n;
}

static int64_t vnode_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    if (!f->vn) { return -EBADF; }
    int64_t base;
    if (whence == SEEK_SET)      { base = 0; }
    else if (whence == SEEK_CUR) { base = (int64_t)f->position; }
    else if (whence == SEEK_END) { base = (int64_t)f->vn->size; }
    else                         { return -EINVAL; }

    int64_t pos = base + offset;
    if (pos < 0) { return -EINVAL; }
    f->position = (uint32_t)pos;
    return pos;
}

// getdents64: fills `buf` with as many complete Linux records as fit,
// and returns the number of BYTES written -- not a count of entries,
// which is what this returned before the layout changed.
//
// Zero means end of directory. A buffer too small for even the first
// record is -EINVAL, as on Linux: returning 0 there would look like an
// empty directory and silently truncate a listing.
static int64_t vnode_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f; (void)request; (void)arg;
    return -ENOTTY;
}

static int vnode_poll(struct file_descriptor *f, int events) {
    (void)f; (void)events;
    // Vnode-backed files are always ready for read and write.
    return POLLIN | POLLOUT;
}

static int64_t vnode_getdents(struct file_descriptor *f, void *buf, int bytes) {
    if (!f->vn) { return -EBADF; }
    if (f->vn->type != VNODE_DIR) { return -ENOTDIR; }
    if (bytes < 0) { return -EINVAL; }

    uint8_t *out = (uint8_t *)buf;
    int written = 0;

    vfs_lock();
    for (;;) {
        // position doubles as the directory cursor for a dir fd, so
        // repeated calls walk forward exactly like read() does.
        struct vfs_dirent de;
        de.ino = 0; de.type = DT_UNKNOWN; de.name[0] = '\0';
        if (f->vn->mount->ops->readdir(f->vn, f->position, &de) != 0) {
            break;  // past the last entry
        }

        uint64_t namelen = 0;
        while (de.name[namelen]) { namelen++; }

        // Header + name + NUL, rounded up so the NEXT record starts
        // 8-byte aligned.
        uint64_t reclen = (DIRENT64_HEADER + namelen + 1 + 7) & ~7ULL;

        if (written + (int)reclen > bytes) {
            // No room. Do NOT advance the cursor: this entry must come
            // back on the next call, not be skipped.
            if (written == 0) { vfs_unlock(); return -EINVAL; }
            break;
        }

        f->position++;

        struct linux_dirent64 *rec = (struct linux_dirent64 *)(out + written);
        rec->d_ino    = de.ino;
        // Linux treats d_off as an opaque cookie for seeking. The next
        // cursor value is exactly that, and it is honest: lseek to it
        // and the following getdents resumes here.
        rec->d_off    = (int64_t)f->position;
        rec->d_reclen = (uint16_t)reclen;
        rec->d_type   = de.type;
        for (uint64_t i = 0; i < namelen; i++) { rec->d_name[i] = de.name[i]; }
        // Every byte after the name, through the alignment padding, is
        // zeroed: a caller reading d_name must find a NUL, and padding
        // left as stack residue would leak kernel bytes to userland.
        for (uint64_t i = namelen; i < reclen - DIRENT64_HEADER; i++) {
            rec->d_name[i] = '\0';
        }

        written += (int)reclen;
    }
    vfs_unlock();
    return written;
}

static void vnode_dup(struct file_descriptor *f) {
    // The fd table copies the descriptor by value, so the new fd holds
    // the same vnode POINTER. Without a reference of its own, the first
    // close on either side would free a vnode the other still holds.
    if (f->vn) { f->vn->refcount++; }
}

static void vnode_close(struct file_descriptor *f) {
    if (f->vn) { vnode_put(f->vn); f->vn = 0; }
}

const struct file_ops vnode_file_ops = {
    .name     = "vnode",
    .read     = vnode_read,
    .write    = vnode_write,
    .lseek    = vnode_lseek,
    .getdents = vnode_getdents,
    .ioctl    = vnode_ioctl,
    .poll     = vnode_poll,
    .dup      = vnode_dup,
    .close    = vnode_close,
};

// Bind the right ops to a freshly populated vnode-backed fd. A device
// vnode gets its per-device file_ops (and the chance to set up f->priv
// via dev->open); everything else keeps vnode_file_ops. Both sys_open
// and the standard-stream setup that runs through fd_table_put go
// through here, so /dev/CONSOLE behaves the same however it was opened.
// Returns 0, or a negative errno if dev->open failed.
int64_t file_bind_vnode_ops(struct file_descriptor *f) {
    f->ops = &vnode_file_ops;
    if (f->vn && f->vn->type == VNODE_DEVICE) {
        const struct devfs_dev *dev = (const struct devfs_dev *)f->vn->fs_private;
        if (dev && dev->fops) {
            f->ops = dev->fops;
            if (dev->open) { return dev->open(f); }
        }
    }
    return 0;
}

// ------------------------------------------------------------- dispatch

// A null ops pointer means a slot that fd_table_alloc has reserved but
// whose owner has not filled in yet. Reachable only through a race in
// the kernel's own code, never from userland -- but a null dispatch
// would be a jump through address zero, so it is checked once here
// rather than trusted at six call sites.
static const struct file_ops *ops_of(struct file_descriptor *f) {
    return f && f->ops ? f->ops : 0;
}

int64_t file_read(struct file_descriptor *f, void *buf, uint64_t len) {
    const struct file_ops *o = ops_of(f);
    if (!o) { return -EBADF; }
    if (!f->readable) { return -EBADF; }
    return o->read(f, buf, len);
}

int64_t file_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    const struct file_ops *o = ops_of(f);
    if (!o) { return -EBADF; }
    if (!f->writable) { return -EBADF; }
    return o->write(f, buf, len);
}

int64_t file_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    const struct file_ops *o = ops_of(f);
    if (!o) { return -EBADF; }
    return o->lseek(f, offset, whence);
}

int64_t file_getdents(struct file_descriptor *f, void *buf, int bytes) {
    const struct file_ops *o = ops_of(f);
    if (!o) { return -EBADF; }
    return o->getdents(f, buf, bytes);
}

int64_t file_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    const struct file_ops *o = ops_of(f);
    if (!o) { return -EBADF; }
    return o->ioctl(f, request, arg);
}

int file_poll(struct file_descriptor *f, int events) {
    const struct file_ops *o = ops_of(f);
    if (!o) { return 0; }
    return o->poll(f, events);
}

void file_dup(struct file_descriptor *f) {
    const struct file_ops *o = ops_of(f);
    if (o) { o->dup(f); }
}

void file_close(struct file_descriptor *f) {
    const struct file_ops *o = ops_of(f);
    if (o) { o->close(f); }
}

// ------------------------------------------------------------- selftest

// Asserts the property the header states as a rule: no op pointer is
// ever null. A missing op is a jump through address zero on the first
// syscall that reaches it, which is a triple fault and a silent reboot
// -- worth one loop at boot to turn into a named failure.
static int ops_complete(const struct file_ops *o) {
    return o && o->name && o->read && o->write && o->lseek &&
           o->getdents && o->ioctl && o->poll && o->dup && o->close;
}

void file_selftest(void) {
    if (!ops_complete(&vnode_file_ops)) {
        serial_write_string("[file] selftest FAILED: vnode_file_ops has a null op\n");
        return;
    }
    if (!ops_complete(pipe_file_ops())) {
        serial_write_string("[file] selftest FAILED: pipe_file_ops has a null op\n");
        return;
    }
    if (!ops_complete(socket_file_ops())) {
        serial_write_string("[file] selftest FAILED: socket_file_ops has a null op\n");
        return;
    }
    serial_write_string("[file] file-ops selftest passed\n");
}
