#include "net/tcp.h"
#include "net/net.h"
#include "net/route.h"
#include "sched/proc.h"
#include "drivers/char/serial.h"
#include "lib/rand.h"
#include "errno.h"

// ---------------------------------------------------------- sequence
//
// Sequence numbers wrap. Every comparison is therefore made on the
// SIGNED difference, which is correct across the wrap and wrong nowhere
// -- as long as the two numbers are within 2^31 of each other, which
// they are by construction because the window is 32 KiB.
static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) <  0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) >  0; }
static inline int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static struct tcb      conns[TCP_MAX_CONNS];
static struct spinlock table_lock;         // LOCK_RANK_SOCKTABLE
static uint64_t stat_tx, stat_rx, stat_retrans, stat_reasm, stat_rst_tx, stat_drop;
static uint32_t fault_drop_n, fault_reorder_n;
static uint64_t fault_counter;
static struct tcp_header last_tx;

int tcp_inuse(void) {
    int n = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) { if (conns[i].in_use) { n++; } }
    return n;
}

uint64_t tcp_rst_tx(void) { return stat_rst_tx; }
const struct tcp_header *tcp_last_tx(void) { return &last_tx; }

void tcp_stats(uint64_t *tx, uint64_t *rx, uint64_t *rt, uint64_t *re,
               uint64_t *rst, uint64_t *drop) {
    if (tx)   { *tx   = stat_tx; }
    if (rx)   { *rx   = stat_rx; }
    if (rt)   { *rt   = stat_retrans; }
    if (re)   { *re   = stat_reasm; }
    if (rst)  { *rst  = stat_rst_tx; }
    if (drop) { *drop = stat_drop; }
}

// One segment held back, so the NEXT one overtakes it. That is what
// reordering is, and it is enough to make a receiver see a gap and a
// sender see duplicate ACKs -- which is the whole point.
static struct { uint32_t src, dst, len; uint8_t data[1500]; int held; } fault_hold;

void tcp_fault_inject(uint32_t drop_1_in_n, uint32_t reorder_1_in_n) {
    fault_drop_n    = drop_1_in_n;
    fault_reorder_n = reorder_1_in_n;
    fault_counter   = 0;
    // A segment held when the injector is turned off would be stranded
    // forever, and the connection would stall on a gap nothing fills.
    if (fault_hold.held) {
        fault_hold.held = 0;
        net_ipv4_output(fault_hold.src, fault_hold.dst, IPPROTO_TCP,
                        fault_hold.data, fault_hold.len);
    }
}

// ------------------------------------------------------------- tracing

static const char *state_name(enum tcp_state s) {
    switch (s) {
    case TCP_CLOSED:       return "CLOSED";
    case TCP_LISTEN:       return "LISTEN";
    case TCP_SYN_SENT:     return "SYN_SENT";
    case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
    case TCP_ESTABLISHED:  return "ESTABLISHED";
    case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
    case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_CLOSING:      return "CLOSING";
    case TCP_LAST_ACK:     return "LAST_ACK";
    case TCP_TIME_WAIT:    return "TIME_WAIT";
    }
    return "?";
}

// Caller holds t->lock.
static void set_state(struct tcb *t, enum tcp_state s) {
    if (t->state == s) { return; }
    t->state = s;
    if (t->trace_n < 16) { t->trace[t->trace_n++] = (uint8_t)s; }
}

void tcp_dump(struct tcb *t, const char *why) {
    serial_write_string("[tcp] ");
    serial_write_string(why);
    serial_write_string(" state=");
    serial_write_string(state_name(t->state));
    serial_write_string(" trace=");
    for (int i = 0; i < t->trace_n; i++) {
        serial_write_string(state_name((enum tcp_state)t->trace[i]));
        serial_write_string(i + 1 < t->trace_n ? ">" : "");
    }
    serial_write_string(" snd_una="); serial_write_hex64(t->snd_una);
    serial_write_string(" snd_nxt="); serial_write_hex64(t->snd_nxt);
    serial_write_string(" rcv_nxt="); serial_write_hex64(t->rcv_nxt);
    serial_write_string(" cwnd=");    serial_write_hex64(t->cwnd);
    serial_write_string("\n");
}

// ----------------------------------------------------------- the table

void tcp_init(void) {
    spin_init(&table_lock, LOCK_RANK_SOCKTABLE, "tcptable");
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        spin_init(&conns[i].lock, LOCK_RANK_TCP, "tcb");
        waitq_init(&conns[i].waiters);
        poll_head_init(&conns[i].poll, "tcp-poll");
        conns[i].in_use = 0;
    }
}

struct tcb *tcp_alloc(void) {
    uint64_t f = spin_lock_irqsave(&table_lock);
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].in_use) { continue; }
        struct tcb *t = &conns[i];
        // Everything but the lock, the wait queue and the poll head,
        // which outlive a connection because a sleeper may still be
        // holding a reference to them.
        t->state = TCP_CLOSED;
        t->in_use = 1;
        t->local_n = t->remote_n = 0; t->lport_n = t->rport_n = 0;
        t->snd_una = t->snd_nxt = t->snd_wnd = t->snd_wl1 = t->snd_wl2 = t->iss = 0;
        t->rcv_nxt = t->irs = 0;
        t->rcv_wnd = TCP_RCVBUF;
        t->mss = TCP_DEFAULT_MSS;
        t->snd_head = t->snd_len = t->rcv_head = t->rcv_len = 0;
        for (int j = 0; j < TCP_REASM_SEGS; j++) { t->reasm[j].used = 0; }
        t->rto_deadline = t->delack_deadline = t->persist_deadline = 0;
        t->timewait_deadline = t->establish_deadline = 0;
        t->rto_ticks = TCP_RTO_MIN_TICKS * 5;    // 1s before any sample
        t->srtt_8 = t->rttvar_8 = 0;
        t->rtt_pending = 0; t->retries = 0; t->persist_shift = 0;
        t->cwnd = 0; t->ssthresh = TCP_SNDBUF; t->dupacks = 0;
        t->in_recovery = 0; t->recover = 0;
        t->backlog = 0; t->accept_n = 0; t->parent = 0;
        t->sock_gone = 0; t->reclaimed = 0;
        t->nodelay = 0; t->reuseaddr = 0; t->so_error = 0;
        t->fin_sent = t->fin_rcvd = t->reset = 0;
        t->shut_rd = t->shut_wr = 0;
        t->trace_n = 0;
        t->txq_n = 0;
        spin_unlock_irqrestore(&table_lock, f);
        return t;
    }
    spin_unlock_irqrestore(&table_lock, f);
    return 0;
}

// Idempotent, and it has to be: tcp_close and the timer's reclaim can
// both reach the same block at the same moment -- the closing thread
// sets sock_gone and then releases, while the timer, between those two
// steps, sees sock_gone with the machine already CLOSED and releases
// first. With a counter that raced to -1 and freed the slot twice, the
// second time quite possibly after it had been handed to somebody else.
//
// `reclaimed` is claimed under t->lock and the slot freed under
// table_lock, in that order and NOT nested: table_lock ranks below
// t->lock, so holding one to take the other is a descending acquire.
void tcp_release(struct tcb *t) {
    uint64_t f = spin_lock_irqsave(&t->lock);
    if (t->reclaimed) { spin_unlock_irqrestore(&t->lock, f); return; }
    t->reclaimed = 1;
    spin_unlock_irqrestore(&t->lock, f);

    uint64_t g = spin_lock_irqsave(&table_lock);
    t->state  = TCP_CLOSED;
    t->in_use = 0;
    spin_unlock_irqrestore(&table_lock, g);
}

