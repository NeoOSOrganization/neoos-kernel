// kernel/net/socket.c -- AF_INET datagram sockets.
//
// A socket is a file descriptor like any other: it carries a file_ops
// table, so read/write/close work on one without the syscall layer
// knowing what it is. That seam already existed for pipes, which is
// most of why sockets were cheap to add.
//
// SOCK_DGRAM only. TCP is not here, and pretending otherwise by
// accepting SOCK_STREAM and behaving like UDP would be worse than
// refusing it: a program would get message boundaries where it expects
// a stream, and silently corrupt its own protocol. SOCK_STREAM returns
// -EPROTONOSUPPORT until there is a real TCP behind it.

#include "net/socket.h"
#include "net/net.h"
#include "fs/file.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "errno.h"
#include "dev/serial.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "sched/proc.h"
#include "sched/fd_table.h"

// One page of queued datagrams per socket, which is a policy choice
// rather than a limit of anything: UDP is allowed to drop, and a
// receive queue that grows without bound turns a slow reader into an
// out-of-memory condition for the whole machine.
#define SOCK_RCVBUF 65536

// The ephemeral range Linux uses by default, and what bind(port 0) and
// an unbound sendto draw from.
#define EPHEMERAL_FIRST 32768
#define EPHEMERAL_LAST  60999

struct dgram {
    struct dgram *next;
    uint32_t      src_ip_n;
    uint16_t      src_port_n;
    uint32_t      len;
    uint8_t       data[];
};

struct socket {
    struct spinlock lock;        // LOCK_RANK_SOCKET
    struct waitq    readers;

    int      type;
    int      refs;

    int      bound;
    uint32_t local_ip_n;
    uint16_t local_port_n;

    int      connected;
    uint32_t peer_ip_n;
    uint16_t peer_port_n;

    struct dgram *rx_head, *rx_tail;
    uint32_t      rx_bytes;
    uint64_t      rx_dropped;    // datagrams refused for want of buffer

    struct socket *next;         // bound-socket table chain
};

// Every socket that has a local port, for demux. A list rather than a
// hash: NeoOS has never had more than a handful open at once, and a
// list that is obviously correct beats a table that is nearly so.
static struct socket  *bound_list;
static struct spinlock sock_table_lock;   // LOCK_RANK_SOCKTABLE
static uint16_t        ephemeral_next = EPHEMERAL_FIRST;

void socket_init(void) {
    spin_init(&sock_table_lock, LOCK_RANK_SOCKTABLE, "socktable");
    bound_list = 0;
}

// ------------------------------------------------------------ port table

// Caller holds sock_table_lock.
static struct socket *lookup_bound(uint32_t dst_ip_n, uint16_t dport_n) {
    struct socket *wildcard = 0;
    for (struct socket *s = bound_list; s; s = s->next) {
        if (s->local_port_n != dport_n) { continue; }
        // An exact address match wins over a wildcard bind, which is
        // what lets one program bind 127.0.0.1:53 while another holds
        // 0.0.0.0:53 -- Linux's rule, and the reason this is a two-pass
        // decision rather than a first match.
        if (s->local_ip_n == dst_ip_n) { return s; }
        if (s->local_ip_n == 0) { wildcard = s; }
    }
    return wildcard;
}

// Caller holds sock_table_lock.
static int port_in_use(uint32_t ip_n, uint16_t port_n) {
    for (struct socket *s = bound_list; s; s = s->next) {
        if (s->local_port_n != port_n) { continue; }
        // A wildcard bind conflicts with everything on that port, and
        // an exact bind conflicts with the same address and with a
        // wildcard.
        if (s->local_ip_n == ip_n || s->local_ip_n == 0 || ip_n == 0) {
            return 1;
        }
    }
    return 0;
}

// Caller holds sock_table_lock. Returns 0 if every ephemeral port is
// taken, which with a 28000-port range means something is leaking.
static uint16_t alloc_ephemeral(uint32_t ip_n) {
    for (int tries = 0; tries <= EPHEMERAL_LAST - EPHEMERAL_FIRST; tries++) {
        uint16_t p = ephemeral_next++;
        if (ephemeral_next > EPHEMERAL_LAST) { ephemeral_next = EPHEMERAL_FIRST; }
        if (!port_in_use(ip_n, htons_k(p))) { return htons_k(p); }
    }
    return 0;
}

// Caller holds sock_table_lock.
static void table_insert(struct socket *s) {
    s->next = bound_list;
    bound_list = s;
}

