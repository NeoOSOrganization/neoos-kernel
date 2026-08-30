// kernel/file.c -- the vnode-backed implementation of struct file_ops,
// plus the dispatch helpers every fd goes through.
//
// This is the code that used to live inline in syscall.c's read/write/
// lseek/getdents cases. Moving it behind an ops table is what lets a
// pipe -- and later a port or a socket -- be read and written by the
// same four syscalls without any of them learning what it is holding.

#include "file.h"
#include "pipe.h"
#include "net/socket.h"
#include "sched/proc.h"
#include "fs/vfs.h"
#include "errno.h"
#include "serial.h"

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

static int64_t vnode_getdents(struct file_descriptor *f, struct dirent *out, int count) {
    if (!f->vn) { return -EBADF; }
    if (f->vn->type != VNODE_DIR) { return -ENOTDIR; }

    // position doubles as the directory cursor for a dir fd, so
    // repeated calls walk forward exactly like read() does on a file.
    vfs_lock();
    int written = 0;
    while (written < count) {
        if (f->vn->mount->ops->readdir(f->vn, f->position, &out[written]) != 0) {
            break;  // past the last entry
        }
        f->position++;
        written++;
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
    .dup      = vnode_dup,
    .close    = vnode_close,
};

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

int64_t file_getdents(struct file_descriptor *f, struct dirent *out, int count) {
    const struct file_ops *o = ops_of(f);
    if (!o) { return -EBADF; }
    return o->getdents(f, out, count);
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
           o->getdents && o->dup && o->close;
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
