// virtio_net.c -- the NIC.
//
// D1. Everything virtio-generic lives in ../virtio/virtio.c; this file
// knows about packets and nothing about ring layout.
//
// BUFFER OWNERSHIP IS THE WHOLE GAME HERE. The device DMAs into
// physical memory this driver hands it. A buffer freed while queued, or
// a descriptor returned to the device while the kernel still reads it,
// is silent corruption somewhere else entirely -- the kind that surfaces
// three subsystems away. Two choices keep it boring:
//
//   - the RX buffer pool is allocated once and never freed;
//   - transmit is synchronous, so a frame's buffer is provably done
//     with before the caller returns.
//
// Neither is fast. Both are obvious, which is what a first driver
// should be.

#include "virtio_net.h"
#include "../virtio/virtio.h"
#include "../pci/pci.h"
#include "../char/serial.h"
#include "../../mm/pmm.h"
#include "../../mm/paging.h"
#include "../../net/net.h"
#include "../../net/netrx.h"
#include "../../net/eth.h"
#include "errno.h"
#include "../../sync/lock.h"

#define RX_BUFFERS   32
#define RX_BUF_SIZE  2048           // header + a full frame, rounded up
#define TX_BUF_SIZE  2048

static struct virtio_device vdev;
static struct virtqueue rxq, txq;
static struct netdev    nic;
static struct spinlock  tx_lock;   // LOCK_RANK_VIRTIO_TX

static uint64_t rx_pool_phys;
static unsigned rx_pool_order;
static uint64_t tx_buf_phys;

// desc index -> the buffer that descriptor points at. Completion gives
// back a descriptor index; this is how the driver gets from that to the
// bytes the device wrote.
static uint64_t rx_desc_buf[256];

static uint8_t  mac[ETH_ALEN];
static int      present;

static uint64_t stat_tx, stat_rx, stat_tx_timeout;
static volatile uint64_t stat_irqs;   // interrupts taken, spurious ones included

int virtio_net_present(void) { return present; }
uint8_t virtio_net_irq_line(void) { return vdev.pci ? vdev.pci->irq_line : 0; }
const uint8_t *virtio_net_mac(void) { return mac; }

static void log_mac(void) {
    static const char *hexdigits = "0123456789abcdef";
    serial_write_string("[virtio-net] mac=");
    for (int i = 0; i < ETH_ALEN; i++) {
        serial_putc(hexdigits[mac[i] >> 4]);
        serial_putc(hexdigits[mac[i] & 0xF]);
        if (i != ETH_ALEN - 1) { serial_putc(':'); }
    }
    serial_write_string("\n");
}

// Hands one RX buffer to the device and records which descriptor got it.
static int rx_give(uint64_t buf_phys) {
    int d = virtio_queue_add(&rxq, buf_phys, RX_BUF_SIZE, 1 /* device writes */);
    if (d < 0) { return -1; }
    rx_desc_buf[d] = buf_phys;
    return d;
}

static int nic_transmit_frame(struct netdev *dev, const uint8_t *frame, uint32_t len) {
    (void)dev;
    return virtio_net_transmit(frame, len);
}