static void table_remove(struct socket *s) {
    uint64_t f = spin_lock_irqsave(&sock_table_lock);
    struct socket **pp = &bound_list;
    while (*pp && *pp != s) { pp = &(*pp)->next; }
    if (*pp) { *pp = s->next; }
    s->next = 0;
    spin_unlock_irqrestore(&sock_table_lock, f);
}

// ------------------------------------------------------------- lifecycle

static struct socket *sock_alloc(int type) {
    struct socket *s = (struct socket *)kmalloc(sizeof(struct socket));
    if (!s) { return 0; }
    for (unsigned i = 0; i < sizeof(*s); i++) { ((uint8_t *)s)[i] = 0; }
    spin_init(&s->lock, LOCK_RANK_SOCKET, "socket");
    waitq_init(&s->readers);
    s->type = type;
    s->refs = 1;
    return s;
}

static void sock_free(struct socket *s) {
    struct dgram *d = s->rx_head;
    while (d) { struct dgram *n = d->next; kfree(d); d = n; }
    kfree(s);
}

// Drops one reference. The last one takes the socket off the demux
// table and frees it. Callers must NOT hold s->lock -- table_remove()
// takes sock_table_lock, which ranks below the socket lock.
static void sock_put(struct socket *s) {
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) != 0) { return; }
    // Off the demux table BEFORE freeing, or a datagram arriving in the
    // gap is delivered into freed memory.
    if (s->bound) { table_remove(s); }
    sock_free(s);
}

// ------------------------------------------------------------ receive path

// Called from net.c's UDP input, which runs in the SENDER's context on
// loopback. It therefore must not do anything that could block, and it
// must not take a lock the sender already holds -- which is why the
// send path below drops the socket lock before handing anything to the
// network.
void net_udp_deliver(uint32_t src_n, uint16_t sport_n,
                     uint32_t dst_n, uint16_t dport_n,
                     const uint8_t *data, uint32_t len) {
    uint64_t tf = spin_lock_irqsave(&sock_table_lock);
    struct socket *s = lookup_bound(dst_n, dport_n);
    if (!s) {
        spin_unlock_irqrestore(&sock_table_lock, tf);
        // No listener. A real stack answers with ICMP port unreachable;
        // there is no ICMP here, so the datagram is simply dropped --
        // which is what UDP allows and what a caller must cope with
        // anyway.
        return;
    }

    // The socket lock is taken UNDER the table lock, which is why the
    // two have different ranks: it is what stops the socket being
    // closed and freed between finding it and using it.
    uint64_t sf = spin_lock_irqsave(&s->lock);
    spin_unlock_irqrestore(&sock_table_lock, tf);

    // A connected socket accepts datagrams only from its peer. That is
    // what "connected" means for UDP -- a filter, not a circuit.
    if (s->connected &&
        (s->peer_ip_n != src_n || s->peer_port_n != sport_n)) {
        spin_unlock_irqrestore(&s->lock, sf);
        return;
    }

    if (s->rx_bytes + len > SOCK_RCVBUF) {
        s->rx_dropped++;
        spin_unlock_irqrestore(&s->lock, sf);
        return;   // UDP: dropping is a legal outcome, not an error
    }

    struct dgram *d = (struct dgram *)kmalloc(sizeof(struct dgram) + len);
    if (!d) {
        s->rx_dropped++;
        spin_unlock_irqrestore(&s->lock, sf);
        return;
    }
    d->next       = 0;
    d->src_ip_n   = src_n;
    d->src_port_n = sport_n;
    d->len        = len;
    for (uint32_t i = 0; i < len; i++) { d->data[i] = data[i]; }

    if (s->rx_tail) { s->rx_tail->next = d; } else { s->rx_head = d; }
    s->rx_tail  = d;
    s->rx_bytes += len;
    spin_unlock_irqrestore(&s->lock, sf);

    waitq_wake_all(&s->readers);
}

// --------------------------------------------------------------- syscalls

static struct socket *sock_of(int fd, struct file_descriptor **out_f) {
    struct process *p = current_proc();
    if (!p) { return 0; }
    struct file_descriptor *f = fd_table_get(p->fd_table, fd);
    if (!f || f->ops != socket_file_ops()) { return 0; }
    if (out_f) { *out_f = f; }
    return (struct socket *)f->priv;
}

// Validates a sockaddr coming from userland and extracts the pair.
static int addr_in(const struct k_sockaddr *addr, uint32_t len,
                   uint32_t *ip_n, uint16_t *port_n) {
    if (!addr || len < sizeof(struct k_sockaddr_in)) { return -EINVAL; }
    if (!user_range_writable((uint64_t)(uintptr_t)addr,
                             sizeof(struct k_sockaddr_in))) {
        return -EFAULT;
    }
    const struct k_sockaddr_in *in = (const struct k_sockaddr_in *)addr;
    if (in->sin_family != AF_INET) { return -EAFNOSUPPORT; }
    *ip_n   = in->sin_addr.s_addr;
    *port_n = in->sin_port;
    return 0;
}

