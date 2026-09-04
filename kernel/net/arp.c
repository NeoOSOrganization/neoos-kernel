#include "net/arp.h"
#include "net/net.h"
#include "net/eth.h"
#include "sync/lock.h"
#include "drivers/char/serial.h"
#include "drivers/char/timer.h"
#include "smp/smp.h"
#include "net/netrx.h"
#include "errno.h"

#define ARP_FREE    0
#define ARP_PENDING 1
#define ARP_VALID   2

// One packet per entry, not a queue.
//
// The alternative is a per-entry list, which needs an allocation policy
// for something that happens once per destination per minute and whose
// only consumer so far is the first packet of a connection. Head-of-line
// replacement is what small stacks do here, and the cost of getting it
// wrong is one retransmission -- which every protocol above already has
// to handle.
struct arp_entry {
    uint32_t ip_n;
    uint8_t  mac[6];
    uint8_t  state;
    uint8_t  requests;
    uint64_t deadline;        // expiry when VALID, next retry when PENDING
    struct netdev *dev;
    uint32_t pending_len;
    uint8_t  pending[ETH_MTU];
};

static struct arp_entry cache[ARP_CACHE_MAX];
static struct spinlock  arp_lock;
static uint64_t stat_requests_tx, stat_replies_tx, stat_learned, stat_dropped;

static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void arp_init(void) {
    spin_init(&arp_lock, LOCK_RANK_ARP, "arp");
    for (int i = 0; i < ARP_CACHE_MAX; i++) { cache[i].state = ARP_FREE; }
}

void arp_stats(uint64_t *req, uint64_t *rep, uint64_t *learned, uint64_t *dropped) {
    if (req)     { *req     = stat_requests_tx; }
    if (rep)     { *rep     = stat_replies_tx; }
    if (learned) { *learned = stat_learned; }
    if (dropped) { *dropped = stat_dropped; }
}

uint64_t arp_replies_sent(void) { return stat_replies_tx; }

// Caller holds arp_lock.
//
// Expiry is LAZY, checked here rather than only in arp_tick, because
// arp_tick runs on the netrx thread and the netrx thread sleeps when no
// frames are arriving. A cache that expired entries only on a tick
// would hand out a sixty-second-old MAC indefinitely on an idle
// machine -- which is exactly the machine on which a peer's NIC gets
// swapped without anybody noticing.
static struct arp_entry *find(uint32_t ip_n) {
    uint64_t now = timer_ticks();
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (cache[i].state == ARP_FREE || cache[i].ip_n != ip_n) { continue; }
        if (cache[i].state == ARP_VALID && now >= cache[i].deadline) {
            cache[i].state = ARP_FREE;
            return 0;
        }
        return &cache[i];
    }
    return 0;
}

// Whether anything is waiting on a reply. netrx asks so it knows to
// wake on a timer instead of sleeping until the next frame -- and a
// retry that only fires when a frame happens to arrive is not a retry.
int arp_pending(void) {
    uint64_t f = spin_lock_irqsave(&arp_lock);
    int n = 0;
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (cache[i].state == ARP_PENDING) { n++; }
    }
    spin_unlock_irqrestore(&arp_lock, f);
    return n;
}

// Caller holds arp_lock. A FREE slot if there is one, otherwise the
// VALID entry closest to expiry -- never a PENDING one, because
// stealing a PENDING entry drops a packet somebody is waiting on and
// leaves a request outstanding that nothing will ever match.
static struct arp_entry *claim(void) {
    struct arp_entry *oldest = 0;
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (cache[i].state == ARP_FREE) { return &cache[i]; }
        if (cache[i].state == ARP_VALID &&
            (!oldest || cache[i].deadline < oldest->deadline)) {
            oldest = &cache[i];
        }
    }
    return oldest;
}

// Not called with the lock held: it transmits.
static void send_request(struct netdev *dev, uint32_t target_n) {
    struct arp_packet p;
    p.htype_n = htons_k(1);
    p.ptype_n = htons_k(ETH_TYPE_IPV4);
    p.hlen    = 6;
    p.plen    = 4;
    p.op_n    = htons_k(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) { p.sha[i] = dev->hwaddr[i]; p.tha[i] = 0; }
    // Byte-wise, because spa_n sits at offset 14 in a packed struct and
    // is therefore not 4-byte aligned. A plain assignment through a
    // uint32_t* would be an unaligned store the compiler is entitled to
    // assume never happens.
    uint32_t src = dev->ip_n;
    for (int i = 0; i < 4; i++) {
        ((uint8_t *)&p.spa_n)[i] = ((const uint8_t *)&src)[i];
        ((uint8_t *)&p.tpa_n)[i] = ((const uint8_t *)&target_n)[i];
    }
    stat_requests_tx++;
    eth_send(dev, bcast, ETH_TYPE_ARP, (const uint8_t *)&p, sizeof p);
}

