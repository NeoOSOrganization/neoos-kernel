// virtio.c -- the legacy virtio PCI transport.
//
// Device init is a fixed handshake and the ORDER IS NOT ADVISORY. A
// device that sees DRIVER_OK before its queues are configured is
// entitled to start using queues that do not exist yet:
//
//   1. reset             write 0 to status
//   2. ACKNOWLEDGE       "I see you"
//   3. DRIVER            "I know how to drive you"
//   4. features          read what it offers, write back what we accept
//   5. queues            (the caller does this)
//   6. DRIVER_OK         the device may now touch the rings
//
// Steps 1-4 are virtio_begin, step 6 is virtio_start, and the gap
// between them is where the caller sets up its queues.

#include "virtio.h"
#include "../../arch/io.h"
#include "../../mm/pmm.h"
#include "../../mm/paging.h"
#include "../char/serial.h"

static void *zero_pages(uint64_t phys, uint64_t bytes) {
    uint8_t *p = (uint8_t *)phys_to_virt(phys);
    for (uint64_t i = 0; i < bytes; i++) { p[i] = 0; }
    return p;
}

int virtio_begin(struct virtio_device *vd, const struct pci_device *pci, uint32_t wanted) {
    vd->pci = pci;

    // Legacy virtio lives in an I/O BAR. Finding a memory BAR here means
    // the device is modern-only, which this transport cannot drive --
    // and saying so is much better than reading zeros forever.
    if (!pci->bar[0].is_io || pci->bar[0].size == 0) {
        serial_write_string("[virtio] FAILED: BAR0 is not I/O space "
                            "(modern-only device? this transport is legacy)\n");
        return -1;
    }
    vd->io_base = (uint16_t)pci->bar[0].addr;

    outb(vd->io_base + VIRTIO_PCI_STATUS, 0);                      // reset
    outb(vd->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    outb(vd->io_base + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    uint32_t offered = inl(vd->io_base + VIRTIO_PCI_DEVICE_FEATURES);
    vd->features = offered & wanted;
    outl(vd->io_base + VIRTIO_PCI_DRIVER_FEATURES, vd->features);

    // The device is allowed to bus-master the rings, and until this bit
    // is set it cannot. Forgetting it gives a driver that configures
    // everything correctly and moves not one byte.
    pci_enable_bus_master(pci);
    return 0;
}

void virtio_start(struct virtio_device *vd) {
    outb(vd->io_base + VIRTIO_PCI_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
}

void virtio_fail(struct virtio_device *vd) {
    outb(vd->io_base + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
}

uint8_t virtio_read_config8(const struct virtio_device *vd, uint8_t offset) {
    return inb((uint16_t)(vd->io_base + VIRTIO_PCI_CONFIG + offset));
}

uint8_t virtio_read_isr(const struct virtio_device *vd) {
    // Reading ISR also acknowledges the interrupt at the device. There
    // is no separate acknowledge, so this must happen exactly once per
    // interrupt or the device stops raising them.
    return inb((uint16_t)(vd->io_base + VIRTIO_PCI_ISR));
}

// The legacy ring layout, in one contiguous block:
//
//   desc   16 * qsize
//   avail  2 + 2 + 2*qsize + 2      (the trailing u16 is used_event)
//   ...    padding to the next 4096 boundary
//   used   2 + 2 + 8*qsize + 2      (the trailing u16 is avail_event)
//
// THE PADDING IS NOT OPTIONAL. The device computes the used ring's
// address from the same formula; get it wrong and it writes completions
// into the padding, where nothing ever looks, and the queue simply never
// completes anything -- with no error reported anywhere.
static uint64_t ring_bytes(uint16_t qsize) {
    uint64_t desc_avail = (uint64_t)16 * qsize
                        + (uint64_t)2 * (3 + qsize);
    uint64_t aligned = (desc_avail + VRING_ALIGN - 1) & ~(uint64_t)(VRING_ALIGN - 1);
    uint64_t used = 6 + (uint64_t)8 * qsize;
    uint64_t used_aligned = (used + VRING_ALIGN - 1) & ~(uint64_t)(VRING_ALIGN - 1);
    return aligned + used_aligned;
}

static unsigned order_for(uint64_t bytes) {
    unsigned order = 0;
    uint64_t got = 4096;
    while (got < bytes) { got <<= 1; order++; }
    return order;
}

int virtio_queue_setup(struct virtio_device *vd, struct virtqueue *vq, uint16_t index) {
    outw(vd->io_base + VIRTIO_PCI_QUEUE_SELECT, index);
    uint16_t qsize = inw(vd->io_base + VIRTIO_PCI_QUEUE_SIZE);
    if (qsize == 0) {
        serial_write_string("[virtio] FAILED: queue does not exist\n");
        return -1;
    }

    uint64_t bytes = ring_bytes(qsize);
    unsigned order = order_for(bytes);

    // pmm_alloc gives 2^order PHYSICALLY CONTIGUOUS frames, which is
    // exactly the requirement: the device reads these through physical
    // addresses and knows nothing about page tables.
    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        serial_write_string("[virtio] FAILED: no memory for the ring\n");
        return -1;
    }

    uint8_t *base = zero_pages(phys, (uint64_t)4096 << order);

    uint64_t desc_avail = (uint64_t)16 * qsize + (uint64_t)2 * (3 + qsize);
    uint64_t used_off = (desc_avail + VRING_ALIGN - 1) & ~(uint64_t)(VRING_ALIGN - 1);

    vq->index      = index;
    vq->size       = qsize;
    vq->desc       = (struct vring_desc *)base;
    vq->avail      = (struct vring_avail *)(base + 16 * (uint64_t)qsize);
    vq->used       = (struct vring_used *)(base + used_off);
    vq->ring_phys  = phys;
    vq->ring_order = order;
    vq->last_used  = 0;

    // Every descriptor starts free, chained through `next`.
    for (uint16_t i = 0; i < qsize; i++) {
        vq->desc[i].next = (uint16_t)(i + 1);
    }
    vq->desc[qsize - 1].next = 0;
    vq->free_head = 0;
    vq->num_free  = qsize;

    // The device is told the ring's location as a page frame number.
    outl(vd->io_base + VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));
    return 0;
}

int virtio_queue_add(struct virtqueue *vq, uint64_t phys, uint32_t len, int device_writable) {
    if (vq->num_free == 0) { return -1; }

    uint16_t d = vq->free_head;
    vq->free_head = vq->desc[d].next;
    vq->num_free--;

    vq->desc[d].addr  = phys;
    vq->desc[d].len   = len;
    vq->desc[d].flags = device_writable ? VRING_DESC_F_WRITE : 0;
    vq->desc[d].next  = 0;

    // Publish the descriptor, THEN the index. The device may be reading
    // the ring on another CPU at this instant; a compiler or CPU that
    // reorders these lets it see an index pointing at a descriptor that
    // has not been written yet.
    vq->avail->ring[vq->avail->idx % vq->size] = d;
    __asm__ volatile ("" ::: "memory");
    vq->avail->idx++;
    __asm__ volatile ("" ::: "memory");
    return d;
}

void virtio_queue_notify(struct virtio_device *vd, struct virtqueue *vq) {
    outw(vd->io_base + VIRTIO_PCI_QUEUE_NOTIFY, vq->index);
}

int virtio_queue_get(struct virtqueue *vq, uint32_t *len_out) {
    // The device bumps used->idx after writing the entry. Read the index
    // first, then the entry it makes visible.
    if (vq->last_used == vq->used->idx) { return -1; }
    __asm__ volatile ("" ::: "memory");

    struct vring_used_elem *e = &vq->used->ring[vq->last_used % vq->size];
    uint32_t id  = e->id;
    uint32_t len = e->len;
    vq->last_used++;

    if (id >= vq->size) { return -1; }   // a device that lies is not followed
    if (len_out) { *len_out = len; }
    return (int)id;
}

void virtio_queue_free(struct virtqueue *vq, uint16_t desc) {
    if (desc >= vq->size) { return; }
    vq->desc[desc].next = vq->free_head;
    vq->free_head = desc;
    vq->num_free++;
}
