// kernel/net/net.c -- the loopback device, IPv4, and UDP.
//
// The whole receive path runs in the SENDER'S context: loopback's
// transmit hands the packet straight back to net_ipv4_input, which
// parses it and delivers it to a socket before sendto() has returned.
// That is what loopback is, and it keeps the stack free of a softirq
// mechanism NeoOS does not have yet. The cost is that a receive is
// charged to whoever sent it, and that the sender must hold no lock the
// receive path takes -- which the socket layer's send path is careful
// about.

#include "net/net.h"
#include "dev/serial.h"
#include "errno.h"
#include "mm/heap.h"

// ---------------------------------------------------------------- checksum

// The internet checksum: the one's-complement sum of 16-bit words, then
// complemented. `start` carries a partial sum in, which is what lets
// UDP fold its pseudo-header in without building a contiguous buffer
// for it.
uint16_t net_checksum(const void *data, uint32_t len, uint32_t start) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = start;

    while (len > 1) {
        // Read as big-endian explicitly rather than casting to
        // uint16_t*: the buffer is not guaranteed 2-byte aligned, and
        // the arithmetic is defined on the wire's byte order, not the
        // host's.
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) { sum += (uint32_t)p[0] << 8; }   // odd trailing byte, high half

    while (sum >> 16) { sum = (sum & 0xFFFF) + (sum >> 16); }
    return (uint16_t)~sum;
}

// The same accumulation, stopping before the fold, for callers that
// have more to add.
static uint32_t checksum_partial(const void *data, uint32_t len, uint32_t start) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = start;
    while (len > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) { sum += (uint32_t)p[0] << 8; }
    return sum;
}

static uint16_t checksum_fold(uint32_t sum) {
    while (sum >> 16) { sum = (sum & 0xFFFF) + (sum >> 16); }
    return (uint16_t)~sum;
}

// ---------------------------------------------------------------- headers

// Wire layout, exactly. Packed because the compiler must not pad it:
// these bytes go on a network, not into a register.
struct ipv4_header {
    uint8_t  version_ihl;      // 4 bits each
    uint8_t  tos;
    uint16_t total_length_n;
    uint16_t id_n;
    uint16_t flags_frag_n;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum_n;
    uint32_t src_n;
    uint32_t dst_n;
} __attribute__((packed));

struct udp_header {
    uint16_t sport_n;
    uint16_t dport_n;
    uint16_t length_n;         // header + payload
    uint16_t checksum_n;
} __attribute__((packed));

_Static_assert(sizeof(struct ipv4_header) == 20, "IPv4 header must be 20 bytes");
_Static_assert(sizeof(struct udp_header)  == 8,  "UDP header must be 8 bytes");

#define IPV4_VERSION_IHL_20  0x45   // version 4, 5 32-bit words of header
#define IPV4_DEFAULT_TTL     64
#define IPV4_FLAG_DF_N       0x0040 // network order of 0x4000

// ------------------------------------------------------------- loopback

static struct netdev loopback;
static uint16_t ip_next_id;   // only ever incremented; wrapping is fine

static int loopback_transmit(struct netdev *dev, const uint8_t *pkt, uint32_t len) {
    dev->tx_packets++;
    // Straight back up. No copy: net_ipv4_input does not retain the
    // buffer, and the socket layer copies the payload into its own
    // datagram before returning.
    net_ipv4_input(dev, pkt, len);
    return 0;
}

struct netdev *net_loopback(void) { return &loopback; }

struct netdev *net_route(uint32_t dst_ip_n) {
    // One interface, so "routing" is a membership test. 127.0.0.0/8 is
    // loopback's, and so is INADDR_ANY -- a datagram sent to 0.0.0.0
    // goes to this host, which with one interface is the same place.
    uint32_t host = ntohl_k(dst_ip_n);
    if (dst_ip_n == 0 || (host >> 24) == 127) { return &loopback; }
    return 0;
}

// ------------------------------------------------------------------ IPv4

