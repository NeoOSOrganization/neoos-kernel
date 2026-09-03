#ifndef NEOOS_VIRTIO_NET_H
#define NEOOS_VIRTIO_NET_H

#include <stdint.h>

#define VIRTIO_NET_VENDOR   0x1AF4
// Transitional virtio-net: the legacy device id. A modern-only device
// would be 0x1041 and would not be driveable by the legacy transport.
#define VIRTIO_NET_DEVICE   0x1000

// The only feature accepted. Everything else -- checksum offload, TSO,
// mergeable receive buffers -- changes the shape of the packets the
// driver sees, and each is a separate thing to get wrong. Refusing them
// gets plain whole frames, which is what a first driver wants.
#define VIRTIO_NET_F_MAC    (1u << 5)

// Prepended to every buffer in both directions. TEN bytes on legacy
// without MRG_RXBUF, which is what is negotiated here; TWELVE under
// modern virtio or with mergeable receive buffers, which append a
// num_buffers field. Getting this wrong shifts every frame by two bytes
// and looks like a broken device rather than a miscounted header.
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

#define VIRTIO_NET_HDR_LEN 10
#define ETH_FRAME_MAX      1514
#define ETH_ALEN           6

#define VECTOR_VIRTIO_NET 0x22

// Finds the device, drives the handshake, sets up both queues and
// registers a netdev. Safe to call on a machine with no virtio-net:
// it says so and returns non-zero.
int  virtio_net_init(void);
void virtio_net_selftest(void);

// Called from the interrupt handler. Does the least possible: reads the
// ISR, moves completed frames into the RX queue, and wakes the RX
// thread. Everything sleepable happens in that thread instead.
void virtio_net_irq(void);

// Sends one complete Ethernet frame. Synchronous: returns once the
// device has reported the buffer back.
int  virtio_net_transmit(const uint8_t *frame, uint32_t len);

const uint8_t *virtio_net_mac(void);
int virtio_net_present(void);
// The device's PCI interrupt line, as a GSI. The kernel routes it to
// VECTOR_VIRTIO_NET; the driver does not, because only the boot path
// knows the IOAPIC's GSI base.
uint8_t virtio_net_irq_line(void);

void virtio_net_stats(uint64_t *tx, uint64_t *rx, uint64_t *tx_timeouts,
                      uint64_t *irqs);

#endif
