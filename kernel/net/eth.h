#ifndef NEOOS_ETH_H
#define NEOOS_ETH_H

#include <stdint.h>

// The link layer D1 left a hole for.
//
// D1's netdev has TWO transmit hooks: `transmit`, documented to take
// one complete IPv4 packet, and `transmit_frame`, taking a complete
// Ethernet frame. An Ethernet device could not honour the first --
// there was nothing that could turn a packet into a frame -- so
// virtio-net answered it with -ENETUNREACH and every real send went
// through the second. This file is the layer that closes that gap, and
// with it `transmit` means the same thing for every device again.
//
// Above this file, nothing knows what a frame is. Below it,
// `transmit_frame` is the link layer's private door onto the wire.

struct netdev;

#define ETH_HDR_LEN   14
#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806
// The minimum frame a medium will carry, without the 4-byte FCS the
// device appends. QEMU does not care; a real switch drops a runt, which
// makes this the kind of bug that only appears off the emulator.
#define ETH_MIN_FRAME 60
#define ETH_MAX_FRAME 1514
#define ETH_MTU       1500

struct eth_header {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype_n;
} __attribute__((packed));
_Static_assert(sizeof(struct eth_header) == ETH_HDR_LEN, "Ethernet header must be 14 bytes");

// netrx's handler, called once per received frame ON THE NETRX THREAD,
// where sleeping is allowed. Registered with netrx_set_handler.
void eth_input(struct netdev *dev, const uint8_t *frame, uint32_t len);

// Builds a frame around `pkt` and hands it to dev->transmit_frame,
// padding to ETH_MIN_FRAME. Returns 0, -EMSGSIZE, or the driver's error.
int eth_send(struct netdev *dev, const uint8_t dst_mac[6],
             uint16_t ethertype, const uint8_t *pkt, uint32_t len);

// The `transmit` hook for an Ethernet device: resolve the next hop,
// frame the packet, send it. Returns 0 when the packet has gone out OR
// has been queued behind an ARP request, and a negative errno when it
// will not go at all.
int eth_output_ipv4(struct netdev *dev, const uint8_t *pkt, uint32_t len);

void eth_stats(uint64_t *rx_ip, uint64_t *rx_arp, uint64_t *rx_unknown,
               uint64_t *rx_runt, uint64_t *tx_frames);

#endif
