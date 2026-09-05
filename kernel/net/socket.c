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
#include "net/tcp.h"
#include "fs/file.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sync/poll_head.h"
#include "errno.h"
#include "drivers/char/serial.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "sched/proc.h"
#include "ipc/signal.h"
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

// The SOCK_STREAM half is defined further down, beside the rest of the
// stream code, so that section stays together.
struct socket;
static int64_t stream_connect(struct socket *s, int nonblock,
                              uint32_t ip_n, uint16_t port_n);
static int64_t stream_send(struct socket *s, int nonblock,
                           const void *buf, uint64_t len);
static int64_t stream_recv(struct socket *s, int nonblock, void *buf, uint64_t len);

struct socket {
    struct spinlock lock;        // LOCK_RANK_SOCKET
    struct waitq    readers;
    struct poll_head poll;       // CS5.2: pollers registered on THIS socket

    int      type;
    int      refs;

    int      bound;
    uint32_t local_ip_n;
    uint16_t local_port_n;

    int      connected;
    uint32_t peer_ip_n;
    uint16_t peer_port_n;

    // SOCK_STREAM only. The TCB's lifetime is not this socket's -- see
    // THE LIFETIME RULE in tcp.h -- so this is a reference, released on
    // close, and the block may well outlive the release.
    struct tcb *tcb;

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
    poll_head_init(&s->poll, "socket-poll");
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
    // The connection outlives the descriptor: tcp_close sends FIN and
    // leaves the TCB to finish through FIN_WAIT and TIME_WAIT with
    // nothing pointing at it. See THE LIFETIME RULE in tcp.h.
    if (s->tcb) { struct tcb *t = s->tcb; s->tcb = 0; tcp_close(t); }
    sock_free(s);
}

// ------------------------------------------------------------ receive path