int net_ipv4_output(uint32_t src_n, uint32_t dst_n, uint8_t protocol,
                    const uint8_t *payload, uint32_t len) {
    struct netdev *dev = net_route(dst_n);
    if (!dev) { return -ENETUNREACH; }
    if (len + sizeof(struct ipv4_header) > dev->mtu) { return -EMSGSIZE; }

    uint32_t total = (uint32_t)sizeof(struct ipv4_header) + len;
    uint8_t *pkt = (uint8_t *)kmalloc(total);
    if (!pkt) { return -ENOBUFS; }

    struct ipv4_header *ip = (struct ipv4_header *)pkt;
    ip->version_ihl    = IPV4_VERSION_IHL_20;
    ip->tos            = 0;
    ip->total_length_n = htons_k((uint16_t)total);
    ip->id_n           = htons_k(ip_next_id++);
    // Don't Fragment, offset zero. Nothing here fragments, and a
    // datagram too large for the link is refused above rather than
    // split -- so saying DF is the truth rather than a hint.
    ip->flags_frag_n   = IPV4_FLAG_DF_N;
    ip->ttl            = IPV4_DEFAULT_TTL;
    ip->protocol       = protocol;
    ip->checksum_n     = 0;
    ip->src_n          = src_n ? src_n : dev->ip_n;
    ip->dst_n          = dst_n ? dst_n : dev->ip_n;
    // Computed over the header with the checksum field zero, which is
    // why it is written last.
    ip->checksum_n     = htons_k(net_checksum(ip, sizeof(*ip), 0));

    for (uint32_t i = 0; i < len; i++) { pkt[sizeof(*ip) + i] = payload[i]; }

    int rc = dev->transmit(dev, pkt, total);
    kfree(pkt);
    return rc;
}

static void udp_input(struct netdev *dev, const struct ipv4_header *ip,
                      const uint8_t *payload, uint32_t len);

void net_ipv4_input(struct netdev *dev, const uint8_t *pkt, uint32_t len) {
    dev->rx_packets++;

    if (len < sizeof(struct ipv4_header)) { dev->rx_dropped++; return; }
    const struct ipv4_header *ip = (const struct ipv4_header *)pkt;

    if ((ip->version_ihl >> 4) != 4) { dev->rx_dropped++; return; }
    uint32_t ihl = (uint32_t)(ip->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(struct ipv4_header) || ihl > len) { dev->rx_dropped++; return; }

    uint32_t total = ntohs_k(ip->total_length_n);
    if (total < ihl || total > len) { dev->rx_dropped++; return; }

    // A checksum over a header that already contains its own checksum
    // sums to zero. Verified even on loopback, where nothing can
    // corrupt a packet: it is the check that catches this stack
    // building a header wrong, which is the failure that actually
    // happens.
    if (net_checksum(ip, ihl, 0) != 0) { dev->rx_dropped++; return; }

    // Fragments are dropped rather than reassembled. Nothing here
    // fragments, so a fragment means a packet from somewhere this stack
    // cannot talk to yet.
    if (ntohs_k(ip->flags_frag_n) & 0x3FFF) {
        if (!(ntohs_k(ip->flags_frag_n) & 0x2000) &&
            (ntohs_k(ip->flags_frag_n) & 0x1FFF) == 0) {
            // offset 0 and no MF: not actually a fragment.
        } else {
            dev->rx_dropped++;
            return;
        }
    }

    if (ip->protocol == IPPROTO_UDP) {
        udp_input(dev, ip, pkt + ihl, total - ihl);
        return;
    }
    // Every other protocol, ICMP included, is silently dropped. An
    // ICMP echo responder would be twenty lines, but nothing can reach
    // it: there are no raw sockets, so no way to send a ping or see a
    // reply. It arrives with raw sockets.
    dev->rx_dropped++;
}

// -------------------------------------------------------------------- UDP

// UDP's checksum covers a PSEUDO-HEADER as well as the datagram: the
// source and destination addresses, the protocol, and the UDP length.
// That is what makes it detect a datagram delivered to the wrong host
// -- information the UDP header itself does not carry.
static uint32_t udp_pseudo_sum(uint32_t src_n, uint32_t dst_n, uint16_t udp_len) {
    uint32_t sum = 0;
    sum = checksum_partial(&src_n, 4, sum);
    sum = checksum_partial(&dst_n, 4, sum);
    sum += IPPROTO_UDP;
    sum += udp_len;
    return sum;
}

int net_udp_output(uint32_t src_n, uint16_t sport_n,
                   uint32_t dst_n, uint16_t dport_n,
                   const uint8_t *data, uint32_t len) {
    if (len > 65535 - sizeof(struct udp_header)) { return -EMSGSIZE; }

    uint32_t total = (uint32_t)sizeof(struct udp_header) + len;
    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) { return -ENOBUFS; }

    struct udp_header *udp = (struct udp_header *)buf;
    udp->sport_n    = sport_n;
    udp->dport_n    = dport_n;
    udp->length_n   = htons_k((uint16_t)total);
    udp->checksum_n = 0;
    for (uint32_t i = 0; i < len; i++) { buf[sizeof(*udp) + i] = data[i]; }

    struct netdev *dev = net_route(dst_n);
    uint32_t real_src = src_n ? src_n : (dev ? dev->ip_n : 0);
    uint32_t sum = udp_pseudo_sum(real_src, dst_n ? dst_n : real_src,
                                  (uint16_t)total);
    uint16_t ck = checksum_fold(checksum_partial(buf, total, sum));
    // htons, for the same reason the IP header's checksum is converted:
    // net_checksum accumulates the buffer as BIG-endian 16-bit words
    // and returns a host-order integer whose value is that big-endian
    // reading. Storing it back without converting puts the two bytes in
    // the wrong order, and every receiver then computes a non-zero sum
    // and drops the datagram -- which is exactly what happened here,
    // silently, since a dropped datagram has nobody to report to.
    //
    // A computed checksum of zero is transmitted as 0xFFFF, because a
    // zero in the field means "no checksum" and the two must not be
    // confused. One's-complement arithmetic has two zeros, which is
    // what makes this legal rather than a fudge.
    udp->checksum_n = htons_k(ck ? ck : 0xFFFF);

    int rc = net_ipv4_output(src_n, dst_n, IPPROTO_UDP, buf, total);
    kfree(buf);
    return rc;
}

