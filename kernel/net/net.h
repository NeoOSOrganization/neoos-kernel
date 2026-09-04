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
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

// Wire layout, exactly. Packed because the compiler must not pad it:
// these bytes go on a network, not into a register. It lives in the
// header rather than in net.c because eth.c, icmp.c and tcp.c all have
// to read a header none of them built.
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
_Static_assert(sizeof(struct ipv4_header) == 20, "IPv4 header must be 20 bytes");

#define IPV4_VERSION_IHL_20  0x45   // version 4, 5 32-bit words of header
#define IPV4_DEFAULT_TTL     64
#define IPV4_FLAG_DF_N       0x0040 // network order of 0x4000

// The all-ones broadcast. A DHCP client has to be able to receive a
// packet addressed to this before it has an address of its own.
#define IP_BROADCAST_N 0xFFFFFFFFu

// 127.0.0.1, in network order.
#define IP_LOOPBACK_N 0x0100007Fu

// What a device carries. Loopback has no medium and therefore no
// framing; an Ethernet device has both, and the difference decides
// whether `transmit` or `transmit_frame` is the way in.
#define NETDEV_LOOPBACK 0
#define NETDEV_ETHERNET 1

struct netdev {
    const char *name;
    uint32_t    mtu;
    uint32_t    ip_n;        // this interface's address, network order
    int         type;        // NETDEV_LOOPBACK or NETDEV_ETHERNET
    uint8_t     hwaddr[6];   // the MAC, zero on loopback
    // Hands one complete IPv4 packet to the device. The device is
    // responsible for getting it to the other end -- which, for
    // loopback, means handing it straight back to net_ipv4_input.
    int       (*transmit)(struct netdev *dev, const uint8_t *pkt, uint32_t len);
    // Hands one COMPLETE ETHERNET FRAME to the device -- header,
    // payload and all. Null on loopback. It exists beside `transmit`
    // rather than replacing it because D1 has no link layer: there is
    // nothing yet that can turn an IP packet into a frame, so an
    // Ethernet device answers `transmit` with -ENETUNREACH and this is
    // the only way onto the wire. D2 builds the layer between them.
    int       (*transmit_frame)(struct netdev *dev, const uint8_t *frame, uint32_t len);
    uint64_t    tx_packets, rx_packets, rx_dropped;
};

void net_init(void);
void net_selftest(void);

// Registers a second interface beside loopback. D1 adds one; the
// routing below still sends everything to loopback, because there is no
// link layer yet -- D2 gives this device an address and a real route.
// Returns 0, or negative when there is no room.
int net_register(struct netdev *dev);
// The registered non-loopback interface, or 0. D1's selftest needs it
// to reach the NIC without knowing which driver provided it.
struct netdev *net_device(void);
// The loopback device. route.c installs 127.0.0.0/8 through it, and it
// is file-static in net.c otherwise.
struct netdev *net_loopback(void);

// Gives an interface an address and installs its two routes: the local
// subnet on link, and the default via `gw_n` (0 for no default). The
// old routes through that device are removed first, so a changed
// address cannot leave a route to the previous subnet behind.
//
// D2 calls this once with a provisional static address, because ARP
// cannot be tested by a host that has no address to put in the sender
// field. D4's DHCP client calls the same function with a real lease --
// which is the point of it being a function rather than three lines in
// the boot path.
void net_ifconfig(struct netdev *dev, uint32_t addr_n, uint32_t mask_n,
                  uint32_t gw_n);

// Whether `ip_n` is an address THIS HOST HOLDS -- which is what bind()
// actually asks, and is not the same question as "can I route there".
//
// While loopback was the only device those two questions had the same
// answer, and socket_bind asked the routing one. The moment a default
// route existed, bind(8.8.8.8) started succeeding: perfectly routable,
// and not ours. Linux returns EADDRNOTAVAIL for exactly this.
int net_is_local_addr(uint32_t ip_n);

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
// Returns 0 when a socket exists on that port, or -ENOENT when nothing
// is bound there -- which is what D3 turns into an ICMP port
// unreachable. A connected socket that filters out a stranger still
// returns 0: the port is open, it is just not listening to them, and
// saying otherwise would be a lie the sender acts on.
int net_udp_deliver(uint32_t src_n, uint16_t sport_n,
                    uint32_t dst_n, uint16_t dport_n,
                    const uint8_t *data, uint32_t len);

// A kernel-internal consumer of ONE UDP port, checked before the socket
// table. dhcp.c is its only user, and it exists so that a lease can be
// obtained without SO_BROADCAST, without raw sockets, and without an
// address to bind to -- three pieces of user-facing surface that would
// otherwise have to be designed and documented before the machine could
// find out its own address.
//
// A hooked port is never "closed": no ICMP port-unreachable is
// generated for it.
typedef void (*udp_kernel_handler)(struct netdev *dev,
                                   uint32_t src_n, uint16_t sport_n,
                                   uint32_t dst_n, uint16_t dport_n,
                                   const uint8_t *data, uint32_t len);
int net_udp_hook(uint16_t port_n, udp_kernel_handler fn);   // 0, or -EBUSY

// The one's-complement sum every IPv4 checksum is built from. Exposed
// because UDP's covers a pseudo-header as well as its own.
uint16_t net_checksum(const void *data, uint32_t len, uint32_t start);

// The transport checksum: the segment, folded together with the
// PSEUDO-HEADER of source, destination, protocol and length. That is
// what makes it detect a segment delivered to the wrong host --
// information neither the UDP nor the TCP header carries.
//
// One function for both protocols. It returns the value to STORE in the
// checksum field (already network order, and never 0 -- a computed zero
// is transmitted as 0xFFFF, which one's-complement arithmetic makes
// legal rather than a fudge). To VERIFY, call it with the received
// checksum left in place and compare the segment's own folded sum
// against zero via net_l4_verify.
uint16_t net_l4_checksum(uint32_t src_n, uint32_t dst_n, uint8_t proto,
                         const void *seg, uint32_t len);
// Nonzero when the segment's checksum is correct.
int net_l4_verify(uint32_t src_n, uint32_t dst_n, uint8_t proto,
                  const void *seg, uint32_t len);

#endif