// Called from net.c's UDP input, which runs in the SENDER's context on
// loopback. It therefore must not do anything that could block, and it
// must not take a lock the sender already holds -- which is why the
// send path below drops the socket lock before handing anything to the
// network.
int net_udp_deliver(uint32_t src_n, uint16_t sport_n,
                    uint32_t dst_n, uint16_t dport_n,
                    const uint8_t *data, uint32_t len) {
    uint64_t tf = spin_lock_irqsave(&sock_table_lock);
    struct socket *s = lookup_bound(dst_n, dport_n);
    if (!s) {
        spin_unlock_irqrestore(&sock_table_lock, tf);
        // No listener. D3's caller turns this into an ICMP port
        // unreachable -- it is reported rather than answered here
        // because socket.c has no business knowing what ICMP is.
        return -ENOENT;
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
        // Delivered as far as the caller is concerned: a socket exists
        // on that port, and a connected socket filtering out a stranger
        // is not the same thing as a closed port. Answering this with a
        // port-unreachable would tell the sender the port is shut when
        // it is merely not listening to THEM.
        return 0;
    }

    if (s->rx_bytes + len > SOCK_RCVBUF) {
        s->rx_dropped++;
        spin_unlock_irqrestore(&s->lock, sf);
        return 0;   // UDP: dropping is a legal outcome, not an error
    }

    struct dgram *d = (struct dgram *)kmalloc(sizeof(struct dgram) + len);
    if (!d) {
        s->rx_dropped++;
        spin_unlock_irqrestore(&s->lock, sf);
        return 0;
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
    poll_head_notify(&s->poll);   // CS5.2: only this socket's pollers
    return 0;
}

// --------------------------------------------------------------- syscalls

// Returns the socket behind `fd` with a REFERENCE HELD, or 0. Every
// caller must sock_put() it on every exit path.
//
// The reference is taken under the fd bucket lock, which is the only
// window in which the slot -- and therefore the object behind it -- is
// guaranteed to still exist. The previous version returned f->priv with
// nothing held and let recv_one take the reference afterwards, which is
// too late: fd_table_close clears the slot under that lock and then runs
// file_close OUTSIDE it, so a concurrent close could free the socket
// between the lookup and the increment. The increment then landed in
// freed memory, and recv_one went on to waitq_sleep on a lock inside it.
//
// That is what the gauntlet's long-standing
//   [lock] PANIC: schedule() with a spinlock held ... holding=socktable
// actually was. Nothing holds socktable there; schedule() consults
// per-CPU held-lock bookkeeping that a freed-and-reused lock had
// corrupted, and "socktable" is simply what the garbage said.
//
// Only an atomic increment happens under the bucket lock -- nothing that
// sleeps, and no second lock -- because fd_table_close deliberately runs
// the closing side outside it.
static struct socket *sock_ref_of(int fd, int *nonblock) {
    struct process *p = current_proc();
    if (!p) { return 0; }
    uint64_t fl = 0;
    struct file_descriptor *f = fd_table_lock_slot(p->fd_table, fd, &fl);
    if (!f) { return 0; }

    struct socket *s = 0;
    if (f->ops == socket_file_ops() && f->priv) {
        s = (struct socket *)f->priv;
        __atomic_fetch_add(&s->refs, 1, __ATOMIC_ACQ_REL);
        if (nonblock) { *nonblock = f->nonblock; }
    }
    fd_table_unlock_slot(p->fd_table, fd, fl);
    return s;
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
    if (type != SOCK_STREAM && type != SOCK_DGRAM) { return -EPROTONOSUPPORT; }
    if (protocol != 0 && protocol != IPPROTO_UDP) { return -EPROTONOSUPPORT; }

    struct process *p = current_proc();
    if (!p) { return -ESRCH; }

    struct socket *s = sock_alloc(type);
    if (s && type == SOCK_STREAM) {
        s->tcb = tcp_alloc();
        if (!s->tcb) { sock_free(s); return -ENOBUFS; }
    }
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
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }

    uint32_t ip_n; uint16_t port_n;
    int rc = addr_in(addr, len, &ip_n, &port_n);
    if (rc != 0) { sock_put(s); return rc; }

    // Only an address this host owns can be bound. INADDR_ANY means
    // "whatever the packet came in on", which with one interface is
    // still just loopback, but is kept distinct because the demux rule
    // above depends on the difference.
    // NOT net_route: an address being reachable does not make it ours.
    // See net_is_local_addr -- once a default route existed,
    // bind(8.8.8.8) started succeeding.
    if (!net_is_local_addr(ip_n)) { sock_put(s); return -EADDRNOTAVAIL; }

    uint64_t tf = spin_lock_irqsave(&sock_table_lock);
    if (s->bound) {
        spin_unlock_irqrestore(&sock_table_lock, tf);
        sock_put(s);
        return -EINVAL;
    }

    if (port_n == 0) {
        port_n = alloc_ephemeral(ip_n);
        if (!port_n) {
            spin_unlock_irqrestore(&sock_table_lock, tf);
            sock_put(s);
            return -EADDRINUSE;
        }
    } else if (port_in_use(ip_n, port_n)) {
        spin_unlock_irqrestore(&sock_table_lock, tf);
        sock_put(s);
        return -EADDRINUSE;
    }

    s->local_ip_n   = ip_n;
    s->local_port_n = port_n;
    s->bound        = 1;
    table_insert(s);
    spin_unlock_irqrestore(&sock_table_lock, tf);
    if (s->tcb) {
        // The TCB demuxes by 4-tuple, so it has to learn the local half
        // here. A listener keeps local_n == 0 (INADDR_ANY), which
        // tcp_find_listener treats as "any local address".
        s->tcb->local_n = ip_n;
        s->tcb->lport_n = port_n;
    }
    sock_put(s);
    return 0;
}