static uint32_t load_be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t v;
    ((uint8_t *)&v)[0] = b[0]; ((uint8_t *)&v)[1] = b[1];
    ((uint8_t *)&v)[2] = b[2]; ((uint8_t *)&v)[3] = b[3];
    return v;                                  // still network order
}

int arp_lookup_only(uint32_t ip_n, uint8_t mac[6]) {
    uint64_t f = spin_lock_irqsave(&arp_lock);
    struct arp_entry *e = find(ip_n);
    if (e && e->state == ARP_VALID) {
        for (int i = 0; i < 6; i++) { mac[i] = e->mac[i]; }
        spin_unlock_irqrestore(&arp_lock, f);
        return 0;
    }
    spin_unlock_irqrestore(&arp_lock, f);
    return -ENOENT;
}

void arp_force_expire(uint32_t ip_n) {
    uint64_t f = spin_lock_irqsave(&arp_lock);
    struct arp_entry *e = find(ip_n);
    if (e) { e->deadline = 0; }
    spin_unlock_irqrestore(&arp_lock, f);
    arp_tick();
}

int arp_resolve(struct netdev *dev, uint32_t ip_n, uint8_t mac[6],
                const uint8_t *pkt, uint32_t len) {
    if (!dev) { return -ENODEV; }
    // Broadcast needs no resolution, and asking for it would sit
    // PENDING forever waiting for a reply nobody sends.
    if (ip_n == IP_BROADCAST_N || ip_n == 0) {
        for (int i = 0; i < 6; i++) { mac[i] = 0xFF; }
        return 0;
    }

    uint64_t f = spin_lock_irqsave(&arp_lock);
    struct arp_entry *e = find(ip_n);

    if (e && e->state == ARP_VALID) {
        for (int i = 0; i < 6; i++) { mac[i] = e->mac[i]; }
        spin_unlock_irqrestore(&arp_lock, f);
        return 0;
    }

    if (e && e->state == ARP_PENDING) {
        if (len <= ETH_MTU) {
            for (uint32_t i = 0; i < len; i++) { e->pending[i] = pkt[i]; }
            e->pending_len = len;
        }
        spin_unlock_irqrestore(&arp_lock, f);
        return -EAGAIN;
    }

    e = claim();
    if (!e) {
        spin_unlock_irqrestore(&arp_lock, f);
        stat_dropped++;
        return -EHOSTUNREACH;
    }
    e->ip_n        = ip_n;
    e->state       = ARP_PENDING;
    e->requests    = 1;
    e->deadline    = timer_ticks() + ARP_RETRY_TICKS;
    e->dev         = dev;
    e->pending_len = 0;
    if (len <= ETH_MTU) {
        for (uint32_t i = 0; i < len; i++) { e->pending[i] = pkt[i]; }
        e->pending_len = len;
    }
    spin_unlock_irqrestore(&arp_lock, f);

    // OUTSIDE the lock: eth_send reaches virtio_net_transmit, which
    // spins on the device.
    send_request(dev, ip_n);
    return -EAGAIN;
}