// An exact 4-tuple match. A listener has remote 0/0 and is found by
// tcp_find_listener instead, so a segment for an established connection
// can never be delivered to the listener it was accepted from.
struct tcb *tcp_find(uint32_t local_n, uint16_t lport_n,
                     uint32_t remote_n, uint16_t rport_n) {
    uint64_t f = spin_lock_irqsave(&table_lock);
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcb *t = &conns[i];
        if (!t->in_use || t->state == TCP_LISTEN) { continue; }
        if (t->lport_n == lport_n && t->rport_n == rport_n &&
            t->remote_n == remote_n &&
            (t->local_n == local_n || t->local_n == 0)) {
            spin_unlock_irqrestore(&table_lock, f);
            return t;
        }
    }
    spin_unlock_irqrestore(&table_lock, f);
    return 0;
}

struct tcb *tcp_find_listener(uint32_t local_n, uint16_t lport_n) {
    uint64_t f = spin_lock_irqsave(&table_lock);
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcb *t = &conns[i];
        if (!t->in_use || t->state != TCP_LISTEN) { continue; }
        if (t->lport_n == lport_n &&
            (t->local_n == local_n || t->local_n == 0)) {
            spin_unlock_irqrestore(&table_lock, f);
            return t;
        }
    }
    spin_unlock_irqrestore(&table_lock, f);
    return 0;
}

// ------------------------------------------------------------- output

// Builds one segment into `buf` and returns its length. Pure: it
// touches no connection state and sends nothing.
static uint32_t build_segment(uint8_t *buf,
                       uint32_t src_n, uint16_t sport_n,
                       uint32_t dst_n, uint16_t dport_n,
                       uint32_t seq, uint32_t ack, uint8_t flags,
                       uint32_t window, uint32_t mss_opt,
                       const uint8_t *data, uint32_t len) {
    struct tcp_header *h = (struct tcp_header *)buf;
    uint32_t hdr = 20;

    h->sport_n  = sport_n;
    h->dport_n  = dport_n;
    h->seq_n    = htonl_k(seq);
    h->ack_n    = htonl_k(ack);
    h->flags    = flags;
    h->window_n = htons_k((uint16_t)(window > 65535 ? 65535 : window));
    h->urgent_n = 0;

    if (mss_opt) {
        // The MSS option, only ever on a SYN, because that is the only
        // segment on which it means anything.
        buf[20] = 2; buf[21] = 4;
        buf[22] = (uint8_t)(mss_opt >> 8); buf[23] = (uint8_t)(mss_opt & 0xFF);
        hdr = 24;
    }
    h->data_off = (uint8_t)((hdr / 4) << 4);

    for (uint32_t i = 0; i < len; i++) { buf[hdr + i] = data[i]; }
    uint32_t total = hdr + len;

    h->checksum_n = 0;
    h->checksum_n = net_l4_checksum(src_n, dst_n, IPPROTO_TCP, buf, total);
    last_tx = *h;
    return total;
}

// Sends one already-built segment. Called with NO connection lock held.
static void tx_now(uint32_t src_n, uint32_t dst_n,
                   const uint8_t *buf, uint32_t total) {
    stat_tx++;

    // The fault injector, applied HERE rather than in the driver, so it
    // behaves identically over loopback and over the wire -- and
    // loopback is where the deterministic suite runs.
    if (fault_drop_n || fault_reorder_n) {
        fault_counter++;
        if (fault_drop_n && fault_counter % fault_drop_n == 0) {
            stat_drop++;
            return;
        }
        if (fault_reorder_n && total <= sizeof fault_hold.data) {
            if (fault_hold.held) {
                // Release the held one AFTER this one: the later
                // segment arrives first, which is the reorder.
                net_ipv4_output(src_n, dst_n, IPPROTO_TCP, buf, total);
                net_ipv4_output(fault_hold.src, fault_hold.dst, IPPROTO_TCP,
                                fault_hold.data, fault_hold.len);
                fault_hold.held = 0;
                return;
            }
            if (fault_counter % fault_reorder_n == 0) {
                fault_hold.src = src_n;
                fault_hold.dst = dst_n;
                fault_hold.len = total;
                for (uint32_t i = 0; i < total; i++) { fault_hold.data[i] = buf[i]; }
                fault_hold.held = 1;
                return;
            }
        }
    }

    net_ipv4_output(src_n, dst_n, IPPROTO_TCP, buf, total);
}

// Stages a segment for transmission after the lock is released.
// Caller holds t->lock. See the staging queue's comment in tcp.h.
static void tx_queue(struct tcb *t, uint32_t seq, uint32_t ack, uint8_t flags,
                     uint32_t window, uint32_t mss_opt,
                     const uint8_t *data, uint32_t len) {
    if (t->txq_n >= (int)(sizeof t->txq / sizeof t->txq[0])) { return; }
    struct tcp_txseg *slot = &t->txq[t->txq_n];
    uint32_t total = build_segment(slot->data, t->local_n, t->lport_n,
                                   t->remote_n, t->rport_n,
                                   seq, ack, flags, window, mss_opt, data, len);
    slot->len = (uint16_t)total;
    t->txq_n++;
}

void tcp_tx_flush(struct tcb *t) {
    for (;;) {
        uint8_t  buf[1500];
        uint32_t len, src, dst;
        uint64_t f = spin_lock_irqsave(&t->lock);
        if (t->txq_n <= 0) { spin_unlock_irqrestore(&t->lock, f); return; }
        len = t->txq[0].len;
        for (uint32_t i = 0; i < len; i++) { buf[i] = t->txq[0].data[i]; }
        for (int i = 1; i < t->txq_n; i++) { t->txq[i - 1] = t->txq[i]; }
        t->txq_n--;
        src = t->local_n; dst = t->remote_n;
        spin_unlock_irqrestore(&t->lock, f);
        tx_now(src, dst, buf, len);
    }
}

// A RST for a segment that matches no TCB. There is no connection, so
// everything it needs comes out of the offending segment.
static void send_rst_raw(uint32_t src_n, uint32_t dst_n,
                         const struct tcp_header *in, uint32_t seg_len) {
    // NEVER answer a RST with a RST. Two hosts that both do produce an
    // exchange that never ends and is invisible without a capture.
    if (in->flags & TCP_RST) { return; }

    stat_rst_tx++;
    uint8_t buf[64];
    uint32_t total;
    if (in->flags & TCP_ACK) {
        // The peer told us where it is; take its ACK as our sequence
        // and send a bare RST.
        total = build_segment(buf, dst_n, in->dport_n, src_n, in->sport_n,
                              ntohl_k(in->ack_n), 0, TCP_RST, 0, 0, 0, 0);
    } else {
        // No ACK to borrow: sequence zero, and acknowledge everything
        // the segment occupied -- INCLUDING the SYN, which consumes one
        // sequence number of its own.
        uint32_t ack = ntohl_k(in->seq_n) + seg_len;
        if (in->flags & TCP_SYN) { ack++; }
        if (in->flags & TCP_FIN) { ack++; }
        total = build_segment(buf, dst_n, in->dport_n, src_n, in->sport_n,
                              0, ack, TCP_RST | TCP_ACK, 0, 0, 0, 0);
    }
    tx_now(dst_n, src_n, buf, total);
}

// Caller holds t->lock.
static uint32_t advertised_window(struct tcb *t) {
    uint32_t free_space = TCP_RCVBUF - t->rcv_len;
    // Silly window avoidance on the RECEIVE side: do not advertise an
    // opening smaller than one segment or half the buffer, or a peer
    // spends the connection sending one-byte segments into the gap.
    if (free_space < t->mss && free_space < TCP_RCVBUF / 2) { return 0; }
    return free_space;
}

