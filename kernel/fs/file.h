#ifndef NEOOS_FILE_H
#define NEOOS_FILE_H

#include <stdint.h>

// What an open file descriptor CAN DO, separated from what it is
// attached to.
//
// A file descriptor used to point straight at a struct vnode, which
// works for exactly as long as everything a program can open is a file
// on a filesystem. A pipe is not: it has no inode, no size, no
// position, and reading one blocks until somebody writes. The same is
// true of the sockets and ports still to come.
//
// So an fd now carries an ops table and an opaque object. Vnode-backed
// files are one implementation of it (kernel/file.c), pipes another
// (kernel/pipe.c). The syscall layer stays uniform -- sys_read is four
// lines and knows nothing about what it is reading from -- which is the
// whole point: the alternative is a `kind` field tested in every
// syscall that touches an fd, and one forgotten test is a pipe being
// read as if it were a file.
//
// No op pointer is ever NULL. An implementation that cannot perform an
// operation supplies a stub returning the truthful error, exactly as
// struct vfs_ops already requires, so callers never branch on which
// kind of file they hold.

struct file_descriptor;
struct vfs_dirent;

// POLL* bits matching Linux x86_64 values for Phase 15 compatibility.
#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

struct file_ops {
    const char *name;   // for diagnostics and the selftest

    int64_t (*read)(struct file_descriptor *f, void *buf, uint64_t len);
    int64_t (*write)(struct file_descriptor *f, const void *buf, uint64_t len);
    int64_t (*lseek)(struct file_descriptor *f, int64_t offset, int whence);
    int64_t (*getdents)(struct file_descriptor *f, void *buf, int bytes);
    int64_t (*ioctl)(struct file_descriptor *f, uint64_t request, void *arg);
    int     (*poll)(struct file_descriptor *f, int events);

    // Reference counting, called by the fd table rather than by the
    // syscall layer. `dup` is called once per new fd that comes to
    // share the object (fork); `close` once per fd that stops. The
    // implementation frees the object when its last reference goes.
    void (*dup)(struct file_descriptor *f);
    void (*close)(struct file_descriptor *f);
};

// The default: an fd whose ops pointer is null is vnode-backed. Every
// path that creates an fd sets this explicitly; the null case exists
// only so a slot that has been allocated but not yet filled in cannot
// dispatch through a garbage pointer.
extern const struct file_ops vnode_file_ops;

// Dispatch helpers. They exist so the null-ops case is handled in one
// place instead of at every call site.
int64_t file_read(struct file_descriptor *f, void *buf, uint64_t len);
int64_t file_write(struct file_descriptor *f, const void *buf, uint64_t len);
int64_t file_lseek(struct file_descriptor *f, int64_t offset, int whence);
int64_t file_getdents(struct file_descriptor *f, void *buf, int bytes);
int64_t file_bind_vnode_ops(struct file_descriptor *f);
int64_t file_ioctl(struct file_descriptor *f, uint64_t request, void *arg);
int     file_poll(struct file_descriptor *f, int events);
void    file_dup(struct file_descriptor *f);
void    file_close(struct file_descriptor *f);

void file_selftest(void);

#endif
