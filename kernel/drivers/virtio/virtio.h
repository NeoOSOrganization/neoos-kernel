#ifndef NEOOS_VIRTIO_H
#define NEOOS_VIRTIO_H

#include <stdint.h>
#include "../pci/pci.h"

// The virtio transport, legacy (0.9.5) over PCI.
//
// LEGACY, DELIBERATELY. QEMU's virtio devices are transitional: they
// speak both this and the modern (1.0) interface. Legacy puts its whole
// register block in BAR0 in I/O SPACE, so the driver is inl/outl at
// fixed offsets -- no PCI capability list to walk, no BAR to map, no
// page tables touched at all. Modern would need all three for identical
// behaviour under QEMU. The cost is recorded: a PCIe-only virtio device
// (disable-legacy=on) would not be driveable by this, and moving to
// modern is a change here that no driver above would notice.
//
// Nothing in this file knows what a packet is. virtio-blk, when it
// comes, uses it unchanged.

// Register offsets in BAR0. These are the no-MSI-X layout; enabling
// MSI-X would shift device configuration from 0x14 to 0x18, which is
// why this driver does not enable it.
#define VIRTIO_PCI_DEVICE_FEATURES 0x00   // r,  32
#define VIRTIO_PCI_DRIVER_FEATURES 0x04   // w,  32
#define VIRTIO_PCI_QUEUE_PFN       0x08   // rw, 32: physical address >> 12
#define VIRTIO_PCI_QUEUE_SIZE      0x0C   // r,  16
#define VIRTIO_PCI_QUEUE_SELECT    0x0E   // w,  16
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10   // w,  16
#define VIRTIO_PCI_STATUS          0x12   // rw,  8
// ISR IS 0x13, AND CONFIG IS 0x14. They are adjacent, and putting the
// ISR on top of the config block reads the first byte of device
// configuration instead -- for virtio-net that is the first byte of the
// MAC, whose bit 0 is clear in 52:54:..., so every interrupt looked
// spurious, was never acknowledged (the ISR read is the acknowledge),
// and the level-triggered line stayed asserted: 110,000 interrupts and
// not one frame delivered, with the frame sitting in the used ring.
#define VIRTIO_PCI_ISR             0x13   // r,   8, clears on read
#define VIRTIO_PCI_CONFIG          0x14   // device-specific config begins here

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FAILED      0x80

// Descriptor flags. F_WRITE marks a buffer the DEVICE writes into (a
// receive buffer); without it the buffer is driver-to-device only.
#define VRING_DESC_F_NEXT   0x1
#define VRING_DESC_F_WRITE  0x2

// Set in the AVAILABLE ring's flags to tell the device not to raise an
// interrupt when it completes buffers on that queue. Used for a queue
// the driver polls -- transmit -- and never for one it waits on.
#define VRING_AVAIL_F_NO_INTERRUPT 0x1

#define VRING_ALIGN 4096

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];       // qsize entries, then a u16 used_event
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;           // index of the head descriptor of the chain
    uint32_t len;          // bytes the DEVICE wrote into it
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];   // qsize entries, then a u16 avail_event
} __attribute__((packed));

struct virtqueue {
    uint16_t index;        // which queue this is, for QUEUE_SELECT/NOTIFY
    uint16_t size;         // entries, as reported by the device
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;

    uint16_t free_head;    // head of the free-descriptor list
    uint16_t num_free;
    uint16_t last_used;    // used->idx as of the last time we looked

    uint64_t ring_phys;    // the one contiguous block all three live in
    unsigned ring_order;
};

struct virtio_device {
    const struct pci_device *pci;
    uint16_t io_base;      // BAR0, I/O space
    uint32_t features;     // what was actually negotiated
};

// Steps 1-4 of the handshake: reset, ACKNOWLEDGE, DRIVER, and feature
// negotiation against `wanted`. The queues are set up by the caller
// between this and virtio_start(), because a device that sees DRIVER_OK
// before its queues exist may legitimately do anything.
int  virtio_begin(struct virtio_device *vd, const struct pci_device *pci,
                  uint32_t wanted);
void virtio_start(struct virtio_device *vd);          // step 6: DRIVER_OK
void virtio_fail(struct virtio_device *vd);           // tell the device we gave up

uint8_t  virtio_read_config8(const struct virtio_device *vd, uint8_t offset);
uint8_t  virtio_read_isr(const struct virtio_device *vd);

// Allocates the ring, tells the device where it is, and leaves every
// descriptor on the free list.
int  virtio_queue_setup(struct virtio_device *vd, struct virtqueue *vq, uint16_t index);

// Puts one buffer on the available ring. `device_writable` marks it as
// a buffer the DEVICE fills (a receive buffer) rather than one it reads.
// Returns the descriptor index, or -1 when the queue is full.
int  virtio_queue_add(struct virtqueue *vq, uint64_t phys, uint32_t len, int device_writable);
void virtio_queue_notify(struct virtio_device *vd, struct virtqueue *vq);

// Takes one completed buffer off the used ring. Returns its descriptor
// index and stores how many bytes the device wrote, or -1 if the device
// has not completed anything since the last call.
int  virtio_queue_get(struct virtqueue *vq, uint32_t *len_out);

// Returns a descriptor to the free list once the caller is done with the
// buffer it points at.
void virtio_queue_free(struct virtqueue *vq, uint16_t desc);

#endif