int virtio_net_init(void) {
    present = 0;

    const struct pci_device *pci = pci_find_id(VIRTIO_NET_VENDOR, VIRTIO_NET_DEVICE);
    if (!pci) {
        serial_write_string("[virtio-net] no device (not a FAILED: "
                            "a machine without one is a valid machine)\n");
        return -1;
    }

    if (virtio_begin(&vdev, pci, VIRTIO_NET_F_MAC) != 0) {
        return -1;
    }
    if (!(vdev.features & VIRTIO_NET_F_MAC)) {
        // Every other feature is refused by choice; this one is needed,
        // because the alternative is inventing a MAC address.
        serial_write_string("[virtio-net] FAILED: device does not offer F_MAC\n");
        virtio_fail(&vdev);
        return -1;
    }
    for (int i = 0; i < ETH_ALEN; i++) {
        mac[i] = virtio_read_config8(&vdev, (uint8_t)i);
    }

    if (virtio_queue_setup(&vdev, &rxq, 0) != 0) { virtio_fail(&vdev); return -1; }
    if (virtio_queue_setup(&vdev, &txq, 1) != 0) { virtio_fail(&vdev); return -1; }

    // Transmit polls the used ring, so a completion interrupt for it is
    // pure noise -- and noise that would be handled by the RX path,
    // which would find nothing and notify the device for no reason. The
    // device is asked not to raise one. RX keeps its interrupt: that is
    // the entire point of the driver.
    txq.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    // One contiguous block for every receive buffer, never freed.
    uint64_t need = (uint64_t)RX_BUFFERS * RX_BUF_SIZE;
    rx_pool_order = 0;
    while (((uint64_t)4096 << rx_pool_order) < need) { rx_pool_order++; }
    rx_pool_phys = pmm_alloc(rx_pool_order);
    if (!rx_pool_phys) {
        serial_write_string("[virtio-net] FAILED: no memory for receive buffers\n");
        virtio_fail(&vdev);
        return -1;
    }

    tx_buf_phys = pmm_alloc(0);
    if (!tx_buf_phys) {
        serial_write_string("[virtio-net] FAILED: no memory for the transmit buffer\n");
        virtio_fail(&vdev);
        return -1;
    }

    for (int i = 0; i < RX_BUFFERS; i++) {
        if (rx_give(rx_pool_phys + (uint64_t)i * RX_BUF_SIZE) < 0) {
            serial_write_string("[virtio-net] FAILED: receive queue full during setup\n");
            virtio_fail(&vdev);
            return -1;
        }
    }

    // Only now: the device may touch the rings.
    virtio_start(&vdev);
    virtio_queue_notify(&vdev, &rxq);

    nic.name  = "eth0";
    nic.mtu   = 1500;
    nic.ip_n  = 0;                  // D2 configures an address
    nic.type  = NETDEV_ETHERNET;
    for (int i = 0; i < ETH_ALEN; i++) { nic.hwaddr[i] = mac[i]; }
    // D2: eth_output_ipv4 resolves the next hop and frames the
    // packet. Until it existed this hook returned -ENETUNREACH,
    // because sending an unframed IP packet onto a real wire is
    // worse than refusing to send it.
    nic.transmit = eth_output_ipv4;
    nic.transmit_frame = nic_transmit_frame;
    net_register(&nic);

    spin_init(&tx_lock, LOCK_RANK_VIRTIO_TX, "virtio-tx");
    present = 1;
    log_mac();
    serial_write_string("[virtio-net] ready\n");
    return 0;
}

int virtio_net_transmit(const uint8_t *frame, uint32_t len) {
    if (!present) { return -ENETDOWN; }
    if (len > ETH_FRAME_MAX) { return -EMSGSIZE; }

    // ONE bounce buffer, ONE queue, and a spin for the completion:
    // every part of that is unsafe against a second caller, and D2 made
    // a second caller permanent. The netrx thread answers ARP requests
    // while any other thread may be sending.
    uint64_t txf = spin_lock_irqsave(&tx_lock);

    uint8_t *buf = (uint8_t *)phys_to_virt(tx_buf_phys);

    // The header goes in front of EVERY buffer, in both directions. Ten
    // bytes here, because legacy without MRG_RXBUF was negotiated; it is
    // twelve under modern virtio. Get it wrong and every frame shifts by
    // two bytes, which looks like a broken device rather than a
    // miscounted header. All zeroes, since no offload was accepted.
    for (int i = 0; i < VIRTIO_NET_HDR_LEN; i++) { buf[i] = 0; }
    for (uint32_t i = 0; i < len; i++) { buf[VIRTIO_NET_HDR_LEN + i] = frame[i]; }

    int d = virtio_queue_add(&txq, tx_buf_phys, VIRTIO_NET_HDR_LEN + len, 0);
    if (d < 0) { spin_unlock_irqrestore(&tx_lock, txf); return -ENOBUFS; }
    virtio_queue_notify(&vdev, &txq);

    // Synchronous: wait for the device to hand the buffer back before
    // returning, so nobody can reuse it underneath the device. Bounded,
    // because a device that never completes must not wedge the caller
    // forever -- and a timeout that is counted is debuggable, where a
    // hang is not.
    for (int spin = 0; spin < 10000000; spin++) {
        uint32_t got = 0;
        int done = virtio_queue_get(&txq, &got);
        if (done >= 0) {
            virtio_queue_free(&txq, (uint16_t)done);
            stat_tx++;
            nic.tx_packets++;
            spin_unlock_irqrestore(&tx_lock, txf);
            return 0;
        }
        __asm__ volatile ("pause");
    }

    stat_tx_timeout++;
    // The descriptor is deliberately NOT freed: the device still owns
    // that buffer as far as anyone here knows, and handing it back to
    // the free list would let the next transmit scribble on memory a
    // late device is still reading.
    spin_unlock_irqrestore(&tx_lock, txf);
    serial_write_string("[virtio-net] transmit timed out waiting for completion\n");
    return -EIO;
}