// Caller holds t->lock.
static void send_flags(struct tcb *t, uint8_t flags, uint32_t seq) {
    tx_queue(t, seq, t->rcv_nxt, flags, advertised_window(t),
             (flags & TCP_SYN) ? TCP_MAX_MSS : 0, 0, 0);
}

// Caller holds t->lock. Everything Nagle, cwnd and the peer's window
// permit, from snd_nxt forward.
void tcp_output(struct tcb *t) {
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT &&
        t->state != TCP_FIN_WAIT_1  && t->state != TCP_LAST_ACK &&
        t->state != TCP_CLOSING) {
        return;
    }

    uint32_t window = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
    const int burst_max = (int)(sizeof t->txq / sizeof t->txq[0]) - 1;
    for (;;) {
        // Bounded burst: the staging queue is finite, and pacing a full
        // congestion window into eight segments per call is a feature.
        // The next ACK, or the timer, continues from here.
        if (t->txq_n >= burst_max) { break; }
        uint32_t in_flight = t->snd_nxt - t->snd_una;
        if (in_flight >= window) { break; }
        uint32_t unsent = t->snd_len - in_flight;
        if (!unsent) { break; }

        uint32_t can = window - in_flight;
        uint32_t n = unsent < can ? unsent : can;
        if (n > t->mss) { n = t->mss; }

        // NAGLE: no SMALL segment while anything is unacknowledged. The
        // exception is a segment that empties the buffer AND fills the
        // window, which is not small in the sense Nagle means.
        if (!t->nodelay && n < t->mss && in_flight > 0) { break; }

        uint8_t seg[TCP_MAX_MSS];
        uint32_t off = (t->snd_head + in_flight) % TCP_SNDBUF;
        for (uint32_t i = 0; i < n; i++) {
            seg[i] = t->sndbuf[(off + i) % TCP_SNDBUF];
        }

        uint8_t flags = TCP_ACK | TCP_PSH;
        tx_queue(t, t->snd_nxt, t->rcv_nxt, flags, advertised_window(t),
                 0, seg, n);

        // One RTT sample in flight at a time, and never from a
        // retransmission -- Karn's algorithm. An estimator that samples
        // retransmissions collapses exactly when the network is worst.
        if (!t->rtt_pending) {
            t->rtt_pending = 1;
            t->rtt_seq = t->snd_nxt;
            t->rtt_at  = timer_ticks();
        }
        t->snd_nxt += n;
        if (!t->rto_deadline) { t->rto_deadline = timer_ticks() + t->rto_ticks; }
        t->delack_deadline = 0;    // the data carried the ACK
    }

    // The FIN goes out once everything before it has been sent.
    if (t->fin_sent == 1 && t->snd_nxt == t->snd_una + t->snd_len) {
        send_flags(t, TCP_FIN | TCP_ACK, t->snd_nxt);
        t->snd_nxt++;
        t->fin_sent = 2;
        if (!t->rto_deadline) { t->rto_deadline = timer_ticks() + t->rto_ticks; }
    }
}

// ------------------------------------------------------- Reno, in full

static void cc_init(struct tcb *t) {
    t->cwnd     = 2 * t->mss;
    t->ssthresh = TCP_SNDBUF;
    t->dupacks  = 0;
    t->in_recovery = 0;
}

static void cc_on_ack(struct tcb *t, uint32_t acked) {
    if (t->in_recovery) {
        // Fast recovery ends when the ACK reaches the point where loss
        // was detected. Deflate to ssthresh rather than to the inflated
        // value, which would otherwise burst.
        t->cwnd = t->ssthresh;
        t->in_recovery = 0;
        t->dupacks = 0;
        return;
    }
    if (t->cwnd < t->ssthresh) {
        t->cwnd += acked < t->mss ? acked : t->mss;      // slow start
    } else {
        // Congestion avoidance: about one MSS per RTT, not per ACK.
        uint32_t inc = t->mss * t->mss / (t->cwnd ? t->cwnd : 1);
        t->cwnd += inc ? inc : 1;
    }
    if (t->cwnd > TCP_SNDBUF) { t->cwnd = TCP_SNDBUF; }
}

static void cc_on_timeout(struct tcb *t) {
    uint32_t flight = t->snd_nxt - t->snd_una;
    uint32_t half = flight / 2;
    t->ssthresh = half > 2 * t->mss ? half : 2 * t->mss;
    t->cwnd = t->mss;
    t->in_recovery = 0;
    t->dupacks = 0;
}

// Caller holds t->lock. Returns nonzero when the caller should
// retransmit immediately.
static int cc_on_dupack(struct tcb *t) {
    t->dupacks++;
    if (t->in_recovery) {
        t->cwnd += t->mss;             // inflate, one per further dup
        return 0;
    }
    // The THIRD duplicate, not the second (which is ordinary
    // reordering) and not the fourth (which is a segment too late).
    if (t->dupacks == 3) {
        uint32_t flight = t->snd_nxt - t->snd_una;
        uint32_t half = flight / 2;
        t->ssthresh = half > 2 * t->mss ? half : 2 * t->mss;
        t->cwnd = t->ssthresh + 3 * t->mss;
        t->in_recovery = 1;
        t->recover = t->snd_nxt;
        return 1;
    }
    return 0;
}

// Caller holds t->lock. Resends the first unacknowledged segment.
static void retransmit(struct tcb *t) {
    uint32_t outstanding = t->snd_nxt - t->snd_una;
    if (t->fin_sent == 2 && outstanding == t->snd_len + 1) { outstanding--; }
    if (!outstanding) {
        if (t->fin_sent == 2) {
            send_flags(t, TCP_FIN | TCP_ACK, t->snd_nxt - 1);
            stat_retrans++;
        }
        return;
    }
    uint32_t n = outstanding < t->mss ? outstanding : t->mss;
    uint8_t seg[TCP_MAX_MSS];
    for (uint32_t i = 0; i < n; i++) {
        seg[i] = t->sndbuf[(t->snd_head + i) % TCP_SNDBUF];
    }
    stat_retrans++;
    // Karn: this segment contributes no RTT sample, and any sample
    // already in flight is abandoned because its ACK is now ambiguous.
    t->rtt_pending = 0;
    tx_queue(t, t->snd_una, t->rcv_nxt, TCP_ACK | TCP_PSH,
             advertised_window(t), 0, seg, n);
}

// ---------------------------------------------------------- RTT / RTO

// Jacobson/Karels, in eighths of a tick.
static void rtt_sample(struct tcb *t, uint64_t measured) {
    uint32_t m8 = (uint32_t)(measured * 8);
    if (!t->srtt_8) {
        t->srtt_8   = m8;
        t->rttvar_8 = m8 / 2;
    } else {
        int32_t err = (int32_t)m8 - (int32_t)t->srtt_8;
        int32_t abs_err = err < 0 ? -err : err;
        t->rttvar_8 = (uint32_t)((int32_t)t->rttvar_8 + (abs_err - (int32_t)t->rttvar_8) / 4);
        t->srtt_8   = (uint32_t)((int32_t)t->srtt_8 + err / 8);
    }
    uint32_t rto = (t->srtt_8 + 4 * t->rttvar_8) / 8;
    if (rto < TCP_RTO_MIN_TICKS) { rto = TCP_RTO_MIN_TICKS; }
    if (rto > TCP_RTO_MAX_TICKS) { rto = TCP_RTO_MAX_TICKS; }
    t->rto_ticks = rto;
}

// ------------------------------------------------------------- receive