int64_t socket_connect(int fd, const struct k_sockaddr *addr, uint32_t len) {
    int nonblock = 0;
    struct socket *s = sock_ref_of(fd, &nonblock);
    if (!s) { return -EBADF; }

    uint32_t ip_n; uint16_t port_n;
    int rc = addr_in(addr, len, &ip_n, &port_n);
    if (rc != 0) { sock_put(s); return rc; }
    if (!net_route(ip_n)) { sock_put(s); return -ENETUNREACH; }

    if (s->type == SOCK_STREAM) {
        int64_t r = stream_connect(s, nonblock, ip_n, port_n);
        sock_put(s);
        return r;
    }

    // A connected datagram socket needs a source port, so that replies
    // have somewhere to go. Linux binds one implicitly here and so does
    // this.
    if (!s->bound) {
        uint64_t tf = spin_lock_irqsave(&sock_table_lock);
        uint16_t p = alloc_ephemeral(0);
        if (!p) {
            spin_unlock_irqrestore(&sock_table_lock, tf);
            sock_put(s);
            return -EADDRINUSE;
        }
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
    sock_put(s);
    return 0;
}

int64_t socket_getsockname(int fd, struct k_sockaddr *addr, uint32_t *len) {
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }
    int64_t rc = addr_out(addr, len, s->local_ip_n, s->local_port_n);
    sock_put(s);
    return rc;
}

