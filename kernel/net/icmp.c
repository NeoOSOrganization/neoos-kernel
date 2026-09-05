#include "net/icmp.h"
#include "net/net.h"
#include "net/netrx.h"
#include "drivers/char/serial.h"
#include "drivers/char/timer.h"
#include "smp/smp.h"
#include "errno.h"

// The largest echo this will answer. A reply longer than the link's MTU
// would have to be fragmented, and nothing here fragments -- so an
// oversized request is REFUSED rather than answered with a truncated
// copy of itself, which would be a lie about what was received.
#define ICMP_MAX_PAYLOAD 1472

// One generated unreachable per 100 ms, globally. Not a security
// measure -- there is no adversary here -- but a misconfigured peer
// hammering a closed port would otherwise fill the wire with our
// replies, and a stack that answers an error storm with an error storm
// is the one making it worse.
#define ICMP_UNREACH_MIN_GAP (TIMER_HZ / 10)

static uint64_t stat_echo_rx, stat_echo_tx, stat_replies_tx,
                stat_unreach_tx, stat_rate_limited;
static uint64_t last_unreach_tick;

static uint8_t  last_reply[8 + ICMP_MAX_PAYLOAD];
static uint8_t  last_unreach[8 + 20 + 8];

struct echo_record { uint16_t id, seq; uint64_t at; };
static struct echo_record echoes[4];
static int echo_next;

void icmp_stats(uint64_t *echo_rx, uint64_t *echo_tx, uint64_t *replies,
                uint64_t *unreach, uint64_t *limited) {
    if (echo_rx) { *echo_rx = stat_echo_rx; }
    if (echo_tx) { *echo_tx = stat_echo_tx; }
    if (replies) { *replies = stat_replies_tx; }
    if (unreach) { *unreach = stat_unreach_tx; }
    if (limited) { *limited = stat_rate_limited; }
}

uint64_t       icmp_replies_tx(void)   { return stat_replies_tx; }
uint64_t       icmp_unreach_tx(void)   { return stat_unreach_tx; }
uint64_t       icmp_rate_limited(void) { return stat_rate_limited; }
const uint8_t *icmp_last_reply(void)   { return last_reply; }
const uint8_t *icmp_last_unreach(void) { return last_unreach; }

// The checksum covers the WHOLE message, header and payload, with the
// checksum field zero. There is no pseudo-header: unlike UDP and TCP,
// ICMP does not checksum the addresses.
static uint16_t icmp_checksum(const void *msg, uint32_t len) {
    return htons_k(net_checksum(msg, len, 0));
}

static void echo_record(uint16_t id, uint16_t seq) {
    echoes[echo_next].id  = id;
    echoes[echo_next].seq = seq;
    echoes[echo_next].at  = timer_ticks();
    echo_next = (echo_next + 1) % 4;
}

uint64_t icmp_echo_reply_at(uint16_t id, uint16_t seq) {
    for (int i = 0; i < 4; i++) {
        if (echoes[i].at && echoes[i].id == id && echoes[i].seq == seq) {
            return echoes[i].at;
        }
    }
    return 0;
}

int icmp_echo_send(uint32_t dst_n, uint16_t id, uint16_t seq) {
    uint8_t msg[8 + 16];
    struct icmp_header *h = (struct icmp_header *)msg;
    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->checksum_n = 0;
    h->id_n  = htons_k(id);
    h->seq_n = htons_k(seq);
    for (int i = 0; i < 16; i++) { msg[8 + i] = (uint8_t)(0xA0 + i); }
    h->checksum_n = icmp_checksum(msg, sizeof msg);

    stat_echo_tx++;
    // src_n = 0: let net_ipv4_output pick the outgoing interface's
    // address, which is the whole point of D2's source selection.
    return net_ipv4_output(0, dst_n, IPPROTO_ICMP, msg, sizeof msg);
}

void icmp_port_unreachable(struct netdev *dev, const struct ipv4_header *ip,
                           const uint8_t *payload, uint32_t payload_len) {
    (void)dev;
    // NEVER for a datagram sent to a broadcast or multicast address.
    // Answering those is how a host becomes an amplifier: one broadcast
    // packet, one reply from every machine on the segment.
    if (ip->dst_n == IP_BROADCAST_N) { return; }
    if ((ntohl_k(ip->dst_n) >> 28) == 0xE) { return; }   // 224.0.0.0/4

    uint64_t now = timer_ticks();
    if (last_unreach_tick && now - last_unreach_tick < ICMP_UNREACH_MIN_GAP) {
        stat_rate_limited++;
        return;
    }
    last_unreach_tick = now;

    uint8_t msg[8 + 20 + 8];
    struct icmp_header *h = (struct icmp_header *)msg;
    h->type = ICMP_DEST_UNREACH;
    h->code = ICMP_UNREACH_PORT;
    h->checksum_n = 0;
    // The four bytes where id/seq live are "unused" for this type and
    // must be zero; a receiver is entitled to check.
    h->id_n = 0;
    h->seq_n = 0;

    const uint8_t *iph = (const uint8_t *)ip;
    for (int i = 0; i < 20; i++) { msg[8 + i] = iph[i]; }
    // The first EIGHT bytes of the payload, not zero and not more: for
    // UDP that is the whole header, so the receiver can see both port
    // numbers and tell which socket failed.
    for (int i = 0; i < 8; i++) {
        msg[8 + 20 + i] = (payload_len > (uint32_t)i) ? payload[i] : 0;
    }
    h->checksum_n = icmp_checksum(msg, sizeof msg);

    for (unsigned i = 0; i < sizeof msg; i++) { last_unreach[i] = msg[i]; }
    stat_unreach_tx++;
    net_ipv4_output(ip->dst_n, ip->src_n, IPPROTO_ICMP, msg, sizeof msg);
}