// Caller holds t->lock. Copies in-order bytes into the receive ring.
static uint32_t deliver(struct tcb *t, const uint8_t *data, uint32_t len) {
    uint32_t space = TCP_RCVBUF - t->rcv_len;
    if (len > space) { len = space; }
    for (uint32_t i = 0; i < len; i++) {
        t->rcvbuf[(t->rcv_head + t->rcv_len + i) % TCP_RCVBUF] = data[i];
    }
    t->rcv_len += len;
    t->rcv_nxt += len;
    return len;
}

// Caller holds t->lock. After rcv_nxt moves, any queued segment that
// now starts exactly there is delivered, repeatedly.
static void drain_reasm(struct tcb *t) {
    int moved = 1;
    while (moved) {
        moved = 0;
        for (int i = 0; i < TCP_REASM_SEGS; i++) {
            struct tcp_reasm *r = &t->reasm[i];
            if (!r->used) { continue; }
            if (seq_le(r->seq, t->rcv_nxt) &&
                seq_gt(r->seq + r->len, t->rcv_nxt)) {
                uint32_t skip = t->rcv_nxt - r->seq;
                deliver(t, r->data + skip, r->len - skip);
                r->used = 0;
                moved = 1;
            } else if (seq_le(r->seq + r->len, t->rcv_nxt)) {
                r->used = 0;                 // entirely old
            }
        }
    }
}

// Caller holds t->lock.
static void queue_reasm(struct tcb *t, uint32_t seq, const uint8_t *data, uint32_t len) {
    if (len > TCP_REASM_MAX) { len = TCP_REASM_MAX; }
    for (int i = 0; i < TCP_REASM_SEGS; i++) {
        if (t->reasm[i].used && t->reasm[i].seq == seq) { return; }  // duplicate
    }
    for (int i = 0; i < TCP_REASM_SEGS; i++) {
        if (t->reasm[i].used) { continue; }
        t->reasm[i].seq = seq;
        t->reasm[i].len = len;
        t->reasm[i].used = 1;
        for (uint32_t j = 0; j < len; j++) { t->reasm[i].data[j] = data[j]; }
        stat_reasm++;
        return;
    }
    // A ninth out-of-order segment is DROPPED. It costs a
    // retransmission and it cannot corrupt the stream, which is the
    // trade a fixed table is here to make.
    stat_drop++;
}

// Caller holds t->lock.
static void wake(struct tcb *t) {
    waitq_wake_all(&t->waiters);
    poll_head_notify(&t->poll);
}

// Caller holds t->lock. Everything that ends a connection abruptly.
static void enter_closed(struct tcb *t, int err) {
    set_state(t, TCP_CLOSED);
    t->so_error = err;
    t->rto_deadline = t->delack_deadline = t->persist_deadline = 0;
    t->establish_deadline = 0;
    wake(t);
}

// ------------------------------------------------------- segment input

static uint32_t parse_mss(const uint8_t *seg, uint32_t hdr_len) {
    uint32_t i = 20;
    while (i + 1 < hdr_len) {
        uint8_t kind = seg[i];
        if (kind == 0) { break; }            // end of options
        if (kind == 1) { i++; continue; }    // no-op
        uint8_t olen = seg[i + 1];
        if (olen < 2 || i + olen > hdr_len) { break; }
        if (kind == 2 && olen == 4) {
            return ((uint32_t)seg[i + 2] << 8) | seg[i + 3];
        }
        i += olen;
    }
    return 0;
}