int64_t socket_sendto(int fd, const void *buf, uint64_t len, int flags,
                      const struct k_sockaddr *dest, uint32_t dest_len) {
    (void)flags;   // MSG_* are all unimplemented; see docs/stdlib.md
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }
    // send only READS buf -- a string literal in .rodata is a valid
    // source (it stopped being writable when the ELF loader started
    // honouring p_flags).
    if (!user_range_readable((uint64_t)(uintptr_t)buf, len)) { sock_put(s); return -EFAULT; }

    uint32_t dst_ip_n; uint16_t dst_port_n;
    if (dest) {
        int rc = addr_in(dest, dest_len, &dst_ip_n, &dst_port_n);
        if (rc != 0) { sock_put(s); return rc; }
    } else {
        if (!s->connected) { sock_put(s); return -EDESTADDRREQ; }
        dst_ip_n   = s->peer_ip_n;
        dst_port_n = s->peer_port_n;
    }

    // An unbound socket gets a source port here, for the same reason
    // connect does: without one, nothing can reply.
    if (!s->bound) {
        uint64_t tf = spin_lock_irqsave(&sock_table_lock);
        if (!s->bound) {
            uint16_t p = alloc_ephemeral(0);
            if (!p) {
                spin_unlock_irqrestore(&sock_table_lock, tf);
                sock_put(s);
                return -EADDRINUSE;
            }
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
    sock_put(s);
    if (rc != 0) { return rc; }
    return (int64_t)len;
}

// Dequeues one datagram, blocking if there is none. Shared by
// recvfrom() and by read() on a socket, which is the same operation
// with nowhere to report the sender.
static int64_t recv_one(struct socket *s, int nonblock, void *buf, uint64_t len,
                        struct k_sockaddr *src, uint32_t *src_len) {
    // A reference IS held across the (possibly blocking) wait below --
    // by the CALLER. sock_ref_of takes it under the fd table's bucket
    // lock and every caller releases it on every exit path, so the
    // socket cannot be freed while this function is parked on
    // s->readers.
    //
    // This function used to take a SECOND reference here and never drop
    // it: none of the four exits below released it, so every recvfrom
    // leaked one and no socket that had ever been read from was freed.
    // It was written when the caller's reference was taken too late to
    // help (see sock_ref_of); once that moved under the bucket lock this
    // became redundant as well as leaky.

    uint64_t sf = spin_lock_irqsave(&s->lock);
    while (!s->rx_head) {
        if (nonblock) {
            spin_unlock_irqrestore(&s->lock, sf);
            return -EAGAIN;
        }
        int rc = waitq_sleep(&s->readers, &s->lock);
        if (rc == -EINTR) {
            spin_unlock_irqrestore(&s->lock, sf);
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
    if (rc != 0) { return rc; }
    return (int64_t)give;
}

int64_t socket_recvfrom(int fd, void *buf, uint64_t len, int flags,
                        struct k_sockaddr *src, uint32_t *src_len) {
    (void)flags;   // MSG_* are all unimplemented; see docs/stdlib.md
    int nonblock = 0;
    struct socket *s = sock_ref_of(fd, &nonblock);
    if (!s) { return -EBADF; }
    if (!user_range_writable((uint64_t)(uintptr_t)buf, len)) {
        sock_put(s);
        return -EFAULT;
    }
    int64_t rc = recv_one(s, nonblock, buf, len, src, src_len);
    sock_put(s);
    return rc;
}


// ===================================================================
// SOCK_STREAM
// ===================================================================
//
// This lives here rather than in the tcp_sock.c the plan called for,
// and the reason is `struct socket`: it is file-private, together with
// sock_ref_of, addr_in and addr_out, and splitting would have meant
// exporting four statics out of a file that works in order to move
// four hundred lines out of it. The section marker is the compromise.
//
// THE RULE FOR EVERY FUNCTION BELOW: the socket lock is never held
// across a call into tcp.c. LOCK_RANK_TCP sits above LOCK_RANK_SOCKET,
// and socket_sendto has had the same rule for UDP since it was written
// -- for the same reason, which is that loopback runs the receive path
// in the sender's context.

// How long a blocked reader or writer sleeps before rechecking.
//
// waitq_sleep on a queue this code does not hold a lock for has a
// lost-wakeup window: the state can change between the check and the
// sleep. Rather than push a spinlock down through tcp.c to close it,
// the sleep is BOUNDED -- a missed wake costs 50ms of latency, not a
// hang. This is a deliberate trade and it is the first thing to revisit
// if TCP latency ever matters here.
#define STREAM_POLL_TICKS 5

// Waits for `t` to leave SYN_SENT, or for the connection to fail.
static int stream_wait_connected(struct tcb *t) {
    for (;;) {
        if (t->state == TCP_ESTABLISHED) { return 0; }
        if (t->state == TCP_CLOSED) {
            return t->so_error ? -t->so_error : -ECONNREFUSED;
        }
        if (signal_pending_any(current_thread())) { return -EINTR; }
        waitq_sleep_timeout(&t->waiters, 0, timer_ticks() + STREAM_POLL_TICKS);
    }
}

int64_t socket_listen(int fd, int backlog) {
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }
    if (s->type != SOCK_STREAM || !s->tcb) { sock_put(s); return -EOPNOTSUPP; }
    if (!s->bound) { sock_put(s); return -EDESTADDRREQ; }
    int rc = tcp_listen(s->tcb, backlog);
    sock_put(s);
    return rc;
}

int64_t socket_accept4(int fd, struct k_sockaddr *addr, uint32_t *len, int flags) {
    int nonblock = 0;
    struct socket *s = sock_ref_of(fd, &nonblock);
    if (!s) { return -EBADF; }
    if (s->type != SOCK_STREAM || !s->tcb) { sock_put(s); return -EOPNOTSUPP; }
    struct tcb *l = s->tcb;
    if (l->state != TCP_LISTEN) { sock_put(s); return -EINVAL; }

    struct tcb *c = 0;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&l->lock);
        if (l->accept_n > 0) {
            c = l->accept_q[0];
            for (int i = 1; i < l->accept_n; i++) { l->accept_q[i - 1] = l->accept_q[i]; }
            l->accept_n--;
        }
        spin_unlock_irqrestore(&l->lock, f);
        if (c) { break; }
        if (nonblock || (flags & SOCK_NONBLOCK)) { sock_put(s); return -EAGAIN; }
        if (signal_pending_any(current_thread())) { sock_put(s); return -EINTR; }
        waitq_sleep_timeout(&l->waiters, 0, timer_ticks() + STREAM_POLL_TICKS);
    }
    sock_put(s);

    // A socket around the accepted connection. The TCB already exists
    // and already holds its own reference; this adds the socket's.
    struct socket *ns = sock_alloc(SOCK_STREAM);
    if (!ns) { tcp_close(c); return -ENOBUFS; }
    ns->tcb          = c;
    ns->bound        = 1;
    ns->connected    = 1;
    ns->local_ip_n   = c->local_n;
    ns->local_port_n = c->lport_n;
    ns->peer_ip_n    = c->remote_n;
    ns->peer_port_n  = c->rport_n;

    struct process *p = current_proc();
    int nfd = fd_table_alloc(p->fd_table);
    if (nfd < 0) { sock_free(ns); tcp_close(c); return nfd; }
    struct file_descriptor *nf = fd_table_get(p->fd_table, nfd);
    if (!nf) { fd_table_close(p->fd_table, nfd); sock_free(ns); return -EBADF; }
    nf->ops      = socket_file_ops();
    nf->priv     = ns;
    nf->readable = 1;
    nf->writable = 1;
    nf->nonblock = (flags & SOCK_NONBLOCK) ? 1 : 0;

    if (addr && len) {
        struct k_sockaddr_in sin;
        sin.sin_family = AF_INET;
        sin.sin_port   = c->rport_n;
        sin.sin_addr.s_addr = c->remote_n;
        for (int i = 0; i < 8; i++) { sin.sin_zero[i] = 0; }
        addr_out(addr, len, c->remote_n, c->rport_n);
        (void)sin;
    }
    return nfd;
}