static void udp_input(struct netdev *dev, const struct ipv4_header *ip,
                      const uint8_t *payload, uint32_t len) {
    if (len < sizeof(struct udp_header)) { dev->rx_dropped++; return; }
    const struct udp_header *udp = (const struct udp_header *)payload;

    uint32_t udp_len = ntohs_k(udp->length_n);
    if (udp_len < sizeof(struct udp_header) || udp_len > len) {
        dev->rx_dropped++;
        return;
    }

    // Zero means the sender did not compute one, which IPv4 permits.
    if (udp->checksum_n != 0) {
        uint32_t sum = udp_pseudo_sum(ip->src_n, ip->dst_n, (uint16_t)udp_len);
        if (checksum_fold(checksum_partial(payload, udp_len, sum)) != 0) {
            dev->rx_dropped++;
            return;
        }
    }

    net_udp_deliver(ip->src_n, udp->sport_n, ip->dst_n, udp->dport_n,
                    payload + sizeof(*udp),
                    udp_len - (uint32_t)sizeof(*udp));
}

// ------------------------------------------------------------------- init

void net_init(void) {
    loopback.name     = "lo";
    loopback.mtu      = NET_MTU_LOOPBACK;
    loopback.ip_n     = IP_LOOPBACK_N;
    loopback.transmit = loopback_transmit;
    serial_write_string("[net] loopback up, 127.0.0.1\n");
}

// --------------------------------------------------------------- selftest

// Asserts the two things that are easy to get wrong and impossible to
// see from the outside: the byte-order helpers, and the checksum.
//
// The checksum vector is a real IPv4 header with a known answer, taken
// from the worked example in RFC 1071. A checksum routine that is
// wrong in a self-consistent way passes a round trip against itself,
// which is why the expected value is written down rather than computed.
void net_selftest(void) {
    if (htons_k(0x1234) != 0x3412 || ntohs_k(0x3412) != 0x1234) {
        serial_write_string("[net] selftest FAILED: htons/ntohs\n");
        return;
    }
    if (htonl_k(0x12345678u) != 0x78563412u) {
        serial_write_string("[net] selftest FAILED: htonl\n");
        return;
    }
    // 127.0.0.1 written as bytes 127,0,0,1 is 0x0100007F little-endian.
    if (IP_LOOPBACK_N != htonl_k(0x7F000001u)) {
        serial_write_string("[net] selftest FAILED: loopback address byte order\n");
        return;
    }

    // RFC 1071 section 3: this header checksums to 0xB861.
    static const uint8_t hdr[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
        0xC0, 0xA8, 0x00, 0xC7
    };
    uint16_t ck = net_checksum(hdr, sizeof(hdr), 0);
    if (ck != 0xB861) {
        serial_write_string("[net] selftest FAILED: checksum=");
        serial_write_hex64(ck);
        serial_write_string(" want 0xb861\n");
        return;
    }

    // And a header carrying its own checksum must sum to zero, which is
    // the form the receive path actually uses.
    uint8_t full[20];
    for (int i = 0; i < 20; i++) { full[i] = hdr[i]; }
    full[10] = (uint8_t)(ck >> 8);
    full[11] = (uint8_t)(ck & 0xFF);
    if (net_checksum(full, sizeof(full), 0) != 0) {
        serial_write_string("[net] selftest FAILED: self-checked header did not sum to zero\n");
        return;
    }

    if (!net_route(IP_LOOPBACK_N)) {
        serial_write_string("[net] selftest FAILED: 127.0.0.1 has no route\n");
        return;
    }
    if (net_route(htonl_k(0x08080808u))) {
        serial_write_string("[net] selftest FAILED: an off-link address was routed\n");
        return;
    }

    serial_write_string("[net] selftest passed\n");
}