// Caller holds t->lock. Handles a segment for a connection that exists.
static void tcp_segment(struct tcb *t, const struct ipv4_header *ip,
                        const struct tcp_header *h, uint32_t hdr_len,
                        const uint8_t *seg, uint32_t seg_total) {
    (void)ip;
    uint32_t data_len = seg_total - hdr_len;
    const uint8_t *data = seg + hdr_len;
    uint32_t seq = ntohl_k(h->seq_n);
    uint32_t ack = ntohl_k(h->ack_n);
    uint8_t  fl  = h->flags;

    if (fl & TCP_RST) {
        // A RST tears the connection down. WHICH error it becomes
        // depends on whether the connection was ever established: a RST
        // answering our SYN means the port is closed, and Linux reports
        // that as ECONNREFUSED. Reporting ECONNRESET there tells a
        // program its connection was broken when in fact it never had
        // one, and every retry loop written against Linux gets it wrong.
        t->reset = 1;
        int err = (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RECEIVED)
                ? ECONNREFUSED : ECONNRESET;
        enter_closed(t, err);
        return;
    }

    if (t->state == TCP_SYN_SENT) {
        if ((fl & TCP_SYN) && (fl & TCP_ACK)) {
            if (!seq_ge(ack, t->iss + 1) || seq_gt(ack, t->snd_nxt)) { return; }
            t->irs     = seq;
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->snd_wnd = ntohs_k(h->window_n);
            uint32_t m = parse_mss(seg, hdr_len);
            if (m) { t->mss = m > TCP_MAX_MSS ? TCP_MAX_MSS : m; }
            cc_init(t);
            set_state(t, TCP_ESTABLISHED);
            t->establish_deadline = 0;
            t->rto_deadline = 0;
            if (t->rtt_pending) { rtt_sample(t, timer_ticks() - t->rtt_at); t->rtt_pending = 0; }
            send_flags(t, TCP_ACK, t->snd_nxt);
            wake(t);
            return;
        }
        if (fl & TCP_SYN) {
            // SIMULTANEOUS OPEN. Three lines, and omitting them turns a
            // legal handshake into a hang.
            t->irs     = seq;
            t->rcv_nxt = seq + 1;
            set_state(t, TCP_SYN_RECEIVED);
            send_flags(t, TCP_SYN | TCP_ACK, t->iss);
            return;
        }
        return;
    }

    // RFC 793 3.9's acceptability test, in the form that matters here:
    // a segment is acceptable if it overlaps the receive window at all.
    if (t->state != TCP_LISTEN) {
        uint32_t win = t->rcv_wnd ? t->rcv_wnd : 1;
        int acceptable;
        if (data_len == 0) {
            acceptable = seq_ge(seq, t->rcv_nxt) && seq_lt(seq, t->rcv_nxt + win);
            if (t->rcv_wnd == 0) { acceptable = (seq == t->rcv_nxt); }
        } else {
            acceptable = (seq_ge(seq, t->rcv_nxt) && seq_lt(seq, t->rcv_nxt + win)) ||
                         (seq_lt(seq, t->rcv_nxt) &&
                          seq_gt(seq + data_len, t->rcv_nxt));
        }
        if (!acceptable) {
            // Not a drop in silence: an unacceptable segment gets an ACK
            // so the peer can resynchronise, which is what breaks the
            // deadlock when one side's idea of the window is stale.
            send_flags(t, TCP_ACK, t->snd_nxt);
            return;
        }
    }

    if (!(fl & TCP_ACK)) { return; }

    if (t->state == TCP_SYN_RECEIVED) {
        if (seq_ge(ack, t->snd_una) && seq_le(ack, t->snd_nxt)) {
            t->snd_una = ack;
            t->snd_wnd = ntohs_k(h->window_n);
            cc_init(t);
            set_state(t, TCP_ESTABLISHED);
            t->establish_deadline = 0;
            t->rto_deadline = 0;
            // A passively opened connection joins its listener's
            // backlog only now: half-open connections must not be
            // accept()able, or a program reads from a connection that
            // does not exist yet.
            if (t->parent) {
                struct tcb *p = t->parent;
                if (p->accept_n < TCP_BACKLOG_MAX) {
                    p->accept_q[p->accept_n++] = t;
                }
                waitq_wake_all(&p->waiters);
                poll_head_notify(&p->poll);
            }
            wake(t);
        } else {
            // Staged, not sent: send_rst_raw transmits immediately and
            // this path runs under t->lock.
            tx_queue(t, ack, 0, TCP_RST, 0, 0, 0, 0);
            return;
        }
    }

    // ---- the ACK, for every state that has one
    if (seq_gt(ack, t->snd_una) && seq_le(ack, t->snd_nxt)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data_acked = acked;
        if (t->fin_sent == 2 && seq_ge(ack, t->snd_una + t->snd_len + 1)) {
            data_acked = t->snd_len;       // the FIN's byte is not data
        }
        t->snd_una   = ack;
        t->snd_head  = (t->snd_head + data_acked) % TCP_SNDBUF;
        t->snd_len  -= data_acked < t->snd_len ? data_acked : t->snd_len;
        t->retries   = 0;
        t->dupacks   = 0;
        t->persist_shift = 0;

        if (t->rtt_pending && seq_ge(ack, t->rtt_seq)) {
            rtt_sample(t, timer_ticks() - t->rtt_at);
            t->rtt_pending = 0;
        }
        cc_on_ack(t, acked);
        t->rto_deadline = (t->snd_una != t->snd_nxt)
                        ? timer_ticks() + t->rto_ticks : 0;
        wake(t);
    } else if (ack == t->snd_una && data_len == 0 && t->snd_una != t->snd_nxt) {
        if (cc_on_dupack(t)) { retransmit(t); }
    }

    // ---- the window update
    if (seq_lt(t->snd_wl1, seq) ||
        (t->snd_wl1 == seq && seq_le(t->snd_wl2, ack))) {
        uint32_t was = t->snd_wnd;
        t->snd_wnd = ntohs_k(h->window_n);
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        if (!t->snd_wnd && was) {
            t->persist_deadline = timer_ticks() + t->rto_ticks;
        } else if (t->snd_wnd) {
            t->persist_deadline = 0;
        }
    }

    // ---- the data
    if (data_len) {
        if (seq == t->rcv_nxt) {
            deliver(t, data, data_len);
            drain_reasm(t);
            // Delayed ACK: 200ms, OR immediately on every second
            // full-sized segment. Arming it is what makes a bulk
            // transfer cost one ACK per two segments instead of one per
            // segment.
            if (!t->delack_deadline) {
                t->delack_deadline = timer_ticks() + TCP_DELACK_TICKS;
            } else {
                t->delack_deadline = 0;
                send_flags(t, TCP_ACK, t->snd_nxt);
            }
            wake(t);
        } else if (seq_gt(seq, t->rcv_nxt)) {
            queue_reasm(t, seq, data, data_len);
            // An out-of-order segment is acknowledged IMMEDIATELY, and
            // that is not an optimisation: the duplicate ACKs it
            // generates are how the peer's fast retransmit fires.
            send_flags(t, TCP_ACK, t->snd_nxt);
        } else {
            send_flags(t, TCP_ACK, t->snd_nxt);   // entirely old
        }
    }

    // ---- the FIN
    if ((fl & TCP_FIN) && seq_le(seq, t->rcv_nxt) &&
        seq_ge(seq + data_len, t->rcv_nxt)) {
        t->fin_rcvd = 1;
        t->rcv_nxt++;
        send_flags(t, TCP_ACK, t->snd_nxt);
        switch (t->state) {
        case TCP_ESTABLISHED: set_state(t, TCP_CLOSE_WAIT); break;
        case TCP_FIN_WAIT_1:
            // SIMULTANEOUS CLOSE.
            set_state(t, TCP_CLOSING);
            break;
        case TCP_FIN_WAIT_2:
            set_state(t, TCP_TIME_WAIT);
            t->timewait_deadline = timer_ticks() + TCP_TIMEWAIT_TICKS;
            break;
        default: break;
        }
        wake(t);
    }

    // ---- the transitions the ACK alone drives
    if (t->state == TCP_FIN_WAIT_1 && t->fin_sent == 2 &&
        seq_ge(t->snd_una, t->snd_nxt)) {
        set_state(t, t->fin_rcvd ? TCP_TIME_WAIT : TCP_FIN_WAIT_2);
        if (t->state == TCP_TIME_WAIT) {
            t->timewait_deadline = timer_ticks() + TCP_TIMEWAIT_TICKS;
        }
    } else if (t->state == TCP_CLOSING && seq_ge(t->snd_una, t->snd_nxt)) {
        set_state(t, TCP_TIME_WAIT);
        t->timewait_deadline = timer_ticks() + TCP_TIMEWAIT_TICKS;
    } else if (t->state == TCP_LAST_ACK && seq_ge(t->snd_una, t->snd_nxt)) {
        enter_closed(t, 0);
        return;
    }

    tcp_output(t);
}

void tcp_input(struct netdev *dev, const struct ipv4_header *ip,
               const uint8_t *seg, uint32_t len) {
    if (len < sizeof(struct tcp_header)) { dev->rx_dropped++; return; }
    const struct tcp_header *h = (const struct tcp_header *)seg;
    uint32_t hdr_len = (uint32_t)(h->data_off >> 4) * 4;
    if (hdr_len < 20 || hdr_len > len) { dev->rx_dropped++; return; }

    // A segment with a bad checksum is dropped SILENTLY: no RST, no
    // ACK. Answering it would be answering something nobody can be
    // shown to have sent, and a RST is a connection-killing answer.
    if (!net_l4_verify(ip->src_n, ip->dst_n, IPPROTO_TCP, seg, len)) {
        dev->rx_dropped++;
        stat_drop++;
        return;
    }
    stat_rx++;

    struct tcb *t = tcp_find(ip->dst_n, h->dport_n, ip->src_n, h->sport_n);
    if (t) {
        uint64_t f = spin_lock_irqsave(&t->lock);
        tcp_segment(t, ip, h, hdr_len, seg, len);
        spin_unlock_irqrestore(&t->lock, f);
        tcp_tx_flush(t);
        return;
    }

    // A SYN for a listening port opens a new connection.
    if ((h->flags & TCP_SYN) && !(h->flags & TCP_ACK)) {
        struct tcb *l = tcp_find_listener(ip->dst_n, h->dport_n);
        if (l) {
            struct tcb *c = tcp_alloc();
            if (!c) {
                // The table is full. REFUSE, loudly and immediately --
                // a fixed table that refuses is the design, and a
                // silent drop here looks like a lost packet forever.
                send_rst_raw(ip->src_n, ip->dst_n, h, len - hdr_len);
                return;
            }
            uint64_t f = spin_lock_irqsave(&c->lock);
            c->local_n  = ip->dst_n;
            c->remote_n = ip->src_n;
            c->lport_n  = h->dport_n;
            c->rport_n  = h->sport_n;
            c->irs      = ntohl_k(h->seq_n);
            c->rcv_nxt  = c->irs + 1;
            c->iss      = (uint32_t)rand_u64();
            c->snd_una  = c->iss;
            c->snd_nxt  = c->iss;
            c->snd_wnd  = ntohs_k(h->window_n);
            c->parent   = l;
            uint32_t m = parse_mss(seg, hdr_len);
            if (m) { c->mss = m > TCP_MAX_MSS ? TCP_MAX_MSS : m; }
            set_state(c, TCP_SYN_RECEIVED);
            c->establish_deadline = timer_ticks() + TCP_ESTABLISH_TICKS;
            send_flags(c, TCP_SYN | TCP_ACK, c->iss);
            c->snd_nxt++;
            c->rto_deadline = timer_ticks() + c->rto_ticks;
            spin_unlock_irqrestore(&c->lock, f);
            tcp_tx_flush(c);
            return;
        }
    }

    send_rst_raw(ip->src_n, ip->dst_n, h, len - hdr_len);
}