// The stream half of connect(). Returns after the handshake, or
// -EINPROGRESS on a non-blocking socket.
static int64_t stream_connect(struct socket *s, int nonblock,
                              uint32_t ip_n, uint16_t port_n) {
    struct tcb *t = s->tcb;
    if (t->state == TCP_ESTABLISHED) { return -EISCONN; }
    if (t->state == TCP_SYN_SENT) {
        // A second connect() on a socket already connecting. Linux
        // returns EALREADY; without a distinct value here a program
        // retrying in a poll loop cannot tell that from a fresh start.
        return nonblock ? -EALREADY : stream_wait_connected(t);
    }

    if (!s->bound) {
        uint64_t tf = spin_lock_irqsave(&sock_table_lock);
        uint16_t p = alloc_ephemeral(0);
        if (!p) { spin_unlock_irqrestore(&sock_table_lock, tf); return -EADDRINUSE; }
        s->local_port_n = p;
        s->bound = 1;
        table_insert(s);
        spin_unlock_irqrestore(&sock_table_lock, tf);
        t->lport_n = p;
    }

    int rc = tcp_connect(t, ip_n, port_n);
    if (rc != 0) { return rc; }
    s->peer_ip_n   = ip_n;
    s->peer_port_n = port_n;
    s->local_ip_n  = t->local_n;

    if (nonblock) { return -EINPROGRESS; }
    rc = stream_wait_connected(t);
    if (rc == 0) { s->connected = 1; }
    return rc;
}

int64_t socket_shutdown(int fd, int how) {
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }
    if (s->type != SOCK_STREAM || !s->tcb) { sock_put(s); return -EOPNOTSUPP; }
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
        sock_put(s); return -EINVAL;
    }
    if (how == SHUT_RD || how == SHUT_RDWR) { s->tcb->shut_rd = 1; }
    if (how == SHUT_WR || how == SHUT_RDWR) { tcp_shutdown_write(s->tcb); }
    waitq_wake_all(&s->tcb->waiters);
    poll_head_notify(&s->poll);
    sock_put(s);
    return 0;
}

int64_t socket_getpeername(int fd, struct k_sockaddr *addr, uint32_t *len) {
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }
    if (!s->connected && !(s->tcb && s->tcb->state == TCP_ESTABLISHED)) {
        sock_put(s);
        return -ENOTCONN;
    }
    int rc = addr_out(addr, len, s->peer_ip_n, s->peer_port_n);
    sock_put(s);
    return rc;
}

