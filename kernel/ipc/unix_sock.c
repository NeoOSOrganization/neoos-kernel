// kernel/ipc/unix_sock.c -- AF_UNIX stream sockets.
//
// socketpair(2) has been here since the pipes milestone, but it only
// connects processes that already share an ancestor: one call makes
// both ends. A window manager needs the other shape entirely -- it
// publishes a name, and processes it has never heard of connect to it.
// That is bind/listen/accept/connect, and none of it existed:
// socket_create refused every domain but AF_INET.
//
// The stream itself is two of pipe.c's rings cross-wired, the way
// socketpair.c does it. Reusing them is what makes poll, epoll and the
// edge-triggered readiness counter work here for free.
//
// LOCKING. A socket's lock is LOCK_RANK_SOCKET (12) and the name table
// is LOCK_RANK_SOCKTABLE (11), both ABOVE pipe's rank 10 -- so no pipe
// function may be called while either is held. The connected endpoints
// are immutable once established, which is what makes that easy: read
// and write take them straight from f->priv with no lock at all.

#include "ipc/unix_sock.h"
#include "ipc/pipe.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sync/poll_head.h"
#include "mm/heap.h"
#include "net/socket.h"
#include "drivers/char/serial.h"
#include "errno.h"

#define UNIX_MAX_BOUND    32
#define UNIX_BACKLOG_MAX  16
#define SOCK_TYPE_MASK    0xF

enum { U_UNBOUND = 0, U_BOUND, U_LISTENING, U_CONNECTED, U_DEAD };

// One end of a connected pair: the ring it reads and the ring it
// writes. Same shape as socketpair's spair_end, for the same reason.
struct unix_conn {
    struct pipe *rd, *wr;
};

struct unix_sock {
    struct spinlock lock;
    int      state;
    int      type;

    // Bound name. Abstract namespace only: sun_path[0] == '\0' and the
    // name is the bytes after it, not NUL-terminated.
    char     name[UNIX_PATH_MAX];
    uint32_t name_len;

    // Listening.
    struct unix_conn backlog[UNIX_BACKLOG_MAX];
    int      bl_count, bl_head, bl_max;
    struct waitq acceptors;
    struct poll_head poll;

    // Connected.
    struct unix_conn conn;

    int refs;
};

static struct spinlock unix_table_lock;
static struct unix_sock *bound[UNIX_MAX_BOUND];

void unix_sock_init(void) {
    spin_init(&unix_table_lock, LOCK_RANK_SOCKTABLE, "unix-names");
    for (int i = 0; i < UNIX_MAX_BOUND; i++) { bound[i] = 0; }
}

// ---- name handling ----------------------------------------------------

// Returns the abstract name length, or a negative errno. Linux's rule:
// a leading NUL means abstract, and the name is every remaining byte of
// the address as measured by addrlen -- NOT a C string, so embedded
// NULs are part of it and there is no terminator.
static int64_t unix_extract_name(const struct k_sockaddr *addr, uint32_t len,
                                 char *out, uint32_t *out_len) {
    if (!addr) { return -EFAULT; }
    const struct k_sockaddr_un *un = (const struct k_sockaddr_un *)addr;
    if (len < sizeof(uint16_t) + 1) { return -EINVAL; }
    if (un->sun_family != AF_UNIX) { return -EAFNOSUPPORT; }

    uint32_t path_len = len - (uint32_t)sizeof(uint16_t);
    if (path_len > UNIX_PATH_MAX) { return -EINVAL; }
    if (un->sun_path[0] != '\0') {
        // Pathname binding would create an S_IFSOCK node and look it up
        // through the VFS. Not implemented; recorded in docs/stdlib.md.
        return -EINVAL;
    }
    uint32_t n = path_len - 1;
    for (uint32_t i = 0; i < n; i++) { out[i] = un->sun_path[1 + i]; }
    *out_len = n;
    return 0;
}

static int unix_name_eq(const struct unix_sock *s, const char *n, uint32_t len) {
    if (s->name_len != len) { return 0; }
    for (uint32_t i = 0; i < len; i++) { if (s->name[i] != n[i]) { return 0; } }
    return 1;
}

// ---- lifetime ---------------------------------------------------------