void arp_input(struct netdev *dev, const uint8_t *pkt, uint32_t len) {
    if (len < sizeof(struct arp_packet)) { stat_dropped++; return; }
    const struct arp_packet *p = (const struct arp_packet *)pkt;
    if (ntohs_k(p->htype_n) != 1 || ntohs_k(p->ptype_n) != ETH_TYPE_IPV4 ||
        p->hlen != 6 || p->plen != 4) {
        stat_dropped++;
        return;
    }
    uint16_t op    = ntohs_k(p->op_n);
    uint32_t spa_n = load_be32(&p->spa_n);
    uint32_t tpa_n = load_be32(&p->tpa_n);
    int for_us = (tpa_n == dev->ip_n) && dev->ip_n != 0;

    // 1. LEARN, before replying. An entry that already exists is
    //    updated -- which is how the reply to our OWN request lands in
    //    the PENDING entry that is waiting for it rather than creating
    //    a second entry beside it. A new entry is created only from a
    //    request addressed to us: caching every sender on a busy
    //    segment would evict the entries actually in use.
    uint8_t  flush[ETH_MTU];
    uint32_t flush_len = 0;
    uint8_t  flush_mac[6];
    struct netdev *flush_dev = 0;

    uint64_t f = spin_lock_irqsave(&arp_lock);
    struct arp_entry *e = find(spa_n);
    if (!e && op == ARP_OP_REQUEST && for_us) { e = claim(); if (e) { e->ip_n = spa_n; e->pending_len = 0; } }
    if (e) {
        int was_pending = (e->state == ARP_PENDING);
        for (int i = 0; i < 6; i++) { e->mac[i] = p->sha[i]; }
        e->state    = ARP_VALID;
        e->requests = 0;
        e->deadline = timer_ticks() + ARP_VALID_TICKS;
        e->dev      = dev;
        stat_learned++;
        if (was_pending && e->pending_len) {
            // Copy the queued packet OUT under the lock and send it
            // after dropping it.
            for (uint32_t i = 0; i < e->pending_len; i++) { flush[i] = e->pending[i]; }
            flush_len = e->pending_len;
            for (int i = 0; i < 6; i++) { flush_mac[i] = e->mac[i]; }
            flush_dev = e->dev;
            e->pending_len = 0;
        }
    }
    spin_unlock_irqrestore(&arp_lock, f);

    if (flush_len) {
        eth_send(flush_dev, flush_mac, ETH_TYPE_IPV4, flush, flush_len);
    }

    // 2. REPLY, but only for our own address. NeoOS is a host, not a
    //    proxy ARP router, and answering for an address we do not hold
    //    is how a machine quietly blackholes a segment.
    if (op == ARP_OP_REQUEST && for_us) {
        struct arp_packet r;
        r.htype_n = htons_k(1);
        r.ptype_n = htons_k(ETH_TYPE_IPV4);
        r.hlen    = 6;
        r.plen    = 4;
        r.op_n    = htons_k(ARP_OP_REPLY);
        for (int i = 0; i < 6; i++) { r.sha[i] = dev->hwaddr[i]; r.tha[i] = p->sha[i]; }
        for (int i = 0; i < 4; i++) {
            ((uint8_t *)&r.spa_n)[i] = ((const uint8_t *)&tpa_n)[i];
            ((uint8_t *)&r.tpa_n)[i] = ((const uint8_t *)&spa_n)[i];
        }
        stat_replies_tx++;
        eth_send(dev, p->sha, ETH_TYPE_ARP, (const uint8_t *)&r, sizeof r);
    }
}

void arp_tick(void) {
    uint64_t now = timer_ticks();

    // Two passes, for the usual reason: the retries must be sent with
    // the lock dropped, so the first pass decides and the second acts.
    struct { struct netdev *dev; uint32_t ip_n; } retry[ARP_CACHE_MAX];
    int n = 0;

    uint64_t f = spin_lock_irqsave(&arp_lock);
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        struct arp_entry *e = &cache[i];
        if (e->state == ARP_VALID && now >= e->deadline) {
            e->state = ARP_FREE;
        } else if (e->state == ARP_PENDING && now >= e->deadline) {
            if (e->requests >= ARP_MAX_REQUESTS) {
                // Give up. The queued packet is dropped; the next send
                // to this address starts a fresh resolution rather than
                // inheriting a dead one.
                e->state = ARP_FREE;
                stat_dropped++;
            } else {
                e->requests++;
                e->deadline = now + ARP_RETRY_TICKS;
                retry[n].dev  = e->dev;
                retry[n].ip_n = e->ip_n;
                n++;
            }
        }
    }
    spin_unlock_irqrestore(&arp_lock, f);

    for (int i = 0; i < n; i++) { send_request(retry[i].dev, retry[i].ip_n); }
}

// ------------------------------------------------------------- selftest

static int arp_fail(const char *msg) {
    serial_write_string("[arp] FAILED: ");
    serial_write_string(msg);
    serial_write_string("\n");
    return 1;
}

