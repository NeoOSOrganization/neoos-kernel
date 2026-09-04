#include "net/eth.h"
#include "net/net.h"
#include "net/arp.h"
#include "net/route.h"
#include "errno.h"

static uint64_t rx_ip, rx_arp, rx_unknown, rx_runt, tx_frames;

void eth_stats(uint64_t *ip, uint64_t *arp, uint64_t *unknown,
               uint64_t *runt, uint64_t *tx) {
    if (ip)      { *ip      = rx_ip; }
    if (arp)     { *arp     = rx_arp; }
    if (unknown) { *unknown = rx_unknown; }
    if (runt)    { *runt    = rx_runt; }
    if (tx)      { *tx      = tx_frames; }
}

int eth_send(struct netdev *dev, const uint8_t dst_mac[6],
             uint16_t ethertype, const uint8_t *pkt, uint32_t len) {
    if (!dev || !dev->transmit_frame) { return -ENODEV; }
    if (len > ETH_MTU)                { return -EMSGSIZE; }

    // On the stack, not the heap: this runs on the netrx thread and on
    // whatever thread is transmitting, and 1514 bytes is affordable on
    // both. It also means an ARP reply built during a receive cannot
    // fail for want of memory, which is the one time a stack cannot
    // afford to.
    uint8_t frame[ETH_MAX_FRAME];
    struct eth_header *h = (struct eth_header *)frame;
    for (int i = 0; i < 6; i++) {
        h->dst[i] = dst_mac[i];
        h->src[i] = dev->hwaddr[i];
    }
    h->ethertype_n = htons_k(ethertype);
    for (uint32_t i = 0; i < len; i++) { frame[ETH_HDR_LEN + i] = pkt[i]; }

    uint32_t total = ETH_HDR_LEN + len;
    // Pad, and pad with ZEROS rather than with whatever was on the
    // stack: the padding of a 42-byte ARP request goes onto the wire,
    // and leaking stack bytes onto a network is a real thing that has
    // happened to real stacks.
    while (total < ETH_MIN_FRAME) { frame[total++] = 0; }

    tx_frames++;
    return dev->transmit_frame(dev, frame, total);
}

void eth_input(struct netdev *dev, const uint8_t *frame, uint32_t len) {
    if (len < ETH_HDR_LEN) { rx_runt++; return; }

    const struct eth_header *h = (const struct eth_header *)frame;
    uint16_t type = ntohs_k(h->ethertype_n);
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint32_t plen = len - ETH_HDR_LEN;

    switch (type) {
    case ETH_TYPE_IPV4:
        rx_ip++;
        net_ipv4_input(dev, payload, plen);
        break;
    case ETH_TYPE_ARP:
        rx_arp++;
        arp_input(dev, payload, plen);
        break;
    default:
        // Counted, NOT logged. A real segment carries broadcast traffic
        // this host has no opinion about, and one serial write per
        // frame is a denial of service against its own boot.
        rx_unknown++;
        break;
    }
}

int eth_output_ipv4(struct netdev *dev, const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct ipv4_header)) { return -EINVAL; }
    const struct ipv4_header *ip = (const struct ipv4_header *)pkt;

    uint32_t next_hop = 0;
    if (!route_lookup(ip->dst_n, &next_hop)) { return -ENETUNREACH; }

    uint8_t mac[6];
    int rc = arp_resolve(dev, next_hop, mac, pkt, len);
    if (rc == -EAGAIN) {
        // Queued behind an ARP request. NOT an error: the packet is
        // going to be sent, and a caller told "error" for a packet
        // about to go out will send it a second time.
        return 0;
    }
    if (rc != 0) { return rc; }

    return eth_send(dev, mac, ETH_TYPE_IPV4, pkt, len);
}