static void unix_get(struct unix_sock *s) {
    __atomic_add_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
}

static void unix_drop_name(struct unix_sock *s) {
    uint64_t f = spin_lock_irqsave(&unix_table_lock);
    for (int i = 0; i < UNIX_MAX_BOUND; i++) {
        if (bound[i] == s) { bound[i] = 0; break; }
    }
    spin_unlock_irqrestore(&unix_table_lock, f);
}

static void unix_conn_close(struct unix_conn *c) {
    if (c->rd) { pipe_close_ep(c->rd, 1, 0); c->rd = 0; }
    if (c->wr) { pipe_close_ep(c->wr, 0, 1); c->wr = 0; }
}

static void unix_put(struct unix_sock *s) {
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST) != 0) { return; }

    unix_drop_name(s);

    // A listener going away takes its unaccepted connections with it.
    // Closing both ends of each queued pair is what gives the peer an
    // ECONNRESET-shaped EOF rather than a socket that never answers.
    for (int i = 0; i < s->bl_count; i++) {
        unix_conn_close(&s->backlog[(s->bl_head + i) % UNIX_BACKLOG_MAX]);
    }
    unix_conn_close(&s->conn);
    kfree(s);
}

// ---- file operations --------------------------------------------------

static int64_t usock_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct unix_sock *s = f->priv;
    if (!s || s->state != U_CONNECTED) { return -ENOTCONN; }
    return pipe_read_ep(s->conn.rd, f->nonblock, buf, len);
}

static int64_t usock_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct unix_sock *s = f->priv;
    if (!s || s->state != U_CONNECTED) { return -ENOTCONN; }
    return pipe_write_ep(s->conn.wr, f->nonblock, buf, len);
}

static int64_t usock_lseek(struct file_descriptor *f, int64_t off, int whence) {
    (void)f; (void)off; (void)whence; return -ESPIPE;
}
static int64_t usock_getdents(struct file_descriptor *f, void *b, int n) {
    (void)f; (void)b; (void)n; return -ENOTDIR;
}
static int64_t usock_ioctl(struct file_descriptor *f, uint64_t r, void *a) {
    (void)f; (void)r; (void)a; return -ENOTTY;
}

static int usock_poll(struct file_descriptor *f, int events) {
    struct unix_sock *s = f->priv;
    if (!s) { return POLLERR; }
    if (s->state == U_LISTENING) {
        // A listener is "readable" when a connection is waiting, which
        // is what accept-loop pollers are asking about.
        uint64_t fl = spin_lock_irqsave(&s->lock);
        int ready = s->bl_count > 0;
        spin_unlock_irqrestore(&s->lock, fl);
        return ready ? (events & POLLIN) : 0;
    }
    if (s->state != U_CONNECTED) { return 0; }
    int mask = pipe_poll_ep(s->conn.rd, 1, 0, events | POLLIN | POLLHUP);
    mask    |= pipe_poll_ep(s->conn.wr, 0, 1, events | POLLOUT);
    return mask & events;
}

// Readiness spans two rings (or a backlog), so there is no single poll
// head to register on -- same situation as a socketpair end. Returning 0
// opts into poll_core's broadcast, and ready_seq keeps EPOLLET working.
static struct poll_head *usock_poll_head(struct file_descriptor *f) {
    struct unix_sock *s = f->priv;
    if (s && s->state == U_LISTENING) { return &s->poll; }
    return 0;
}

static uint64_t usock_ready_seq(struct file_descriptor *f) {
    struct unix_sock *s = f->priv;
    if (!s || s->state != U_CONNECTED) { return 0; }
    return pipe_ready_seq_ep(s->conn.rd) + pipe_ready_seq_ep(s->conn.wr);
}

static void usock_dup(struct file_descriptor *f) {
    struct unix_sock *s = f->priv;
    if (!s) { return; }
    unix_get(s);
    if (s->state == U_CONNECTED) {
        pipe_dup_ep(s->conn.rd, 1, 0);
        pipe_dup_ep(s->conn.wr, 0, 1);
    }
}

static void usock_close(struct file_descriptor *f) {
    struct unix_sock *s = f->priv;
    if (!s) { return; }
    f->priv = 0;
    unix_put(s);
}