static int addr_out(struct k_sockaddr *addr, uint32_t *len,
                    uint32_t ip_n, uint16_t port_n) {
    if (!addr || !len) { return 0; }   // both optional, as POSIX has them
    if (!user_range_writable((uint64_t)(uintptr_t)len, sizeof(uint32_t))) {
        return -EFAULT;
    }
    uint32_t want = sizeof(struct k_sockaddr_in);
    if (!user_range_writable((uint64_t)(uintptr_t)addr, want)) { return -EFAULT; }

    struct k_sockaddr_in in;
    for (unsigned i = 0; i < sizeof(in); i++) { ((uint8_t *)&in)[i] = 0; }
    in.sin_family      = AF_INET;
    in.sin_port        = port_n;
    in.sin_addr.s_addr = ip_n;

    // POSIX truncates rather than failing, and reports the size it
    // WOULD have needed -- so a caller with a small buffer can tell.
    uint32_t give = *len < want ? *len : want;
    for (uint32_t i = 0; i < give; i++) { ((uint8_t *)addr)[i] = ((uint8_t *)&in)[i]; }
    *len = want;
    return 0;
}

int64_t socket_create(int domain, int type, int protocol) {
    if (domain != AF_INET) { return -EAFNOSUPPORT; }
    if (type == SOCK_STREAM) { return -EPROTONOSUPPORT; }  // no TCP yet
    if (type != SOCK_DGRAM)  { return -EPROTONOSUPPORT; }
    if (protocol != 0 && protocol != IPPROTO_UDP) { return -EPROTONOSUPPORT; }

    struct process *p = current_proc();
    if (!p) { return -ESRCH; }

    struct socket *s = sock_alloc(type);
    if (!s) { return -ENOMEM; }

    int fd = fd_table_alloc(p->fd_table);
    if (fd < 0) { sock_free(s); return fd; }

    struct file_descriptor *f = fd_table_get(p->fd_table, fd);
    if (!f) { fd_table_close(p->fd_table, fd); sock_free(s); return -EBADF; }
    f->ops      = socket_file_ops();
    f->priv     = s;
    f->readable = 1;
    f->writable = 1;
    return fd;
}

int64_t socket_bind(int fd, const struct k_sockaddr *addr, uint32_t len) {
    struct socket *s = sock_of(fd, 0);
    if (!s) { return -EBADF; }

    uint32_t ip_n; uint16_t port_n;
    int rc = addr_in(addr, len, &ip_n, &port_n);
    if (rc != 0) { return rc; }

    // Only an address this host owns can be bound. INADDR_ANY means
    // "whatever the packet came in on", which with one interface is
    // still just loopback, but is kept distinct because the demux rule
    // above depends on the difference.
    if (ip_n != 0 && !net_route(ip_n)) { return -EADDRNOTAVAIL; }

    uint64_t tf = spin_lock_irqsave(&sock_table_lock);
    if (s->bound) { spin_unlock_irqrestore(&sock_table_lock, tf); return -EINVAL; }

    if (port_n == 0) {
        port_n = alloc_ephemeral(ip_n);
        if (!port_n) { spin_unlock_irqrestore(&sock_table_lock, tf); return -EADDRINUSE; }
    } else if (port_in_use(ip_n, port_n)) {
        spin_unlock_irqrestore(&sock_table_lock, tf);
        return -EADDRINUSE;
    }

    s->local_ip_n   = ip_n;
    s->local_port_n = port_n;
    s->bound        = 1;
    table_insert(s);
    spin_unlock_irqrestore(&sock_table_lock, tf);
    return 0;
}

int64_t socket_connect(int fd, const struct k_sockaddr *addr, uint32_t len) {
    struct socket *s = sock_of(fd, 0);
    if (!s) { return -EBADF; }

    uint32_t ip_n; uint16_t port_n;
    int rc = addr_in(addr, len, &ip_n, &port_n);
    if (rc != 0) { return rc; }
    if (!net_route(ip_n)) { return -ENETUNREACH; }

    // A connected datagram socket needs a source port, so that replies
    // have somewhere to go. Linux binds one implicitly here and so does
    // this.
    if (!s->bound) {
        uint64_t tf = spin_lock_irqsave(&sock_table_lock);
        uint16_t p = alloc_ephemeral(0);
        if (!p) { spin_unlock_irqrestore(&sock_table_lock, tf); return -EADDRINUSE; }
        s->local_ip_n   = 0;
        s->local_port_n = p;
        s->bound        = 1;
        table_insert(s);
        spin_unlock_irqrestore(&sock_table_lock, tf);
    }

    uint64_t sf = spin_lock_irqsave(&s->lock);
    s->peer_ip_n   = ip_n;
    s->peer_port_n = port_n;
    s->connected   = 1;
    spin_unlock_irqrestore(&s->lock, sf);
    return 0;
}