void icmp_input(struct netdev *dev, const struct ipv4_header *ip,
                const uint8_t *msg, uint32_t len) {
    if (len < sizeof(struct icmp_header)) { dev->rx_dropped++; return; }
    // A message whose checksum is wrong is dropped SILENTLY. Answering
    // it -- with a reply or with an error -- would be answering
    // something nobody can be shown to have sent.
    // net_checksum returns the COMPLEMENTED sum, so a message that
    // already carries its own correct checksum sums to zero -- the same
    // convention net_ipv4_input uses on the IP header.
    if (net_checksum(msg, len, 0) != 0) { dev->rx_dropped++; return; }

    const struct icmp_header *h = (const struct icmp_header *)msg;

    if (h->type == ICMP_ECHO_REPLY) {
        echo_record(ntohs_k(h->id_n), ntohs_k(h->seq_n));
        return;
    }
    if (h->type != ICMP_ECHO_REQUEST || h->code != 0) { return; }

    // NEVER answer an echo sent to a broadcast or multicast address.
    // One packet in, one reply out of every machine on the segment, is
    // the oldest amplification there is -- and the same rule the
    // unreachable path already follows, for the same reason. Linux
    // makes this a sysctl and defaults it off.
    if (ip->dst_n == IP_BROADCAST_N) { return; }
    if ((ntohl_k(ip->dst_n) >> 28) == 0xE) { return; }   // 224.0.0.0/4

    stat_echo_rx++;
    uint32_t payload_len = len - (uint32_t)sizeof(struct icmp_header);
    if (payload_len > ICMP_MAX_PAYLOAD) { return; }

    // COPY the message and change the type. Building a reply from
    // scratch passes every check a small test makes and then breaks
    // `ping -s 1000` on the first real use, because the payload a
    // sender put in is the payload it expects to get back.
    uint8_t reply[8 + ICMP_MAX_PAYLOAD];
    for (uint32_t i = 0; i < len; i++) { reply[i] = msg[i]; }
    struct icmp_header *r = (struct icmp_header *)reply;
    r->type = ICMP_ECHO_REPLY;
    r->checksum_n = 0;
    r->checksum_n = icmp_checksum(reply, len);

    for (uint32_t i = 0; i < len; i++) { last_reply[i] = reply[i]; }
    stat_replies_tx++;
    // Source is the address it was sent TO, not the interface's: a ping
    // to an address this host holds must be answered from that address.
    net_ipv4_output(ip->dst_n, ip->src_n, IPPROTO_ICMP, reply, len);
}

// ------------------------------------------------------------- selftest

static int icmp_fail(const char *msg) {
    serial_write_string("[icmp] FAILED: ");
    serial_write_string(msg);
    serial_write_string("\n");
    return 1;
}

