// kernel/sync/epoll.c -- epoll(7), built on poll_core (kernel/syscall/
// sys_poll.c) rather than as a second readiness mechanism. See
// epoll.h's own comment for why, and for the level-triggered-only
// scope.

#include "sync/epoll.h"
#include "sync/lock.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "errno.h"
#include "mm/heap.h"

// EPOLL_CTL_* -- Linux's values.
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

static int64_t epoll_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f; (void)buf; (void)len;
    return -EINVAL;   // matches Linux: read() on an epoll fd is not supported
}

static int64_t epoll_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f; (void)buf; (void)len;
    return -EINVAL;
}

static int64_t epoll_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -ESPIPE;
}

static int64_t epoll_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

static int64_t epoll_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f; (void)request; (void)arg;
    return -ENOTTY;
}

static int epoll_poll(struct file_descriptor *f, int events) {
    (void)f; (void)events;
    return 0;   // an epoll fd polling another epoll fd is not supported
}

static void epoll_dup(struct file_descriptor *f) {
    struct epoll_obj *o = (struct epoll_obj *)f->priv;
    uint64_t flags = spin_lock_irqsave(&o->lock);
    o->refs++;
    spin_unlock_irqrestore(&o->lock, flags);
}

static void epoll_close(struct file_descriptor *f) {
    struct epoll_obj *o = (struct epoll_obj *)f->priv;
    uint64_t flags = spin_lock_irqsave(&o->lock);
    int last = (--o->refs == 0);
    spin_unlock_irqrestore(&o->lock, flags);
    if (!last) { return; }

    struct epoll_entry *e = o->list;
    while (e) {
        struct epoll_entry *next = e->next;
        kfree(e);
        e = next;
    }
    kfree(o);
}

const struct file_ops epoll_file_ops = {
    .name       = "epoll",
    .read       = epoll_read,
    .write      = epoll_write,
    .lseek      = epoll_lseek,
    .getdents   = epoll_getdents,
    .ioctl      = epoll_ioctl,
    .poll       = epoll_poll,
    .poll_head  = 0,
    .mmap       = 0,
    .dup        = epoll_dup,
    .close      = epoll_close,
};

int epoll_create(int flags) {
    // EPOLL_CLOEXEC is O_CLOEXEC's value; NeoOS's exec does not walk
    // the fd table closing anything yet (same divergence pipe2/dup3
    // already record), so it is accepted and has nothing to do.
    if (flags & ~0x80000) { return -EINVAL; }

    struct process *p = current_proc();
    if (!p) { return -ESRCH; }

    struct epoll_obj *o = kmalloc(sizeof(*o));
    if (!o) { return -ENOMEM; }
    spin_init(&o->lock, LOCK_RANK_POLLHEAD, "epoll");
    o->list = 0;
    o->nfds = 0;
    o->refs = 1;

    int fd = fd_table_alloc(p->fd_table);
    if (fd < 0) { kfree(o); return fd; }
    struct file_descriptor *f = fd_table_get(p->fd_table, fd);
    if (!f) { fd_table_close(p->fd_table, fd); kfree(o); return -EBADF; }

    f->ops = &epoll_file_ops;
    f->priv = o;
    f->readable = 0;
    f->writable = 0;
    return fd;
}

int epoll_ctl_do(struct file_descriptor *epf, int op, int fd, uint32_t events, uint64_t data) {
    if (!epf || epf->ops != &epoll_file_ops) { return -EINVAL; }
    struct epoll_obj *o = (struct epoll_obj *)epf->priv;

    uint64_t flags = spin_lock_irqsave(&o->lock);

    struct epoll_entry **pp = &o->list;
    while (*pp && (*pp)->fd != fd) { pp = &(*pp)->next; }
    struct epoll_entry *found = *pp;

    int rc = 0;
    switch (op) {
    case EPOLL_CTL_ADD:
        if (found) { rc = -EEXIST; break; }
        {
            struct epoll_entry *e = kmalloc(sizeof(*e));
            if (!e) { rc = -ENOMEM; break; }
            e->fd = fd; e->events = events; e->data = data; e->next = 0;
            *pp = e;
            o->nfds++;
        }
        break;
    case EPOLL_CTL_MOD:
        if (!found) { rc = -ENOENT; break; }
        found->events = events;
        found->data = data;
        break;
    case EPOLL_CTL_DEL:
        if (!found) { rc = -ENOENT; break; }
        *pp = found->next;
        o->nfds--;
        kfree(found);
        break;
    default:
        rc = -EINVAL;
        break;
    }

    spin_unlock_irqrestore(&o->lock, flags);
    return rc;
}
