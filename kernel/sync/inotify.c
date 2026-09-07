// kernel/sync/inotify.c -- see inotify.h for the scope and why.

#include "sync/inotify.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "errno.h"
#include "mm/heap.h"

struct inotify_obj {
    struct spinlock lock;
    struct waitq readers;   // never woken -- see inotify.h
    int next_wd;
    int refs;
};

static int64_t inotify_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)buf;
    if (len == 0) { return 0; }
    struct inotify_obj *o = (struct inotify_obj *)f->priv;
    if (f->nonblock) { return -EAGAIN; }

    uint64_t flags = spin_lock_irqsave(&o->lock);
    int rc = waitq_sleep(&o->readers, &o->lock);
    spin_unlock_irqrestore(&o->lock, flags);
    return (rc == -EINTR) ? -EINTR : 0;   // an event that will never come
}

static int64_t inotify_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f; (void)buf; (void)len;
    return -EBADF;   // matches Linux: an inotify fd is not writable
}

static int64_t inotify_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -ESPIPE;
}

static int64_t inotify_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

static int64_t inotify_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f; (void)request; (void)arg;
    return -ENOTTY;
}

static int inotify_poll(struct file_descriptor *f, int events) {
    (void)f; (void)events;
    return 0;   // never readable: no event will ever be queued
}

static void inotify_dup(struct file_descriptor *f) {
    struct inotify_obj *o = (struct inotify_obj *)f->priv;
    uint64_t flags = spin_lock_irqsave(&o->lock);
    o->refs++;
    spin_unlock_irqrestore(&o->lock, flags);
}

static void inotify_close(struct file_descriptor *f) {
    struct inotify_obj *o = (struct inotify_obj *)f->priv;
    uint64_t flags = spin_lock_irqsave(&o->lock);
    int last = (--o->refs == 0);
    spin_unlock_irqrestore(&o->lock, flags);
    if (last) { kfree(o); }
}

const struct file_ops inotify_file_ops = {
    .name       = "inotify",
    .read       = inotify_read,
    .write      = inotify_write,
    .lseek      = inotify_lseek,
    .getdents   = inotify_getdents,
    .ioctl      = inotify_ioctl,
    .poll       = inotify_poll,
    .poll_head  = 0,
    .mmap       = 0,
    .dup        = inotify_dup,
    .close      = inotify_close,
};

int inotify_create(int flags) {
    // IN_CLOEXEC (O_CLOEXEC, 0x80000) and IN_NONBLOCK (O_NONBLOCK,
    // 0x800) are Linux's only inotify_init1 flags.
    if (flags & ~(0x80000 | 0x800)) { return -EINVAL; }

    struct process *p = current_proc();
    if (!p) { return -ESRCH; }

    struct inotify_obj *o = kmalloc(sizeof(*o));
    if (!o) { return -ENOMEM; }
    spin_init(&o->lock, LOCK_RANK_POLLHEAD, "inotify");
    waitq_init(&o->readers);
    o->next_wd = 1;
    o->refs = 1;

    int fd = fd_table_alloc(p->fd_table);
    if (fd < 0) { kfree(o); return fd; }
    struct file_descriptor *f = fd_table_get(p->fd_table, fd);
    if (!f) { fd_table_close(p->fd_table, fd); kfree(o); return -EBADF; }

    f->ops = &inotify_file_ops;
    f->priv = o;
    f->readable = 1;
    f->writable = 0;
    f->nonblock = (flags & 0x800) ? 1 : 0;
    return fd;
}

int inotify_add_watch_do(struct file_descriptor *f, uint32_t mask) {
    (void)mask;
    if (!f || f->ops != &inotify_file_ops) { return -EINVAL; }
    struct inotify_obj *o = (struct inotify_obj *)f->priv;
    uint64_t flags = spin_lock_irqsave(&o->lock);
    int wd = o->next_wd++;
    spin_unlock_irqrestore(&o->lock, flags);
    return wd;   // recorded nowhere: no event will ever reference it
}

int inotify_rm_watch_do(struct file_descriptor *f, int wd) {
    (void)wd;
    if (!f || f->ops != &inotify_file_ops) { return -EINVAL; }
    return 0;
}