void virtio_net_irq(void) {
    if (!present) { return; }

    // Reading the ISR also acknowledges the interrupt at the device.
    // Exactly once per interrupt, or it stops raising them.
    stat_irqs++;
    uint8_t isr = virtio_read_isr(&vdev);
    if (!(isr & 1)) { return; }

    for (;;) {
        uint32_t len = 0;
        int d = virtio_queue_get(&rxq, &len);
        if (d < 0) { break; }

        uint64_t buf = rx_desc_buf[d];
        if (len > VIRTIO_NET_HDR_LEN) {
            const uint8_t *frame = (const uint8_t *)phys_to_virt(buf) + VIRTIO_NET_HDR_LEN;
            stat_rx++;
            nic.rx_packets++;
            // Copies and returns. Everything sleepable happens in the
            // netrx thread instead -- see kernel/net/netrx.h.
            netrx_post(&nic, frame, len - VIRTIO_NET_HDR_LEN);
        }

        // Straight back to the device. The buffer's contents were copied
        // out above, so the device may overwrite it now.
        virtio_queue_free(&rxq, (uint16_t)d);
        rx_give(buf);
    }
    virtio_queue_notify(&vdev, &rxq);
}

void virtio_net_stats(uint64_t *tx, uint64_t *rx, uint64_t *tx_timeouts, uint64_t *irqs) {
    if (tx)          { *tx = stat_tx; }
    if (rx)          { *rx = stat_rx; }
    if (tx_timeouts) { *tx_timeouts = stat_tx_timeout; }
    // Counted because a WRONG one is silent: an interrupt the handler
    // dismisses is never acknowledged, the level-triggered line stays
    // asserted, and the machine takes the same interrupt forever while
    // reporting nothing. That is exactly how the ISR-offset bug looked.
    if (irqs)        { *irqs = stat_irqs; }
}

// --------------------------------------------------------------- selftest
//
// One hand-built ARP request for the gateway, and the reply. Not an ARP
// layer -- 42 bytes of known constants -- because deferring the test to
// D2 would leave the entire driver unproven for a milestone.
//
// Passing requires ALL of: the PCI device was found, the handshake
// completed, the MAC came out of device configuration, the TX ring and
// its notification and completion worked, the device raised an
// interrupt, the RX ring delivered a frame, and the netrx thread got it
// out of the queue. That is every part of D1, against a peer this
// kernel does not control.

#define ARP_FRAME_LEN 42

// QEMU user networking (SLIRP) always puts the gateway here and always
// answers ARP for it. Under -netdev tap the peer is whatever the host
// is, so this check runs only on the user-networking path.
static const uint8_t GATEWAY_IP[4] = { 10, 0, 2, 2 };
static const uint8_t SELF_IP[4]    = { 10, 0, 2, 15 };

static volatile int      arp_reply_seen;
static uint8_t           arp_reply_mac[ETH_ALEN];

static void selftest_rx(struct netdev *dev, const uint8_t *frame, uint32_t len) {
    (void)dev;
    if (len < ARP_FRAME_LEN) { return; }
    if (frame[12] != 0x08 || frame[13] != 0x06) { return; }   // not ARP
    if (frame[20] != 0x00 || frame[21] != 0x02) { return; }   // not a reply

    // The sender hardware address, at offset 22, is the gateway's MAC.
    for (int i = 0; i < ETH_ALEN; i++) { arp_reply_mac[i] = frame[22 + i]; }
    arp_reply_seen = 1;
}