static const struct file_ops unix_ops = {
    .name      = "unix",
    .read      = usock_read,
    .write     = usock_write,
    .lseek     = usock_lseek,
    .getdents  = usock_getdents,
    .ioctl     = usock_ioctl,
    .poll      = usock_poll,
    .poll_head = usock_poll_head,
    .ready_seq = usock_ready_seq,
    .dup       = usock_dup,
    .close     = usock_close,
};

int unix_socket_is(struct file_descriptor *f) {
    return f && f->ops == &unix_ops;
}

// ---- creation ---------------------------------------------------------

static struct unix_sock *unix_alloc(int type) {
    struct unix_sock *s = kmalloc(sizeof(struct unix_sock));
    if (!s) { return 0; }
    spin_init(&s->lock, LOCK_RANK_SOCKET, "unix-sock");
    s->state = U_UNBOUND;
    s->type = type;
    s->name_len = 0;
    s->bl_count = s->bl_head = 0;
    s->bl_max = UNIX_BACKLOG_MAX;
    waitq_init(&s->acceptors);
    poll_head_init(&s->poll, "unix-listen");
    s->conn.rd = s->conn.wr = 0;
    for (int i = 0; i < UNIX_BACKLOG_MAX; i++) { s->backlog[i].rd = s->backlog[i].wr = 0; }
    s->refs = 1;
    return s;
}

static int64_t unix_install(struct unix_sock *s, int nonblock) {
    struct process *p = current_proc();
    int fd = fd_table_alloc(p->fd_table);
    if (fd < 0) { return fd; }
    struct file_descriptor *f = fd_table_get(p->fd_table, fd);
    if (!f) { fd_table_close(p->fd_table, fd); return -EBADF; }
    f->ops      = &unix_ops;
    f->priv     = s;
    f->readable = 1;
    f->writable = 1;
    f->nonblock = nonblock ? 1 : 0;
    return fd;
}

int64_t unix_socket_create(int type, int protocol) {
    int base = type & SOCK_TYPE_MASK;
    if (base != SOCK_STREAM) { return -EPROTOTYPE; }   // no AF_UNIX datagrams
    if (protocol != 0) { return -EPROTONOSUPPORT; }
    if (!current_proc()) { return -ESRCH; }

    struct unix_sock *s = unix_alloc(base);
    if (!s) { return -ENOMEM; }
    int64_t fd = unix_install(s, type & SOCK_NONBLOCK);
    if (fd < 0) { kfree(s); }
    return fd;
}

// ---- bind / listen / connect / accept ---------------------------------

int64_t unix_bind(struct file_descriptor *f, const struct k_sockaddr *addr, uint32_t len) {
    struct unix_sock *s = f->priv;
    char name[UNIX_PATH_MAX];
    uint32_t nlen = 0;
    int64_t rc = unix_extract_name(addr, len, name, &nlen);
    if (rc < 0) { return rc; }
    if (s->state != U_UNBOUND) { return -EINVAL; }

    uint64_t fl = spin_lock_irqsave(&unix_table_lock);
    int slot = -1;
    for (int i = 0; i < UNIX_MAX_BOUND; i++) {
        if (bound[i] && unix_name_eq(bound[i], name, nlen)) {
            spin_unlock_irqrestore(&unix_table_lock, fl);
            return -EADDRINUSE;
        }
        if (!bound[i] && slot < 0) { slot = i; }
    }
    if (slot < 0) { spin_unlock_irqrestore(&unix_table_lock, fl); return -ENOSPC; }
    for (uint32_t i = 0; i < nlen; i++) { s->name[i] = name[i]; }
    s->name_len = nlen;
    s->state = U_BOUND;
    bound[slot] = s;
    spin_unlock_irqrestore(&unix_table_lock, fl);
    return 0;
}

int64_t unix_listen(struct file_descriptor *f, int backlog) {
    struct unix_sock *s = f->priv;
    if (s->state != U_BOUND && s->state != U_LISTENING) { return -EINVAL; }
    if (backlog < 1) { backlog = 1; }
    if (backlog > UNIX_BACKLOG_MAX) { backlog = UNIX_BACKLOG_MAX; }
    uint64_t fl = spin_lock_irqsave(&s->lock);
    s->bl_max = backlog;
    s->state = U_LISTENING;
    spin_unlock_irqrestore(&s->lock, fl);
    return 0;
}