int64_t socket_setsockopt(int fd, int level, int opt, const void *val, uint32_t len) {
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }
    if (len < 4 || !user_range_readable((uint64_t)(uintptr_t)val, 4)) {
        sock_put(s); return -EFAULT;
    }
    int v = *(const int *)val;
    int rc = 0;
    if (level == SOL_SOCKET && opt == SO_REUSEADDR) {
        if (s->tcb) { s->tcb->reuseaddr = v ? 1 : 0; }
    } else if (level == SOL_SOCKET && (opt == SO_SNDBUF || opt == SO_RCVBUF)) {
        // ACCEPTED AND IGNORED, and the divergence is recorded in
        // docs/stdlib.md. The buffers are fixed at compile time because
        // the connection table is static. Failing here instead would
        // break every program that sets a buffer size out of habit.
        rc = 0;
    } else if (level == IPPROTO_TCP && opt == TCP_NODELAY) {
        if (!s->tcb) { rc = -EOPNOTSUPP; } else { s->tcb->nodelay = v ? 1 : 0; }
    } else {
        // NOT silently successful. A program that sets an option and
        // does not get it behaves mysteriously forever after.
        rc = -ENOPROTOOPT;
    }
    sock_put(s);
    return rc;
}

int64_t socket_getsockopt(int fd, int level, int opt, void *val, uint32_t *len) {
    struct socket *s = sock_ref_of(fd, 0);
    if (!s) { return -EBADF; }
    if (!user_range_writable((uint64_t)(uintptr_t)val, 4) ||
        !user_range_writable((uint64_t)(uintptr_t)len, 4)) {
        sock_put(s); return -EFAULT;
    }
    int v = 0;
    int rc = 0;
    if (level == SOL_SOCKET && opt == SO_ERROR) {
        // Reading SO_ERROR CLEARS it, as Linux does. A non-blocking
        // connect reports its result exactly once.
        if (s->tcb) { v = s->tcb->so_error; s->tcb->so_error = 0; }
    } else if (level == SOL_SOCKET && opt == SO_REUSEADDR) {
        v = s->tcb ? s->tcb->reuseaddr : 0;
    } else if (level == SOL_SOCKET && opt == SO_SNDBUF) {
        v = TCP_SNDBUF;
    } else if (level == SOL_SOCKET && opt == SO_RCVBUF) {
        v = TCP_RCVBUF;
    } else if (level == SOL_SOCKET && opt == SO_TYPE) {
        v = s->type;
    } else if (level == IPPROTO_TCP && opt == TCP_NODELAY) {
        v = s->tcb ? s->tcb->nodelay : 0;
    } else {
        rc = -ENOPROTOOPT;
    }
    if (rc == 0) { *(int *)val = v; *len = 4; }
    sock_put(s);
    return rc;
}

// Blocking send over a stream. Loops because the send buffer is finite
// and a large write must not be truncated the way a datagram is.
static int64_t stream_send(struct socket *s, int nonblock,
                           const void *buf, uint64_t len) {
    struct tcb *t = s->tcb;
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        uint32_t sent = 0;
        int rc = tcp_send(t, p + done, (uint32_t)(len - done), &sent);
        if (rc != 0) {
            // Partial progress is reported as progress. A write that
            // moved 4000 bytes and then hit a reset returns 4000, and
            // the error surfaces on the next call -- which is what
            // POSIX requires and what every program expects.
            return done ? (int64_t)done : rc;
        }
        done += sent;
        if (done >= len) { break; }
        if (nonblock) { return done ? (int64_t)done : -EAGAIN; }
        if (signal_pending_any(current_thread())) { return done ? (int64_t)done : -EINTR; }
        waitq_sleep_timeout(&t->waiters, 0, timer_ticks() + STREAM_POLL_TICKS);
    }
    return (int64_t)done;
}