int64_t socket_getsockname(int fd, struct k_sockaddr *addr, uint32_t *len) {
    struct socket *s = sock_of(fd, 0);
    if (!s) { return -EBADF; }
    return addr_out(addr, len, s->local_ip_n, s->local_port_n);
}

int64_t socket_sendto(int fd, const void *buf, uint64_t len, int flags,
                      const struct k_sockaddr *dest, uint32_t dest_len) {
    (void)flags;   // MSG_* are all unimplemented; see docs/stdlib.md
    struct socket *s = sock_of(fd, 0);
    if (!s) { return -EBADF; }
    if (!user_range_writable((uint64_t)(uintptr_t)buf, len)) { return -EFAULT; }

    uint32_t dst_ip_n; uint16_t dst_port_n;
    if (dest) {
        int rc = addr_in(dest, dest_len, &dst_ip_n, &dst_port_n);
        if (rc != 0) { return rc; }
    } else {
        if (!s->connected) { return -EDESTADDRREQ; }
        dst_ip_n   = s->peer_ip_n;
        dst_port_n = s->peer_port_n;
    }

    // An unbound socket gets a source port here, for the same reason
    // connect does: without one, nothing can reply.
    if (!s->bound) {
        uint64_t tf = spin_lock_irqsave(&sock_table_lock);
        if (!s->bound) {
            uint16_t p = alloc_ephemeral(0);
            if (!p) { spin_unlock_irqrestore(&sock_table_lock, tf); return -EADDRINUSE; }
            s->local_ip_n   = 0;
            s->local_port_n = p;
            s->bound        = 1;
            table_insert(s);
        }
        spin_unlock_irqrestore(&sock_table_lock, tf);
    }

    // NO SOCKET LOCK IS HELD HERE, deliberately. On loopback the send
    // path runs straight into the receive path, which locks the
    // DESTINATION socket -- and a program sending to itself would then
    // take the same lock twice.
    uint32_t src_ip_n = s->local_ip_n ? s->local_ip_n : IP_LOOPBACK_N;
    int rc = net_udp_output(src_ip_n, s->local_port_n,
                            dst_ip_n ? dst_ip_n : IP_LOOPBACK_N, dst_port_n,
                            (const uint8_t *)buf, (uint32_t)len);
    if (rc != 0) { return rc; }
    return (int64_t)len;
}

// Dequeues one datagram, blocking if there is none. Shared by
// recvfrom() and by read() on a socket, which is the same operation
// with nowhere to report the sender.
static int64_t recv_one(struct socket *s, int nonblock, void *buf, uint64_t len,
                        struct k_sockaddr *src, uint32_t *src_len) {
    // Hold a reference across the (possibly blocking) wait. A close() on
    // another thread of this process -- or this thread being SIGKILLed
    // as its process exits, which closes every fd -- can drop the fd's
    // reference to zero while we are parked on s->readers. Without this
    // ref, sock_free() would pull s->lock and s->readers out from under
    // waitq_sleep()'s own re-acquire, and schedule() would then run
    // against a freed lock (seen as a bogus "[lock] PANIC: schedule()
    // with a spinlock held ... holding=socktable").
    __atomic_fetch_add(&s->refs, 1, __ATOMIC_ACQ_REL);

    uint64_t sf = spin_lock_irqsave(&s->lock);
    while (!s->rx_head) {
        if (nonblock) {
            spin_unlock_irqrestore(&s->lock, sf);
            sock_put(s);
            return -EAGAIN;
        }
        int rc = waitq_sleep(&s->readers, &s->lock);
        if (rc == -EINTR) {
            spin_unlock_irqrestore(&s->lock, sf);
            sock_put(s);
            return -EINTR;
        }
    }

    struct dgram *d = s->rx_head;
    s->rx_head = d->next;
    if (!s->rx_head) { s->rx_tail = 0; }
    s->rx_bytes -= d->len;
    spin_unlock_irqrestore(&s->lock, sf);

    // A datagram larger than the buffer is TRUNCATED and the rest
    // discarded -- the whole message is consumed either way, because
    // that is what a message boundary means. Linux reports the excess
    // via MSG_TRUNC, which is not implemented; the return value is the
    // number of bytes actually delivered.
    uint32_t give = d->len < len ? d->len : (uint32_t)len;
    for (uint32_t i = 0; i < give; i++) { ((uint8_t *)buf)[i] = d->data[i]; }

    int rc = addr_out(src, src_len, d->src_ip_n, d->src_port_n);
    kfree(d);
    sock_put(s);
    if (rc != 0) { return rc; }
    return (int64_t)give;
}