int64_t unix_connect(struct file_descriptor *f, const struct k_sockaddr *addr, uint32_t len) {
    struct unix_sock *s = f->priv;
    char name[UNIX_PATH_MAX];
    uint32_t nlen = 0;
    int64_t rc = unix_extract_name(addr, len, name, &nlen);
    if (rc < 0) { return rc; }
    if (s->state == U_CONNECTED) { return -EISCONN; }
    if (s->state != U_UNBOUND && s->state != U_BOUND) { return -EINVAL; }

    // Find the listener and take a reference, so it cannot be freed
    // between here and the queueing below.
    uint64_t fl = spin_lock_irqsave(&unix_table_lock);
    struct unix_sock *srv = 0;
    for (int i = 0; i < UNIX_MAX_BOUND; i++) {
        if (bound[i] && unix_name_eq(bound[i], name, nlen)) { srv = bound[i]; break; }
    }
    if (srv && srv->state == U_LISTENING) { unix_get(srv); } else { srv = 0; }
    spin_unlock_irqrestore(&unix_table_lock, fl);
    if (!srv) { return -ECONNREFUSED; }

    // Two rings, cross-wired. Allocated outside every socket lock: pipe
    // ranks below both of them.
    struct pipe *a = pipe_alloc();
    struct pipe *b = a ? pipe_alloc() : 0;
    if (!a || !b) {
        if (a) { pipe_free(a); }
        unix_put(srv);
        return -ENOMEM;
    }
    pipe_init_ends(a);
    pipe_init_ends(b);

    fl = spin_lock_irqsave(&srv->lock);
    if (srv->state != U_LISTENING || srv->bl_count >= srv->bl_max) {
        spin_unlock_irqrestore(&srv->lock, fl);
        pipe_free(a); pipe_free(b);
        unix_put(srv);
        return -ECONNREFUSED;
    }
    int idx = (srv->bl_head + srv->bl_count) % UNIX_BACKLOG_MAX;
    srv->backlog[idx].rd = a;      // server reads a, client writes a
    srv->backlog[idx].wr = b;      // server writes b, client reads b
    srv->bl_count++;
    spin_unlock_irqrestore(&srv->lock, fl);

    waitq_wake_all(&srv->acceptors);
    poll_head_notify(&srv->poll);
    waitq_poll_notify();
    unix_put(srv);

    s->conn.rd = b;
    s->conn.wr = a;
    s->state = U_CONNECTED;
    return 0;
}

int64_t unix_accept4(struct file_descriptor *f, struct k_sockaddr *addr,
                     uint32_t *len, int flags) {
    struct unix_sock *s = f->priv;
    if (s->state != U_LISTENING) { return -EINVAL; }

    struct unix_conn c;
    uint64_t fl = spin_lock_irqsave(&s->lock);
    for (;;) {
        if (s->bl_count > 0) {
            c = s->backlog[s->bl_head];
            s->backlog[s->bl_head].rd = s->backlog[s->bl_head].wr = 0;
            s->bl_head = (s->bl_head + 1) % UNIX_BACKLOG_MAX;
            s->bl_count--;
            break;
        }
        if (f->nonblock) { spin_unlock_irqrestore(&s->lock, fl); return -EAGAIN; }
        int rc = waitq_sleep(&s->acceptors, &s->lock);
        if (rc == -EINTR) { spin_unlock_irqrestore(&s->lock, fl); return -EINTR; }
    }
    spin_unlock_irqrestore(&s->lock, fl);

    struct unix_sock *ns = unix_alloc(s->type);
    if (!ns) { unix_conn_close(&c); return -ENOMEM; }
    ns->conn = c;
    ns->state = U_CONNECTED;

    int64_t fd = unix_install(ns, flags & SOCK_NONBLOCK);
    if (fd < 0) { unix_conn_close(&c); kfree(ns); return fd; }

    // An accepted AF_UNIX socket has no address of its own to report.
    if (addr && len) { *len = 0; }
    return fd;
}
