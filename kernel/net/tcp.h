#ifndef NEOOS_TCP_H
#define NEOOS_TCP_H

#include <stdint.h>
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sync/poll_head.h"
#include "drivers/char/timer.h"

// TCP: the state machine, the windows, and the timers.
//
// SPLIT FROM tcp_sock.c ON PURPOSE. This file answers "what does this
// segment mean"; that one answers "what does accept() return". They
// fail differently, they are read at different times, and a single file
// containing both is the file nobody can hold in their head.
//
// THE LIFETIME RULE, because it is the one thing here that surprises
// everybody: a TCB's lifetime IS NOT its socket's. close() on an
// established connection sends FIN and leaves the TCB behind to finish
// closing -- through FIN_WAIT_1, FIN_WAIT_2 and TIME_WAIT -- long after
// the file descriptor is gone. That is what `refs` is for: the socket
// holds one, the state machine holds one until CLOSED, and the block is
// reclaimed when both are released.

struct netdev;
struct ipv4_header;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

struct tcp_header {
    uint16_t sport_n, dport_n;
    uint32_t seq_n, ack_n;
    uint8_t  data_off;          // high nibble: header length in 32-bit words
    uint8_t  flags;
    uint16_t window_n;
    uint16_t checksum_n;
    uint16_t urgent_n;
} __attribute__((packed));
_Static_assert(sizeof(struct tcp_header) == 20, "TCP header must be 20 bytes");

// SIXTEEN connections with 32 KiB each way, and the numbers are chosen
// against the machine rather than against the RFC. The table is STATIC
// -- nothing allocates on the receive path, so a SYN flood exhausts a
// fixed table and refuses instead of exhausting the heap and panicking
// -- which means its size is a permanent charge against a 128 MiB
// machine. Sixty-four connections at 64 KiB each way is nearly 9 MiB of
// .bss; this is 1.2 MiB. A 32 KiB window still needs no window scaling,
// which is the property the size was actually chosen for.
#define TCP_MAX_CONNS   16
#define TCP_SNDBUF      32768
#define TCP_RCVBUF      32768
#define TCP_REASM_SEGS  8
#define TCP_REASM_MAX   1460
#define TCP_BACKLOG_MAX 8
#define TCP_DEFAULT_MSS 536
#define TCP_MAX_MSS     1460

// MSL is FIVE SECONDS, so TIME_WAIT is ten. Linux uses 60, giving a
// two-minute TIME_WAIT that cannot be observed inside a 150-second boot
// which also runs forty other suites. Recorded as a divergence in
// docs/stdlib.md.
#define TCP_MSL_TICKS       (5 * TIMER_HZ)
#define TCP_TIMEWAIT_TICKS  (2 * TCP_MSL_TICKS)

#define TCP_RTO_MIN_TICKS   (TIMER_HZ / 5)      // 200ms
#define TCP_RTO_MAX_TICKS   (60 * TIMER_HZ)
#define TCP_DELACK_TICKS    (TIMER_HZ / 5)      // 200ms
#define TCP_MAX_RETRIES     12
#define TCP_ESTABLISH_TICKS (30 * TIMER_HZ)

enum tcp_state {
    TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED,
    TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT,
    TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};

struct tcp_reasm {
    uint32_t seq, len;
    int      used;
    uint8_t  data[TCP_REASM_MAX];
};

struct tcb {
    struct spinlock  lock;          // LOCK_RANK_TCP
    struct waitq     waiters;
    struct poll_head poll;

    enum tcp_state state;
    int      in_use;
    int      refs;                  // see THE LIFETIME RULE above

    uint32_t local_n, remote_n;
    uint16_t lport_n, rport_n;

    // Send sequence space, RFC 793's names because every line of the
    // state machine is easier to check against the RFC with them.
    uint32_t snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2, iss;
    // Receive sequence space.
    uint32_t rcv_nxt, rcv_wnd, irs;
    uint32_t mss;

    // Rings, not queues: nothing allocates on the receive path, so a
    // SYN flood exhausts a fixed table and refuses rather than
    // exhausting the heap and panicking.
    uint8_t  sndbuf[TCP_SNDBUF];
    uint32_t snd_head, snd_len;     // head = index of snd_una's byte
    uint8_t  rcvbuf[TCP_RCVBUF];
    uint32_t rcv_head, rcv_len;

    struct tcp_reasm reasm[TCP_REASM_SEGS];

