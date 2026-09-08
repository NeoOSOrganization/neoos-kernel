// kernel/sync/epoll.c -- epoll(7), built on poll_core (kernel/syscall/
// sys_poll.c) rather than as a second readiness mechanism. See
// epoll.h's own comment for why, and for how EPOLLET rides on top of a
// core that is otherwise level-triggered.

#include "sync/epoll.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "errno.h"
#include "mm/heap.h"

// EPOLL_CTL_* -- Linux's values.
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

// Every live epoll object, so a close() of any descriptor can strip it
// from every set it was registered in (epoll_forget_fd, below).
static struct epoll_obj *g_epoll_list;
static struct spinlock   g_epoll_lock;
static int               g_epoll_lock_ready;

static void epoll_global_init_once(void) {
    // No dedicated init hook; first epoll_create() sets this up. Racing
    // creates would both see 0 and both init -- harmless, spin_init is
    // idempotent for this purpose and the very first process to make an
    // epoll fd is long past SMP bring-up being a factor here.
    if (!g_epoll_lock_ready) {
        spin_init(&g_epoll_lock, LOCK_RANK_EPOLL_LIST, "epoll-list");
        g_epoll_lock_ready = 1;
    }
}

void epoll_forget_fd(struct fd_table *owner, int fd) {
    if (!g_epoll_lock_ready || !owner) { return; }
    uint64_t gf = spin_lock_irqsave(&g_epoll_lock);
    for (struct epoll_obj *o = g_epoll_list; o; o = o->g_next) {
        if (o->owner != owner) { continue; }
        uint64_t of = spin_lock_irqsave(&o->lock);
        struct epoll_entry **pp = &o->list;
        while (*pp) {
            if ((*pp)->fd == fd) {
                struct epoll_entry *dead = *pp;
                *pp = dead->next;
                o->nfds--;
                kfree(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
        spin_unlock_irqrestore(&o->lock, of);
    }
    spin_unlock_irqrestore(&g_epoll_lock, gf);
}

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

    // Off the global list before freeing, under the same lock
    // epoll_forget_fd() walks it with.
    uint64_t gf = spin_lock_irqsave(&g_epoll_lock);
    struct epoll_obj **gpp = &g_epoll_list;
    while (*gpp && *gpp != o) { gpp = &(*gpp)->g_next; }
    if (*gpp) { *gpp = o->g_next; }
    spin_unlock_irqrestore(&g_epoll_lock, gf);

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

    epoll_global_init_once();

    struct epoll_obj *o = kmalloc(sizeof(*o));
    if (!o) { return -ENOMEM; }
    spin_init(&o->lock, LOCK_RANK_POLLHEAD, "epoll");
    o->list = 0;
    o->nfds = 0;
    o->refs = 1;
    o->gen_ctr = 0;
    o->owner = p->fd_table;
    o->g_next = 0;

    int fd = fd_table_alloc(p->fd_table);
    if (fd < 0) { kfree(o); return fd; }
    struct file_descriptor *f = fd_table_get(p->fd_table, fd);
    if (!f) { fd_table_close(p->fd_table, fd); kfree(o); return -EBADF; }

    f->ops = &epoll_file_ops;
    f->priv = o;
    f->readable = 0;
    f->writable = 0;

    // On the global list only once it is fully wired -- an error path
    // that kfree()s `o` must never leave a dangling pointer here.
    uint64_t gf = spin_lock_irqsave(&g_epoll_lock);
    o->g_next = g_epoll_list;
    g_epoll_list = o;
    spin_unlock_irqrestore(&g_epoll_lock, gf);
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
            // A fresh registration is fully armed: whatever the fd is
            // ready for right now is an edge it has not been told about.
            e->last_ready = 0; e->last_seq = 0; e->have_seq = 0;
            e->gen = ++o->gen_ctr;
            *pp = e;
            o->nfds++;
        }
        break;
    case EPOLL_CTL_MOD:
        if (!found) { rc = -ENOENT; break; }
        found->events = events;
        found->data = data;
        // MOD re-arms, which epoll(7) is explicit about and which is how
        // an edge-triggered caller that lost track recovers. The new gen
        // also fences off any epoll_wait already in flight against the
        // old registration.
        found->last_ready = 0; found->last_seq = 0; found->have_seq = 0;
        found->gen = ++o->gen_ctr;
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

    // Wake any epoll_wait() blocked on this object so it re-snapshots
    // the list. A thread can legitimately call epoll_wait() on an empty
    // set and have another thread register fds afterwards (.NET's
    // SocketAsyncEngine does exactly this); without this nudge that
    // waiter only re-checks on its EPOLL_REEVAL_TICKS safety timer.
    if (rc == 0 && (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD)) {
        waitq_poll_notify();
    }
    return rc;
}