// -------------------------------------------------------------- timers

void tcp_timer_tick(void) {
    uint64_t now = timer_ticks();
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcb *t = &conns[i];
        if (!t->in_use) { continue; }
        uint64_t f = spin_lock_irqsave(&t->lock);

        if (t->timewait_deadline && now >= t->timewait_deadline) {
            t->timewait_deadline = 0;
            enter_closed(t, 0);
            spin_unlock_irqrestore(&t->lock, f);
            tcp_tx_flush(t);
            continue;
        }

        // THE RECLAIM RULE, and it is the only place a finished
        // connection's slot comes back.
        //
        // It used to be TIME_WAIT expiry that dropped the reference,
        // unconditionally -- which freed the block out from under any
        // socket still open on it (a program that shut down its write
        // side and kept reading), handing the slot to the next
        // connection. And it missed the passive close entirely:
        // CLOSE_WAIT -> LAST_ACK -> CLOSED never passes through
        // TIME_WAIT, so every connection this machine did not initiate
        // the close of leaked a slot until the table was empty and
        // everything was refused.
        //
        // Both are the same question asked properly: the socket is
        // gone, the state machine has finished, so nothing can reach
        // this block again.
        if (t->sock_gone && t->state == TCP_CLOSED && !t->reclaimed) {
            spin_unlock_irqrestore(&t->lock, f);
            tcp_release(t);      // idempotent; the closer may beat us here
            continue;
        }

        if (t->establish_deadline && now >= t->establish_deadline) {
            t->establish_deadline = 0;
            tcp_dump(t, "establishment timed out");
            enter_closed(t, ETIMEDOUT);
            spin_unlock_irqrestore(&t->lock, f);
            tcp_tx_flush(t);
            continue;
        }

        if (t->rto_deadline && now >= t->rto_deadline) {
            if (++t->retries > TCP_MAX_RETRIES) {
                tcp_dump(t, "retransmission gave up");
                enter_closed(t, ETIMEDOUT);
                spin_unlock_irqrestore(&t->lock, f);
                tcp_tx_flush(t);
                continue;
            }
            cc_on_timeout(t);
            // Exponential backoff, clamped. A stack that retries at a
            // fixed interval against a dead peer is a stack that
            // generates traffic proportional to how broken the network
            // already is.
            t->rto_ticks *= 2;
            if (t->rto_ticks > TCP_RTO_MAX_TICKS) { t->rto_ticks = TCP_RTO_MAX_TICKS; }
            t->rto_deadline = now + t->rto_ticks;
            if (t->state == TCP_SYN_SENT) {
                send_flags(t, TCP_SYN, t->iss);
                stat_retrans++;
            } else if (t->state == TCP_SYN_RECEIVED) {
                send_flags(t, TCP_SYN | TCP_ACK, t->iss);
                stat_retrans++;
            } else {
                retransmit(t);
            }
        }

        if (t->delack_deadline && now >= t->delack_deadline) {
            t->delack_deadline = 0;
            send_flags(t, TCP_ACK, t->snd_nxt);
        }

        if (t->persist_deadline && now >= t->persist_deadline) {
            // The zero-window probe: ONE byte, deliberately outside the
            // peer's advertised window, so the peer must answer with a
            // window update. Without it, a lost window update deadlocks
            // a connection that is otherwise perfectly healthy.
            t->persist_shift++;
            uint64_t backoff = t->rto_ticks << (t->persist_shift > 6 ? 6 : t->persist_shift);
            if (backoff > TCP_RTO_MAX_TICKS) { backoff = TCP_RTO_MAX_TICKS; }
            t->persist_deadline = now + backoff;
            if (t->snd_len > (t->snd_nxt - t->snd_una)) {
                uint8_t b = t->sndbuf[(t->snd_head + (t->snd_nxt - t->snd_una)) % TCP_SNDBUF];
                tx_queue(t, t->snd_nxt, t->rcv_nxt, TCP_ACK,
                         advertised_window(t), 0, &b, 1);
            } else {
                send_flags(t, TCP_ACK, t->snd_nxt - 1);
            }
        }

        spin_unlock_irqrestore(&t->lock, f);
        tcp_tx_flush(t);
    }
}

static struct waitq timer_wait;

static void tcp_timer_thread(void) {
    for (;;) {
        tcp_timer_tick();
        // One pass per tick, and a THREAD rather than a tick callback
        // because firing a timer TRANSMITS: transmitting takes the ARP
        // lock and may spin on the device, and neither belongs in a
        // timer interrupt. Nothing ever wakes this queue; the timeout
        // is the whole mechanism.
        waitq_sleep_timeout(&timer_wait, 0, timer_ticks() + 1);
    }
}

void tcp_timer_start(void) {
    waitq_init(&timer_wait);
    thread_alloc_kernel(tcp_timer_thread);
}

// ------------------------------------------------- the socket-facing ops
//
// Each takes t->lock itself. tcp_sock.c must not hold the SOCKET lock
// across any of them -- see LOCK_RANK_TCP.

int tcp_connect(struct tcb *t, uint32_t dst_n, uint16_t dport_n) {
    uint32_t next_hop;
    struct netdev *dev = route_lookup(dst_n, &next_hop);
    if (!dev) { return -ENETUNREACH; }

    uint64_t f = spin_lock_irqsave(&t->lock);
    if (t->state != TCP_CLOSED) {
        spin_unlock_irqrestore(&t->lock, f);
        return -EISCONN;
    }
    t->local_n  = dev->ip_n;
    t->remote_n = dst_n;
    t->rport_n  = dport_n;
    // A RANDOM initial sequence number, not a counter. A predictable
    // ISN lets anybody who can guess it inject into the connection, and
    // it is the kind of thing that is embarrassing to have written down.
    t->iss     = (uint32_t)rand_u64();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss;
    t->snd_wnd = TCP_SNDBUF;
    cc_init(t);
    set_state(t, TCP_SYN_SENT);
    t->establish_deadline = timer_ticks() + TCP_ESTABLISH_TICKS;
    t->rtt_pending = 1; t->rtt_seq = t->iss; t->rtt_at = timer_ticks();
    send_flags(t, TCP_SYN, t->iss);
    t->snd_nxt++;
    t->rto_deadline = timer_ticks() + t->rto_ticks;
    spin_unlock_irqrestore(&t->lock, f);
    tcp_tx_flush(t);
    return 0;
}

int tcp_listen(struct tcb *t, int backlog) {
    uint64_t f = spin_lock_irqsave(&t->lock);
    if (t->state != TCP_CLOSED && t->state != TCP_LISTEN) {
        spin_unlock_irqrestore(&t->lock, f);
        return -EINVAL;
    }
    if (backlog < 1) { backlog = 1; }
    if (backlog > TCP_BACKLOG_MAX) { backlog = TCP_BACKLOG_MAX; }
    t->backlog = backlog;
    set_state(t, TCP_LISTEN);
    spin_unlock_irqrestore(&t->lock, f);
    return 0;
}

int tcp_send(struct tcb *t, const uint8_t *data, uint32_t len, uint32_t *sent) {
    uint64_t f = spin_lock_irqsave(&t->lock);
    if (t->reset)   { spin_unlock_irqrestore(&t->lock, f); return -ECONNRESET; }
    if (t->shut_wr) { spin_unlock_irqrestore(&t->lock, f); return -EPIPE; }
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) {
        spin_unlock_irqrestore(&t->lock, f);
        return t->state == TCP_CLOSED ? -ENOTCONN : -ENOTCONN;
    }
    uint32_t space = TCP_SNDBUF - t->snd_len;
    uint32_t n = len < space ? len : space;
    for (uint32_t i = 0; i < n; i++) {
        t->sndbuf[(t->snd_head + t->snd_len + i) % TCP_SNDBUF] = data[i];
    }
    t->snd_len += n;
    tcp_output(t);
    spin_unlock_irqrestore(&t->lock, f);
    tcp_tx_flush(t);
    *sent = n;
    return 0;
}