int64_t socket_recvfrom(int fd, void *buf, uint64_t len, int flags,
                        struct k_sockaddr *src, uint32_t *src_len) {
    (void)flags;   // MSG_* are all unimplemented; see docs/stdlib.md
    struct file_descriptor *f = 0;
    struct socket *s = sock_of(fd, &f);
    if (!s) { return -EBADF; }
    if (!user_range_writable((uint64_t)(uintptr_t)buf, len)) { return -EFAULT; }
    return recv_one(s, f->nonblock, buf, len, src, src_len);
}

// -------------------------------------------------------------- file_ops

// read/write on a socket are recvfrom/sendto with no address, which is
// exactly what POSIX says they are. They work only on a connected
// socket, because an unconnected one has nowhere to send.
static int64_t sock_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return -EBADF; }
    if (!s->bound) { return -ENOTCONN; }
    return recv_one(s, f->nonblock, buf, len, 0, 0);
}

static int64_t sock_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return -EBADF; }
    if (!s->connected) { return -ENOTCONN; }
    uint32_t src_ip_n = s->local_ip_n ? s->local_ip_n : IP_LOOPBACK_N;
    int rc = net_udp_output(src_ip_n, s->local_port_n,
                            s->peer_ip_n, s->peer_port_n,
                            (const uint8_t *)buf, (uint32_t)len);
    if (rc != 0) { return rc; }
    return (int64_t)len;
}

static int64_t sock_lseek(struct file_descriptor *f, int64_t off, int whence) {
    (void)f; (void)off; (void)whence;
    return -ESPIPE;
}

static int64_t sock_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

static void sock_dup(struct file_descriptor *f) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return; }
    // Lock-free for the same reason pipe_dup is: fd_table_dup calls it
    // under the fd table lock, which ranks above this object's.
    __atomic_fetch_add(&s->refs, 1, __ATOMIC_ACQ_REL);
}

static void sock_close(struct file_descriptor *f) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return; }
    f->priv = 0;
    sock_put(s);
}

static int64_t sock_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f; (void)request; (void)arg;
    return -ENOTTY;
}

static int sock_poll(struct file_descriptor *f, int events) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return POLLERR; }

    int mask = 0;
    if (f->readable && s->rx_head) { mask |= POLLIN; }
    if (f->writable) { mask |= POLLOUT; }

    return mask & events;
}

static const struct file_ops sock_ops = {
    .name     = "socket",
    .read     = sock_read,
    .write    = sock_write,
    .lseek    = sock_lseek,
    .getdents = sock_getdents,
    .ioctl    = sock_ioctl,
    .poll     = sock_poll,
    .dup      = sock_dup,
    .close    = sock_close,
};

const struct file_ops *socket_file_ops(void) { return &sock_ops; }

// --------------------------------------------------------------- selftest

// Asserts the struct layouts that cross the syscall boundary. Getting
// one wrong is invisible until a ported program passes a sockaddr and
// the port lands in the wrong half of a word -- at which point the
// symptom is a packet going somewhere unrelated, not a compile error.
void socket_selftest(void) {
    if (sizeof(struct k_sockaddr_in) != 16) {
        serial_write_string("[socket] selftest FAILED: sockaddr_in is not 16 bytes\n");
        return;
    }
    if (__builtin_offsetof(struct k_sockaddr_in, sin_family) != 0 ||
        __builtin_offsetof(struct k_sockaddr_in, sin_port)   != 2 ||
        __builtin_offsetof(struct k_sockaddr_in, sin_addr)   != 4 ||
        __builtin_offsetof(struct k_sockaddr_in, sin_zero)   != 8) {
        serial_write_string("[socket] selftest FAILED: sockaddr_in field offsets\n");
        return;
    }
    if (sizeof(struct k_sockaddr) != 16) {
        serial_write_string("[socket] selftest FAILED: sockaddr is not 16 bytes\n");
        return;
    }
    serial_write_string("[socket] selftest passed\n");
}