static int64_t stream_recv(struct socket *s, int nonblock, void *buf, uint64_t len) {
    struct tcb *t = s->tcb;
    for (;;) {
        uint32_t got = 0;
        int rc = tcp_recv(t, (uint8_t *)buf, (uint32_t)len, &got);
        if (rc == 0) { return (int64_t)got; }        // got == 0 means EOF
        if (rc != -EAGAIN) { return rc; }
        if (nonblock) { return -EAGAIN; }
        if (signal_pending_any(current_thread())) { return -EINTR; }
        waitq_sleep_timeout(&t->waiters, 0, timer_ticks() + STREAM_POLL_TICKS);
    }
}

// -------------------------------------------------------------- file_ops

// read/write on a socket are recvfrom/sendto with no address, which is
// exactly what POSIX says they are. They work only on a connected
// socket, because an unconnected one has nowhere to send.
static int64_t sock_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return -EBADF; }
    if (s->type == SOCK_STREAM) {
        if (!s->tcb) { return -ENOTCONN; }
        if (!user_range_writable((uint64_t)(uintptr_t)buf, len)) { return -EFAULT; }
        return stream_recv(s, f->nonblock, buf, len);
    }
    if (!s->bound) { return -ENOTCONN; }
    return recv_one(s, f->nonblock, buf, len, 0, 0);
}

static int64_t sock_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return -EBADF; }
    if (s->type == SOCK_STREAM) {
        if (!s->tcb) { return -ENOTCONN; }
        if (!user_range_readable((uint64_t)(uintptr_t)buf, len)) { return -EFAULT; }
        return stream_send(s, f->nonblock, buf, len);
    }
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

static struct poll_head *sock_poll_head(struct file_descriptor *f) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return 0; }
    // ALWAYS the socket's own head, never the TCB's -- even though it is
    // the TCB that raises the events.
    //
    // A poll registration is a `struct poll_reg` living on the POLLER'S
    // STACK, threaded into the head's list for as long as that thread is
    // inside poll(). The socket's lifetime is exactly the descriptor's,
    // which is what the poller holds. The TCB's is NOT: it outlives the
    // socket through TIME_WAIT and is then RECYCLED into the next
    // connection, carrying whatever list it had. Handing pollers a head
    // inside a recycled object is a use-after-free waiting for a busy
    // enough machine.
    //
    // The cost is that a stream socket's readiness change reaches
    // pollers through the global broadcast (see tcp_wake_pollers) rather
    // than through this head. That is the documented fallback, and it is
    // noise rather than a correctness gap.
    return &s->poll;
}

static int sock_poll(struct file_descriptor *f, int events) {
    struct socket *s = (struct socket *)f->priv;
    if (!s) { return POLLERR; }

    int mask = 0;
    if (s->type == SOCK_STREAM && s->tcb) {
        struct tcb *t = s->tcb;
        // A listener is READABLE when something is waiting to be
        // accepted: that is what makes poll() usable as an accept loop.
        if (t->state == TCP_LISTEN) {
            if (t->accept_n > 0) { mask |= POLLIN; }
        } else {
            // EOF counts as readable. A poll loop that woke only on
            // data would never notice the peer closing, and would hang
            // on a connection that is finished rather than broken.
            if (t->rcv_len || t->fin_rcvd || t->state == TCP_CLOSED) { mask |= POLLIN; }
            if (t->state == TCP_ESTABLISHED && t->snd_len < TCP_SNDBUF) { mask |= POLLOUT; }
            // A non-blocking connect reports its result through
            // POLLOUT, so a connection that FAILED must be writable
            // too, or the poller waits forever for a verdict it has.
            if (t->state == TCP_CLOSED) { mask |= POLLOUT | POLLHUP; }
            if (t->reset) { mask |= POLLERR | POLLHUP; }
            if (t->so_error) { mask |= POLLERR; }
        }
        return mask & (events | POLLERR | POLLHUP);
    }

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
    .poll_head = sock_poll_head,
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