int tcp_recv(struct tcb *t, uint8_t *out, uint32_t len, uint32_t *got) {
    uint64_t f = spin_lock_irqsave(&t->lock);
    if (t->rcv_len) {
        uint32_t n = len < t->rcv_len ? len : t->rcv_len;
        for (uint32_t i = 0; i < n; i++) {
            out[i] = t->rcvbuf[(t->rcv_head + i) % TCP_RCVBUF];
        }
        t->rcv_head = (t->rcv_head + n) % TCP_RCVBUF;
        t->rcv_len -= n;
        // The window just opened. Tell the peer, or a connection that
        // filled the buffer stays stalled until a timer happens to fire.
        if (advertised_window(t)) { send_flags(t, TCP_ACK, t->snd_nxt); }
        spin_unlock_irqrestore(&t->lock, f);
        tcp_tx_flush(t);
        *got = n;
        return 0;
    }
    if (t->reset)   { spin_unlock_irqrestore(&t->lock, f); return -ECONNRESET; }
    // EOF, not an error: the peer closed and there is nothing left.
    if (t->fin_rcvd || t->shut_rd || t->state == TCP_CLOSED) {
        spin_unlock_irqrestore(&t->lock, f);
        *got = 0;
        return 0;
    }
    spin_unlock_irqrestore(&t->lock, f);
    return -EAGAIN;
}

void tcp_shutdown_write(struct tcb *t) {
    uint64_t f = spin_lock_irqsave(&t->lock);
    if (t->shut_wr || t->fin_sent) { spin_unlock_irqrestore(&t->lock, f); return; }
    t->shut_wr  = 1;
    t->fin_sent = 1;
    switch (t->state) {
    case TCP_ESTABLISHED: set_state(t, TCP_FIN_WAIT_1); break;
    case TCP_CLOSE_WAIT:  set_state(t, TCP_LAST_ACK);   break;
    default: break;
    }
    tcp_output(t);
    spin_unlock_irqrestore(&t->lock, f);
    tcp_tx_flush(t);
}

void tcp_close(struct tcb *t) {
    // Everything the LISTENER was holding for somebody who never called
    // accept(). Each of those connections was allocated by tcp_input
    // and has nothing else pointing at it; closing the listener without
    // releasing them leaks a table slot apiece, permanently.
    struct tcb *orphans[TCP_BACKLOG_MAX];
    int n_orphans = 0;

    uint64_t f = spin_lock_irqsave(&t->lock);
    enum tcp_state st = t->state;
    t->sock_gone = 1;
    if (st == TCP_LISTEN) {
        for (int i = 0; i < t->accept_n; i++) { orphans[n_orphans++] = t->accept_q[i]; }
        t->accept_n = 0;
    }
    spin_unlock_irqrestore(&t->lock, f);

    for (int i = 0; i < n_orphans; i++) {
        struct tcb *c = orphans[i];
        uint64_t g = spin_lock_irqsave(&c->lock);
        c->sock_gone = 1;
        // RST rather than a polite close: nobody ever accepted this, so
        // there is no application state to flush, and the peer should
        // learn immediately rather than wait out a FIN exchange with a
        // connection that was never really there.
        send_flags(c, TCP_RST, c->snd_nxt);
        set_state(c, TCP_CLOSED);
        spin_unlock_irqrestore(&c->lock, g);
        tcp_tx_flush(c);
        tcp_release(c);
    }

    if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) {
        tcp_shutdown_write(t);
        // The reference is NOT dropped here. The TCB has to live on
        // through FIN_WAIT_1, FIN_WAIT_2, LAST_ACK and TIME_WAIT with no
        // file descriptor attached -- see THE LIFETIME RULE in tcp.h.
        // The timer reclaims it once the state machine reaches CLOSED.
        return;
    }
    if (st == TCP_LISTEN || st == TCP_CLOSED || st == TCP_SYN_SENT) {
        uint64_t g = spin_lock_irqsave(&t->lock);
        if (st == TCP_SYN_SENT) { send_flags(t, TCP_RST, t->snd_nxt); }
        set_state(t, TCP_CLOSED);
        spin_unlock_irqrestore(&t->lock, g);
        tcp_tx_flush(t);
        tcp_release(t);
    }
}

// ------------------------------------------------------------ selftest

static int tcp_fail(const char *msg) {
    serial_write_string("[tcp] FAILED: ");
    serial_write_string(msg);
    serial_write_string("\n");
    return 1;
}

// The RST arms use loopback: nothing there is expected to reply, and a
// RST is never answered with a RST, so the echo terminates.
#define T_LOCAL  IP_LOOPBACK_N
#define T_REMOTE IP_LOOPBACK_N

// The TRANSITION arms cannot. Loopback delivers our own replies back
// into tcp_input, where they match no connection -- our reply's ports
// are the mirror of the connection's -- so the stack RSTs itself and
// every transition is undone microseconds after it happens. The traces
// showed each state being reached and then CLOSED.
//
// So the staged peer is an address that ROUTES OUT: 10.0.2.99, which
// nothing answers. The replies leave (or queue behind an ARP request
// that never completes, which is just as good) and never come back.
#define T_PEER 0x6302000Au      // 10.0.2.99

// Builds one segment and pushes it through the real input path,
// including the real checksum. `damage` flips a checksum bit, which is
// how the corrupt-segment case is made without a second code path.
static void inject_from(uint32_t src_n, uint32_t dst_n,
                        uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                        uint8_t flags, const uint8_t *data, uint32_t len, int damage) {
    uint8_t seg[20 + 64];
    struct tcp_header *h = (struct tcp_header *)seg;
    h->sport_n  = htons_k(sport);
    h->dport_n  = htons_k(dport);
    h->seq_n    = htonl_k(seq);
    h->ack_n    = htonl_k(ack);
    h->data_off = 5 << 4;
    h->flags    = flags;
    h->window_n = htons_k(8192);
    h->urgent_n = 0;
    for (uint32_t i = 0; i < len; i++) { seg[20 + i] = data[i]; }
    uint32_t total = 20 + len;
    h->checksum_n = 0;
    h->checksum_n = net_l4_checksum(src_n, dst_n, IPPROTO_TCP, seg, total);
    if (damage) { h->checksum_n ^= htons_k(0x0100); }

    struct ipv4_header ip;
    ip.version_ihl = IPV4_VERSION_IHL_20;
    ip.tos = 0; ip.total_length_n = htons_k(20 + total);
    ip.id_n = 0; ip.flags_frag_n = IPV4_FLAG_DF_N;
    ip.ttl = IPV4_DEFAULT_TTL; ip.protocol = IPPROTO_TCP;
    ip.checksum_n = 0;
    ip.src_n = src_n; ip.dst_n = dst_n;
    tcp_input(net_loopback(), &ip, seg, total);
}

static void inject(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                   uint8_t flags, const uint8_t *data, uint32_t len, int damage) {
    inject_from(T_REMOTE, T_LOCAL, sport, dport, seq, ack, flags, data, len, damage);
}

