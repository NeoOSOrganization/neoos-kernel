#ifndef NEOOS_ICMP_H
#define NEOOS_ICMP_H

#include <stdint.h>

// ICMP, in three parts and no more.
//
// ECHO REPLY is the first externally visible result this kernel has:
// the host types `ping 10.0.2.15` and NeoOS answers.
//
// ECHO REQUEST exists because the automated test cannot depend on a
// human running ping. The selftest pings 10.0.2.2, which slirp answers
// itself, and asserts on the reply -- and the same code gives userland
// a ping later without new kernel work.
//
// PORT UNREACHABLE is generated for a UDP datagram that hits no socket.
// Linux does it, and without it a wrong port number is SILENT -- which
// is simultaneously the most common thing to get wrong when bringing up
// a network stack and the least diagnosable.
//
// Not here: timestamp, address mask, redirect, router discovery, and
// any ICMP socket type. The echo path is kernel-internal; userland
// reaches it through a raw socket that this milestone does not add.

#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO_REQUEST 8

#define ICMP_UNREACH_NET   0
#define ICMP_UNREACH_HOST  1
#define ICMP_UNREACH_PROTO 2
#define ICMP_UNREACH_PORT  3

struct icmp_header {
    uint8_t  type, code;
    uint16_t checksum_n;
    uint16_t id_n, seq_n;    // echo only; four "unused" bytes otherwise
} __attribute__((packed));
_Static_assert(sizeof(struct icmp_header) == 8, "ICMP header must be 8 bytes");

struct netdev;
struct ipv4_header;

void icmp_input(struct netdev *dev, const struct ipv4_header *ip,
                const uint8_t *msg, uint32_t len);

// Sends one echo request. Returns 0 or a negative errno.
int icmp_echo_send(uint32_t dst_n, uint16_t id, uint16_t seq);

// The tick at which a reply matching (id, seq) arrived, or 0 for "not
// yet". A caller polls it; four entries are remembered, which is three
// more than the one outstanding probe anything here has.
uint64_t icmp_echo_reply_at(uint16_t id, uint16_t seq);

// Quotes the IP header and the first 8 bytes of its payload, which is
// what the RFC requires and what lets the receiver work out which
// socket the failure belongs to.
void icmp_port_unreachable(struct netdev *dev, const struct ipv4_header *ip,
                           const uint8_t *payload, uint32_t payload_len);

void icmp_stats(uint64_t *echo_rx, uint64_t *echo_tx, uint64_t *replies_tx,
                uint64_t *unreach_tx, uint64_t *rate_limited);

// Test hooks, declared rather than extern-ed from another file.
uint64_t       icmp_replies_tx(void);
uint64_t       icmp_unreach_tx(void);
uint64_t       icmp_rate_limited(void);
const uint8_t *icmp_last_reply(void);     // the last reply message, header first
const uint8_t *icmp_last_unreach(void);   // the last unreachable, header first

void icmp_selftest(void);

#endif