void arp_selftest(void) {
    serial_write_string("[arp] selftest\n");
    struct netdev *dev = net_device();
    if (!dev) {
        serial_write_string("[arp] SKIPPED: no NIC\n");
        return;
    }
    int failed = 0;
    uint8_t mac[6];

    // 1. Resolve the gateway THROUGH THE LAYER. D1 proved the wire by
    //    hand; this proves the cache, the pending slot and completion.
    //
    //    A single-CPU machine reports SKIPPED rather than FAILED: it is
    //    ANOTHER CPU that runs the netrx thread and delivers the reply,
    //    so a missing CPU is a missing CPU and not a broken cache.
    if (smp_cpu_count() < 2) {
        serial_write_string("[arp] SKIPPED: one CPU, nothing drains netrx\n");
    } else {
        uint8_t dummy[20] = {0};
        // The interrupt window: boot runs with interrupts off, so a
        // bare hlt here never returns. netrx.h explains why this is the
        // shape it is.
        uint64_t w = netrx_boot_window_open();
        int rc = arp_resolve(dev, 0x0202000Au /* 10.0.2.2 */, mac, dummy, sizeof dummy);
        for (int i = 0; i < 400 && rc != 0; i++) {
            netrx_boot_park();
            rc = arp_lookup_only(0x0202000Au, mac);
        }
        netrx_boot_window_close(w);
        if (rc != 0) {
            // The stats, on the failure path only: "never resolved"
            // has four different causes and they are told apart by
            // whether the request went out and whether anything came
            // back at all.
            uint64_t req, rep, learned, dropped, e_ip, e_arp, e_unk, e_runt, e_tx;
            arp_stats(&req, &rep, &learned, &dropped);
            eth_stats(&e_ip, &e_arp, &e_unk, &e_runt, &e_tx);
            serial_write_string("[arp] tx_req="); serial_write_hex64(req);
            serial_write_string(" learned="); serial_write_hex64(learned);
            serial_write_string(" rx_arp="); serial_write_hex64(e_arp);
            serial_write_string(" rx_ip="); serial_write_hex64(e_ip);
            serial_write_string(" tx_frames="); serial_write_hex64(e_tx);
            serial_write_string("\n");
            failed |= arp_fail("gateway never resolved");
        } else if (!mac[0] && !mac[1] && !mac[2] && !mac[3] && !mac[4] && !mac[5]) {
            failed |= arp_fail("gateway resolved to a null MAC");
        } else {
            char buf[18];
            static const char hex[] = "0123456789abcdef";
            for (int i = 0; i < 6; i++) {
                buf[i * 3]     = hex[mac[i] >> 4];
                buf[i * 3 + 1] = hex[mac[i] & 0x0F];
                buf[i * 3 + 2] = (i < 5) ? ':' : '\n';
            }
            serial_write_string("[arp] 10.0.2.2 is at ");
            serial_write_raw_n(buf, 18);
        }
    }

    // 2. A request for OUR address is answered, and its sender learned.
    struct arp_packet req;
    req.htype_n = htons_k(1);
    req.ptype_n = htons_k(ETH_TYPE_IPV4);
    req.hlen = 6; req.plen = 4;
    req.op_n = htons_k(ARP_OP_REQUEST);
    const uint8_t peer[6] = { 2, 0, 0, 0, 0, 1 };
    for (int i = 0; i < 6; i++) { req.sha[i] = peer[i]; req.tha[i] = 0; }
    uint32_t peer_ip = 0x0302000Au;                 // 10.0.2.3
    for (int i = 0; i < 4; i++) {
        ((uint8_t *)&req.spa_n)[i] = ((const uint8_t *)&peer_ip)[i];
        ((uint8_t *)&req.tpa_n)[i] = ((const uint8_t *)&dev->ip_n)[i];
    }

    uint64_t before = arp_replies_sent();
    arp_input(dev, (const uint8_t *)&req, sizeof req);
    if (arp_replies_sent() != before + 1) {
        failed |= arp_fail("no reply to a request for our own address");
    }
    if (arp_lookup_only(peer_ip, mac) != 0 || mac[5] != 1) {
        failed |= arp_fail("did not learn from a request addressed to us");
    }

    // 3. A request for SOMEONE ELSE is not answered.
    uint32_t other = 0x6402000Au;                   // 10.0.2.100
    for (int i = 0; i < 4; i++) { ((uint8_t *)&req.tpa_n)[i] = ((const uint8_t *)&other)[i]; }
    before = arp_replies_sent();
    arp_input(dev, (const uint8_t *)&req, sizeof req);
    if (arp_replies_sent() != before) {
        failed |= arp_fail("replied on behalf of another address");
    }

    // 4. An entry expires rather than being trusted forever.
    arp_force_expire(peer_ip);
    if (arp_lookup_only(peer_ip, mac) == 0) {
        failed |= arp_fail("an expired entry was still used");
    }

    // 5. A truncated packet is dropped, not parsed. The target is put
    //    BACK to our own address first, and that matters: with a target
    //    of somebody else, a parser that ignores the length still
    //    declines to reply, and the assertion passes without testing
    //    anything. Proving the detector is what found that.
    for (int i = 0; i < 4; i++) {
        ((uint8_t *)&req.tpa_n)[i] = ((const uint8_t *)&dev->ip_n)[i];
    }
    before = arp_replies_sent();
    arp_input(dev, (const uint8_t *)&req, 10);
    if (arp_replies_sent() != before) {
        failed |= arp_fail("parsed a truncated ARP packet");
    }

    serial_write_string(failed ? "[arp] FAILED\n" : "[arp] ALL PASSED\n");
}