// Puts a TCB directly into `st` with a known sequence space, so one
// transition can be tested without driving the eleven before it.
static struct tcb *stage(enum tcp_state st, uint16_t lport, uint16_t rport,
                         uint32_t local_n, uint32_t peer_n) {
    struct tcb *t = tcp_alloc();
    if (!t) { return 0; }
    t->local_n = local_n; t->remote_n = peer_n;
    t->lport_n = htons_k(lport); t->rport_n = htons_k(rport);
    t->iss = 1000; t->snd_una = 1000; t->snd_nxt = 1000; t->snd_wnd = 8192;
    t->irs = 2000; t->rcv_nxt = 2001; t->rcv_wnd = TCP_RCVBUF;
    t->mss = TCP_DEFAULT_MSS;
    cc_init(t);
    t->state = st;
    // In SYN_SENT the SYN is ALREADY OUT, so snd_nxt is one past iss.
    // Staging it at iss makes the peer's SYN|ACK unacceptable and the
    // transition silently never happens -- which is what it did.
    if (st == TCP_SYN_SENT) { t->snd_nxt = t->iss + 1; }
    t->trace_n = 0;
    return t;
}

struct transition {
    const char *name;
    enum tcp_state from;
    uint8_t  flags;
    int      fin_sent;      // 1 when the state implies our FIN is out
    // Whether the injected segment's ACK covers our FIN. It is a
    // separate axis from the flags, and conflating the two is what made
    // the CLOSING row unreachable: a FIN that ALSO acknowledges our FIN
    // is a simultaneous close COMPLETING, and TIME_WAIT is then the
    // correct answer. To reach CLOSING the peer must acknowledge our
    // data and not our FIN.
    int      acks_fin;
    enum tcp_state to;
};

void tcp_selftest(void) {
    serial_write_string("[tcp] selftest\n");
    int failed = 0;

    // ---- 1. A SYN to a port nothing listens on is REFUSED.
    uint64_t before = tcp_rst_tx();
    inject(40000, 9999, 0x11223344u, 0, TCP_SYN, 0, 0, 0);
    if (tcp_rst_tx() != before + 1) {
        failed |= tcp_fail("a SYN to a closed port was silent");
    } else {
        const struct tcp_header *r = tcp_last_tx();
        if (!(r->flags & TCP_RST) || !(r->flags & TCP_ACK)) {
            failed |= tcp_fail("a RST answering a SYN must also carry ACK");
        }
        // The SYN occupies one sequence number, so the RST must
        // acknowledge seq+1. Getting this wrong makes the peer ignore
        // the refusal and retry until it times out.
        if (ntohl_k(r->ack_n) != 0x11223345u) {
            failed |= tcp_fail("RST ack does not consume the SYN");
        }
    }

    // ---- 2. A RST is NEVER answered with a RST.
    before = tcp_rst_tx();
    inject(40001, 9999, 0x55667788u, 1, TCP_RST | TCP_ACK, 0, 0, 0);
    if (tcp_rst_tx() != before) {
        failed |= tcp_fail("answered a RST with a RST");
    }

    // ---- 3. A corrupt segment is dropped silently -- no RST.
    before = tcp_rst_tx();
    inject(40002, 9999, 0x99AABBCCu, 0, TCP_SYN, 0, 0, 1);
    if (tcp_rst_tx() != before) {
        failed |= tcp_fail("RST'd a segment with a bad checksum");
    }

    // ---- 4. Every transition the RFC names, including the two
    //         simultaneous cases -- three lines each in a machine that
    //         already exists, and a hang apiece when omitted.
    static const struct transition table[] = {
        { "SYN_SENT + SYN|ACK -> ESTABLISHED",  TCP_SYN_SENT,     TCP_SYN|TCP_ACK, 0, 1, TCP_ESTABLISHED  },
        { "SYN_SENT + SYN -> SYN_RECEIVED",     TCP_SYN_SENT,     TCP_SYN,         0, 1, TCP_SYN_RECEIVED },
        { "SYN_RECEIVED + ACK -> ESTABLISHED",  TCP_SYN_RECEIVED, TCP_ACK,         0, 1, TCP_ESTABLISHED  },
        { "ESTABLISHED + FIN -> CLOSE_WAIT",    TCP_ESTABLISHED,  TCP_FIN|TCP_ACK, 0, 1, TCP_CLOSE_WAIT   },
        { "FIN_WAIT_1 + ACK -> FIN_WAIT_2",     TCP_FIN_WAIT_1,   TCP_ACK,         1, 1, TCP_FIN_WAIT_2   },
        { "FIN_WAIT_1 + FIN -> CLOSING",        TCP_FIN_WAIT_1,   TCP_FIN|TCP_ACK, 1, 0, TCP_CLOSING      },
        { "FIN_WAIT_2 + FIN -> TIME_WAIT",      TCP_FIN_WAIT_2,   TCP_FIN|TCP_ACK, 1, 1, TCP_TIME_WAIT    },
        { "CLOSING + ACK -> TIME_WAIT",         TCP_CLOSING,      TCP_ACK,         1, 1, TCP_TIME_WAIT    },
        { "LAST_ACK + ACK -> CLOSED",           TCP_LAST_ACK,     TCP_ACK,         1, 1, TCP_CLOSED       },
    };

    struct netdev *nic = net_device();
    if (!nic || !nic->ip_n) {
        serial_write_string("[tcp] SKIPPED: no addressed NIC for the transition arm\n");
    } else
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++) {
        const struct transition *tr = &table[i];
        uint16_t lport = (uint16_t)(20000 + i);
        uint16_t rport = (uint16_t)(30000 + i);
        struct tcb *t = stage(tr->from, lport, rport, nic->ip_n, T_PEER);
        if (!t) { failed |= tcp_fail("out of TCBs staging a transition"); break; }
        if (tr->fin_sent) {
            // Our FIN is already out and unacknowledged: snd_nxt is one
            // past the data, and the ACK the peer sends covers it.
            t->fin_sent = 2;
            t->snd_nxt  = t->snd_una + 1;
        }
        if (tr->from == TCP_FIN_WAIT_2 || tr->from == TCP_CLOSING ||
            tr->from == TCP_LAST_ACK) {
            t->fin_rcvd = (tr->from != TCP_FIN_WAIT_2);
        }
        // In FIN_WAIT_2 our FIN is already acknowledged -- that is what
        // got us out of FIN_WAIT_1 -- so the send space is empty.
        if (tr->from == TCP_FIN_WAIT_2) { t->snd_una = t->snd_nxt; }

        uint32_t ack = tr->acks_fin ? t->snd_nxt : t->snd_una;
        inject_from(T_PEER, nic->ip_n, rport, lport,
                    t->rcv_nxt, ack, tr->flags, 0, 0, 0);

        if (t->state != tr->to) {
            serial_write_string("[tcp] FAILED: ");
            serial_write_string(tr->name);
            serial_write_string(" -- got ");
            serial_write_string(state_name(t->state));
            serial_write_string("\n");
            tcp_dump(t, "transition failed");
            failed = 1;
        }
        t->state = TCP_CLOSED;
        tcp_release(t);
    }

    // ---- 5. The table is finite, and a full table REFUSES.
    struct tcb *held[TCP_MAX_CONNS];
    int n = 0;
    while (n < TCP_MAX_CONNS && (held[n] = tcp_alloc()) != 0) { n++; }
    if (n != TCP_MAX_CONNS) {
        failed |= tcp_fail("could not fill the connection table");
    }
    if (tcp_alloc() != 0) {
        failed |= tcp_fail("allocated past the end of the connection table");
    }
    for (int i = 0; i < n; i++) { held[i]->state = TCP_CLOSED; tcp_release(held[i]); }

    serial_write_string(failed ? "[tcp] FAILED\n" : "[tcp] ALL PASSED\n");
}
