#ifndef NEOOS_NET_H
#define NEOOS_NET_H

#include <stdint.h>

// NeoOS's network stack: IPv4 and UDP over a loopback device.
//
// SCOPE, stated plainly because the gap between this and a network
// stack is large: there is one device and it is loopback, there is no
// link layer, no ARP, no routing table, and no TCP. What there IS is a
// real IPv4 datagram path with real checksums and a real socket layer
// on top, so that `socket`/`bind`/`sendto`/`recvfrom` are the same
// calls a ported program would make, and adding a NIC later means
// adding a driver and a link layer under an interface that already
// works rather than inventing the whole stack at once.
//
// A DEVICE HERE CARRIES IP PACKETS, NOT ETHERNET FRAMES. That is
// honest for loopback, which has no medium and therefore no framing,
// and it is the seam a real driver will have to change: an Ethernet
// NIC needs a link layer (and ARP) below this interface, not beside it.

#define NET_MTU_LOOPBACK 65536

// Everything on the wire is big-endian; x86 is not. These are the only
// conversions in the stack, so a value's byte order is decided at the
// one point it crosses the boundary rather than argued about at each
// use. Fields named _n in a struct are network order.
static inline uint16_t htons_k(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs_k(uint16_t v) { return htons_k(v); }
static inline uint32_t htonl_k(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl_k(uint32_t v) { return htonl_k(v); }

#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17

// 127.0.0.1, in network order.
#define IP_LOOPBACK_N 0x0100007Fu

struct netdev {
    const char *name;
    uint32_t    mtu;
    uint32_t    ip_n;        // this interface's address, network order
    // Hands one complete IPv4 packet to the device. The device is
    // responsible for getting it to the other end -- which, for
    // loopback, means handing it straight back to net_ipv4_input.
    int       (*transmit)(struct netdev *dev, const uint8_t *pkt, uint32_t len);
    uint64_t    tx_packets, rx_packets, rx_dropped;
};

void net_init(void);
void net_selftest(void);

// The interface an address belongs to, or 0. With one loopback device
// this answers only for 127.0.0.0/8 and INADDR_ANY.
struct netdev *net_route(uint32_t dst_ip_n);
// Builds an IPv4 header around `payload` and hands the result to the
// device. Returns 0, or a negative errno.
int net_ipv4_output(uint32_t src_n, uint32_t dst_n, uint8_t protocol,
                    const uint8_t *payload, uint32_t len);

// The receive path. A device calls this with one complete IPv4 packet.
void net_ipv4_input(struct netdev *dev, const uint8_t *pkt, uint32_t len);

// UDP. Implemented in net.c; the socket layer is its only caller.
int  net_udp_output(uint32_t src_n, uint16_t sport_n,
                    uint32_t dst_n, uint16_t dport_n,
                    const uint8_t *data, uint32_t len);

// Delivered by the UDP receive path to the socket layer. Defined in
// socket.c so that net.c does not need to know what a socket is.
void net_udp_deliver(uint32_t src_n, uint16_t sport_n,
                     uint32_t dst_n, uint16_t dport_n,
                     const uint8_t *data, uint32_t len);

// The one's-complement sum every IPv4 checksum is built from. Exposed
// because UDP's covers a pseudo-header as well as its own.
uint16_t net_checksum(const void *data, uint32_t len, uint32_t start);

#endif