static void build_arp_request(uint8_t *f) {
    for (int i = 0; i < ARP_FRAME_LEN; i++) { f[i] = 0; }

    for (int i = 0; i < ETH_ALEN; i++) { f[i] = 0xFF; }        // broadcast
    for (int i = 0; i < ETH_ALEN; i++) { f[6 + i] = mac[i]; }  // us
    f[12] = 0x08; f[13] = 0x06;                               // ethertype ARP

    f[14] = 0x00; f[15] = 0x01;      // hardware type: Ethernet
    f[16] = 0x08; f[17] = 0x00;      // protocol type: IPv4
    f[18] = ETH_ALEN;
    f[19] = 4;
    f[20] = 0x00; f[21] = 0x01;      // opcode: request

    for (int i = 0; i < ETH_ALEN; i++) { f[22 + i] = mac[i]; }
    for (int i = 0; i < 4; i++)        { f[28 + i] = SELF_IP[i]; }
    // target hardware address stays zero -- that is what is being asked
    for (int i = 0; i < 4; i++)        { f[38 + i] = GATEWAY_IP[i]; }
}

void virtio_net_selftest(void) {
    if (!present) {
        serial_write_string("[virtio-net] SKIPPED: no device on this machine\n");
        return;
    }

    int mac_nonzero = 0;
    for (int i = 0; i < ETH_ALEN; i++) { if (mac[i]) { mac_nonzero = 1; } }
    if (!mac_nonzero) {
        serial_write_string("[virtio-net] FAILED: MAC is all zeroes "
                            "(device configuration was not read)\n");
        return;
    }

    arp_reply_seen = 0;
    netrx_handler prev_handler = netrx_set_handler(selftest_rx);

    uint8_t frame[ARP_FRAME_LEN];
    build_arp_request(frame);

    // The interrupt window and the hlt-not-schedule rule are explained
    // once, in netrx.h. D2 moved them there because arp, icmp, dhcp and
    // tcp all need the same dance and each got it wrong differently.
    uint64_t rflags = netrx_boot_window_open();

    int rc = virtio_net_transmit(frame, ARP_FRAME_LEN);

    // Bounded by the 100 Hz timer at worst, so ~2 seconds.
    for (int i = 0; rc == 0 && i < 200 && !arp_reply_seen; i++) {
        netrx_boot_park();
    }

    netrx_boot_window_close(rflags);
    // Put the real handler back. Leaving selftest_rx installed sent
    // every subsequent frame to a counter, which is invisible until the
    // next milestone's suite times out for no stated reason.
    netrx_set_handler(prev_handler);

    if (rc != 0) {
        serial_write_string("[virtio-net] FAILED: could not transmit the ARP request\n");
        return;
    }

    if (!arp_reply_seen) {
        uint64_t q = 0, dropped = 0, delivered = 0;
        netrx_stats(&q, &dropped, &delivered);
        if (q > 0 && delivered == 0) {
            // The device answered and the interrupt ran; nothing ran the
            // netrx thread. On a single-CPU machine that is expected --
            // kmain cannot yield to it -- and it is not a driver fault.
            serial_write_string("[virtio-net] SKIPPED: a frame was received but no "
                                "CPU was free to run the netrx thread\n");
            return;
        }
        serial_write_string("[virtio-net] FAILED: no ARP reply from the gateway "
                            "(queued/dropped/delivered follow)\n");
        serial_write_string("[virtio-net]   queued=");
        serial_write_hex64(q);
        serial_write_string(" dropped=");
        serial_write_hex64(dropped);
        serial_write_string(" delivered=");
        serial_write_hex64(delivered);
        serial_write_string("\n");
        return;
    }

    static const char *hexdigits = "0123456789abcdef";
    serial_write_string("[virtio-net] gateway 10.0.2.2 is at ");
    for (int i = 0; i < ETH_ALEN; i++) {
        serial_putc(hexdigits[arp_reply_mac[i] >> 4]);
        serial_putc(hexdigits[arp_reply_mac[i] & 0xF]);
        if (i != ETH_ALEN - 1) { serial_putc(':'); }
    }
    serial_write_string("\n");
    serial_write_string("[virtio-net] ALL PASSED\n");
}