    uint64_t rto_deadline, delack_deadline, persist_deadline,
             timewait_deadline, establish_deadline;
    uint32_t rto_ticks;
    // SRTT and RTTVAR in EIGHTHS of a tick. A 10ms clock quantises a
    // plain-tick estimator to zero on a link that answers in
    // microseconds, which slirp does.
    uint32_t srtt_8, rttvar_8;
    uint32_t rtt_seq; uint64_t rtt_at; int rtt_pending;
    uint8_t  retries;
    uint8_t  persist_shift;

    uint32_t cwnd, ssthresh, dupacks, recover;
    int      in_recovery;

    int      backlog;
    struct tcb *accept_q[TCP_BACKLOG_MAX];
    int      accept_n;
    struct tcb *parent;

    int      nodelay, reuseaddr, so_error;
    int      fin_sent, fin_rcvd, reset;
    int      shut_rd, shut_wr;

    // THE STAGING QUEUE, and it is the most important structural fact
    // in this file after the lifetime rule.
    //
    // A segment cannot be transmitted while t->lock is held. Loopback
    // delivers SYNCHRONOUSLY, in the sender's context: net_ipv4_output
    // runs straight into net_ipv4_input, which locks the DESTINATION
    // connection. Over 127.0.0.1 both ends of a connection live in this
    // same table, so a send under A's lock takes B's -- and the other
    // end doing the same thing at the same moment is a deadlock, not
    // merely a rank complaint. socket.c has the same note about UDP.
    //
    // So: build under the lock, where the sequence numbers are stable,
    // and flush after releasing it. Eight segments per burst, which
    // also paces a full congestion window into something bounded.
    struct tcp_txseg { uint16_t len; uint8_t data[1500]; } txq[8];
    int      txq_n;

    // The state ring. TCP's failure mode is a HANG, not a fault: a
    // wrong transition does not panic, it waits, and the gauntlet
    // reports a missing marker 150 seconds later with nothing in the
    // log. This is dumped on the establishment timeout and by tcp_dump.
    uint8_t  trace[16];
    int      trace_n;
};

void tcp_init(void);
void tcp_input(struct netdev *dev, const struct ipv4_header *ip,
               const uint8_t *seg, uint32_t len);
// The timer thread's body, one pass over every active TCB.
void tcp_timer_tick(void);
void tcp_timer_start(void);

// Allocation and lifetime. Both halves of TCP use these; nothing else
// should.
struct tcb *tcp_alloc(void);
void        tcp_ref(struct tcb *t);
void        tcp_unref(struct tcb *t);
struct tcb *tcp_find(uint32_t local_n, uint16_t lport_n,
                     uint32_t remote_n, uint16_t rport_n);
struct tcb *tcp_find_listener(uint32_t local_n, uint16_t lport_n);

// The operations tcp_sock.c drives the machine with. Each takes the
// TCB's lock itself; none may be called with it held.
int  tcp_connect(struct tcb *t, uint32_t dst_n, uint16_t dport_n);
int  tcp_listen(struct tcb *t, int backlog);
int  tcp_send(struct tcb *t, const uint8_t *data, uint32_t len, uint32_t *sent);
int  tcp_recv(struct tcb *t, uint8_t *out, uint32_t len, uint32_t *got);
void tcp_close(struct tcb *t);
void tcp_shutdown_write(struct tcb *t);
// Pushes whatever the window and Nagle allow. Called after a send and
// from the timer.
void tcp_output(struct tcb *t);

void tcp_dump(struct tcb *t, const char *why);
// Sends everything staged under the lock. MUST be called with t->lock
// NOT held, by whoever released it.
void tcp_tx_flush(struct tcb *t);

// The fault injector: drop 1 in N transmitted segments, reorder 1 in M.
// Zero disables. Slirp never drops and never reorders, so without this
// the recovery paths never execute -- and a stack whose recovery paths
// have never run is a stack whose recovery paths do not work.
void tcp_fault_inject(uint32_t drop_1_in_n, uint32_t reorder_1_in_n);
void tcp_stats(uint64_t *segs_tx, uint64_t *segs_rx, uint64_t *retrans,
               uint64_t *reasm, uint64_t *rst_tx, uint64_t *dropped);

uint64_t tcp_rst_tx(void);
const struct tcp_header *tcp_last_tx(void);

void tcp_selftest(void);

#endif
