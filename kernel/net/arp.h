#ifndef NEOOS_ARP_H
#define NEOOS_ARP_H

#include <stdint.h>
#include "net/eth.h"
#include "drivers/char/timer.h"

// Address resolution: turning "10.0.2.2" into "52:55:0a:00:02:02".
//
// D1 proved the wire by hand-building one ARP request inside the driver
// selftest and reading the reply back out of the receive queue. That
// proved the DEVICE. This proves the LAYER: a cache with expiry, a
// request that retries, and a packet that waits for the answer instead
// of being dropped.

#define ARP_CACHE_MAX     16
#define ARP_VALID_TICKS   (60 * TIMER_HZ)   // see the divergence note below
#define ARP_RETRY_TICKS   (1 * TIMER_HZ)
#define ARP_MAX_REQUESTS  3

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

// Ethernet-over-IPv4 only. The hardware/protocol type fields exist so
// that ARP can carry other pairs; nothing here does, and a packet
// claiming a different pair is dropped rather than reinterpreted.
struct arp_packet {
    uint16_t htype_n, ptype_n;
    uint8_t  hlen, plen;
    uint16_t op_n;
    uint8_t  sha[6];
    uint32_t spa_n;          // NOT 4-byte aligned in this struct -- see arp.c
    uint8_t  tha[6];
    uint32_t tpa_n;
} __attribute__((packed));
_Static_assert(sizeof(struct arp_packet) == 28, "ARP packet must be 28 bytes");

struct netdev;

void arp_init(void);

// Called by eth_input, on the netrx thread.
void arp_input(struct netdev *dev, const uint8_t *pkt, uint32_t len);

// Fills `mac` and returns 0 when the answer is already known.
// Returns -EAGAIN having QUEUED a copy of (pkt, len): a request has
// gone out and the packet will be sent when the reply lands.
// Returns -EHOSTUNREACH when there is no cache entry to be had.
int arp_resolve(struct netdev *dev, uint32_t ip_n, uint8_t mac[6],
                const uint8_t *pkt, uint32_t len);

// Retries pending requests and expires valid entries. Called once per
// tick from the netrx thread, which is the only context that is both
// periodic and allowed to transmit.
void arp_tick(void);

// Nonzero while at least one request is outstanding. netrx uses it to
// decide whether to sleep on a timeout or until the next frame.
int arp_pending(void);

void arp_stats(uint64_t *requests_tx, uint64_t *replies_tx,
               uint64_t *learned, uint64_t *dropped);

// Test hooks. A test that needs to reach inside says so in the header
// rather than extern-ing a static from another file.
int      arp_lookup_only(uint32_t ip_n, uint8_t mac[6]);   // 0 or -ENOENT
uint64_t arp_replies_sent(void);
void     arp_force_expire(uint32_t ip_n);

void arp_selftest(void);

#endif