void icmp_selftest(void) {
    serial_write_string("[icmp] selftest\n");
    int failed = 0;

    // ---- 1. An injected echo request produces a byte-exact reply.
    uint8_t buf[20 + 8 + 16];
    struct ipv4_header *ip = (struct ipv4_header *)buf;
    struct icmp_header *ic = (struct icmp_header *)(buf + 20);
    struct netdev *lo = net_loopback();

    ip->version_ihl    = IPV4_VERSION_IHL_20;
    ip->tos            = 0;
    ip->total_length_n = htons_k(sizeof buf);
    ip->id_n           = 0;
    ip->flags_frag_n   = IPV4_FLAG_DF_N;
    ip->ttl            = IPV4_DEFAULT_TTL;
    ip->protocol       = IPPROTO_ICMP;
    ip->checksum_n     = 0;
    ip->src_n          = IP_LOOPBACK_N;
    ip->dst_n          = IP_LOOPBACK_N;

    ic->type = ICMP_ECHO_REQUEST;
    ic->code = 0;
    ic->id_n  = htons_k(0xBEEF);
    ic->seq_n = htons_k(7);
    for (int i = 0; i < 16; i++) { buf[28 + i] = (uint8_t)(0xA0 + i); }
    ic->checksum_n = 0;
    ic->checksum_n = htons_k(net_checksum(ic, 8 + 16, 0));

    uint64_t before = icmp_replies_tx();
    icmp_input(lo, ip, (const uint8_t *)ic, 8 + 16);
    if (icmp_replies_tx() != before + 1) {
        failed |= icmp_fail("no reply to an echo request");
    } else {
        const uint8_t *r = icmp_last_reply();
        const struct icmp_header *rh = (const struct icmp_header *)r;
        if (r[0] != ICMP_ECHO_REPLY) {
            failed |= icmp_fail("reply is not type 0");
        }
        if (rh->id_n != htons_k(0xBEEF) || rh->seq_n != htons_k(7)) {
            failed |= icmp_fail("reply did not echo id/seq");
        }
        for (int i = 0; i < 16; i++) {
            if (r[8 + i] != (uint8_t)(0xA0 + i)) {
                failed |= icmp_fail("reply did not echo the payload");
                break;
            }
        }
        if (net_checksum(r, 8 + 16, 0) != 0) {
            failed |= icmp_fail("reply checksum is wrong");
        }
    }

    // ---- 2. A corrupt request is dropped, not answered.
    ic->checksum_n ^= htons_k(0x0100);
    before = icmp_replies_tx();
    icmp_input(lo, ip, (const uint8_t *)ic, 8 + 16);
    if (icmp_replies_tx() != before) {
        failed |= icmp_fail("replied to a corrupt echo request");
    }
    ic->checksum_n ^= htons_k(0x0100);

    // ---- 3. A REAL ping to the gateway, through the real driver.
    //         slirp answers pings addressed to itself, so this needs no
    //         cooperation from the host and no privileges.
    if (!net_device()) {
        serial_write_string("[icmp] SKIPPED: no NIC for the wire ping\n");
    } else if (smp_cpu_count() < 2) {
        serial_write_string("[icmp] SKIPPED: one CPU, nothing drains netrx\n");
    } else {
        uint64_t t0 = timer_ticks();
        uint64_t w = netrx_boot_window_open();
        int rc = icmp_echo_send(0x0202000Au /* 10.0.2.2 */, 0x1234, 1);
        uint64_t at = 0;
        for (int i = 0; rc == 0 && i < 400 && !at; i++) {
            netrx_boot_park();
            at = icmp_echo_reply_at(0x1234, 1);
        }
        netrx_boot_window_close(w);
        if (rc != 0) {
            failed |= icmp_fail("could not send an echo request");
        } else if (!at) {
            failed |= icmp_fail("no echo reply from the gateway");
        } else {
            serial_write_string("[icmp] gateway rtt ticks=");
            serial_write_hex64(at - t0);
            serial_write_string("\n");
        }
    }

    // ---- 4. A UDP datagram to a closed port is answered.
    uint64_t u0 = icmp_unreach_tx();
    last_unreach_tick = 0;          // the limiter must not eat the first one
    net_udp_output(IP_LOOPBACK_N, htons_k(4444),
                   IP_LOOPBACK_N, htons_k(9), (const uint8_t *)"x", 1);
    if (icmp_unreach_tx() != u0 + 1) {
        failed |= icmp_fail("a closed UDP port was silent");
    } else {
        const uint8_t *q = icmp_last_unreach();
        if (q[0] != ICMP_DEST_UNREACH || q[1] != ICMP_UNREACH_PORT) {
            failed |= icmp_fail("unreachable has the wrong type/code");
        }
        if (q[8 + 9] != IPPROTO_UDP) {
            failed |= icmp_fail("unreachable did not quote the IP header");
        }
        // Bytes 2..3 of the quoted UDP header are the DESTINATION port.
        uint16_t quoted_dport;
        ((uint8_t *)&quoted_dport)[0] = q[8 + 20 + 2];
        ((uint8_t *)&quoted_dport)[1] = q[8 + 20 + 3];
        if (quoted_dport != htons_k(9)) {
            failed |= icmp_fail("unreachable did not quote the UDP ports");
        }
    }

    // ---- 5. The rate limiter limits, and counts what it suppressed.
    u0 = icmp_unreach_tx();
    uint64_t l0 = icmp_rate_limited();
    for (int i = 0; i < 10; i++) {
        net_udp_output(IP_LOOPBACK_N, htons_k(4444),
                       IP_LOOPBACK_N, htons_k(9), (const uint8_t *)"x", 1);
    }
    if (icmp_unreach_tx() - u0 > 1) {
        failed |= icmp_fail("rate limiter did not limit");
    }
    if (icmp_rate_limited() == l0) {
        failed |= icmp_fail("rate limiter limited without counting");
    }

    serial_write_string(failed ? "[icmp] FAILED\n" : "[icmp] ALL PASSED\n");
}
