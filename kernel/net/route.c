#include "net/route.h"
#include "net/net.h"
#include "drivers/char/serial.h"
#include "errno.h"

// NO LOCK, and the reason belongs here rather than in a commit message.
//
// This table is written twice in the life of a machine: once by
// route_init at boot, and again by the DHCP thread when a lease is
// installed or replaced. It is READ on every single transmit.
//
// A lock would therefore be taken millions of times to protect two
// writes. What a reader can observe without one is a torn view ACROSS
// entries -- a half-installed lease, where the subnet route is visible
// and the default route is not yet. The consequence of that is one
// packet routed by the old table, which a routing table is allowed to
// do: routes change and packets in flight do not care. What a reader
// CANNOT observe is a torn entry, because each field is an aligned word
// and `dev` is published last (see route_add).
//
// The day routes become writable from userland -- a route(8), a second
// NIC, anything that changes the table while traffic is flowing -- this
// comment is the thing that says what changed and why a lock is now
// needed. Until then it would be ceremony.

struct route {
    uint32_t dst_n, mask_n, gw_n;
    struct netdev *dev;         // 0 means the slot is free
    int prefix_len;
};

static struct route table[ROUTE_MAX];

static int mask_prefix_len(uint32_t mask_n) {
    uint32_t m = ntohl_k(mask_n);
    int n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

int route_add(uint32_t dst_n, uint32_t mask_n, uint32_t gw_n, struct netdev *dev) {
    if (!dev) { return -EINVAL; }
    struct route *slot = 0;
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (table[i].dev && table[i].dst_n == (dst_n & mask_n) &&
            table[i].mask_n == mask_n) {
            slot = &table[i];       // replace
            break;
        }
        if (!slot && !table[i].dev) { slot = &table[i]; }
    }
    if (!slot) { return -ENOSPC; }

    slot->dst_n      = dst_n & mask_n;
    slot->mask_n     = mask_n;
    slot->gw_n       = gw_n;
    slot->prefix_len = mask_prefix_len(mask_n);
    // `dev` last: it is what makes the slot live to a concurrent
    // reader, so every other field must already be true when it lands.
    slot->dev        = dev;
    return 0;
}

void route_flush_dev(struct netdev *dev) {
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (table[i].dev == dev) { table[i].dev = 0; }
    }
}

struct netdev *route_lookup(uint32_t dst_n, uint32_t *next_hop_n) {
    struct route *best = 0;
    for (int i = 0; i < ROUTE_MAX; i++) {
        struct route *r = &table[i];
        struct netdev *dev = r->dev;      // read once
        if (!dev) { continue; }
        if ((dst_n & r->mask_n) != r->dst_n) { continue; }
        // LONGEST prefix, not first match. A table that returns the
        // first match sends everything through whichever route happens
        // to sit lowest in the array, which for a table containing a
        // default route means everything goes to the gateway --
        // including the machine's own subnet.
        if (!best || r->prefix_len > best->prefix_len) { best = r; }
    }
    if (!best) { return 0; }
    if (next_hop_n) { *next_hop_n = best->gw_n ? best->gw_n : dst_n; }
    return best->dev;
}

void route_init(void) {
    for (int i = 0; i < ROUTE_MAX; i++) { table[i].dev = 0; }
    // 127.0.0.0/8. INADDR_ANY (0.0.0.0) is handled by net_route, which
    // means "this host" and therefore loopback, rather than by a
    // 0.0.0.0/32 route that would collide with the default route's
    // 0.0.0.0/0 in a way that reads as a bug.
    route_add(htonl_k(0x7F000000u), htonl_k(0xFF000000u), 0, net_loopback());
}

// ------------------------------------------------------------- selftest

static int route_fail(const char *msg) {
    serial_write_string("[route] FAILED: ");
    serial_write_string(msg);
    serial_write_string("\n");
    return 1;
}

void route_selftest(void) {
    serial_write_string("[route] selftest\n");
    int failed = 0;

    // Save and restore: this runs at boot, before DHCP, but a selftest
    // that leaves a fake device in the routing table is a selftest that
    // breaks the next thing to transmit.
    struct route saved[ROUTE_MAX];
    for (int i = 0; i < ROUTE_MAX; i++) { saved[i] = table[i]; }

    struct netdev *lo = net_loopback();
    static struct netdev fake;
    fake.name = "fake";
    fake.type = NETDEV_ETHERNET;
    fake.ip_n = 0x0F02000Au;                // 10.0.2.15

    // The DEFAULT route goes in FIRST, and that ordering is the whole
    // point. Added the other way round, a table that returns the first
    // match instead of the longest still answers 10.0.2.1 correctly --
    // it trips over the /24 before it reaches the /0 -- and the
    // assertion below passes for the wrong reason. Proving the detector
    // is what found that; the order stays.
    if (route_add(0, 0, 0x0202000Au, &fake) != 0) {
        failed |= route_fail("could not add the default route");
    }
    if (route_add(0x0002000Au, 0x00FFFFFFu, 0, &fake) != 0) {
        failed |= route_fail("could not add the subnet route");
    }

    uint32_t nh = 0;
    if (route_lookup(0x0100007Fu, &nh) != lo || nh != 0x0100007Fu) {
        failed |= route_fail("127.0.0.1 did not route to lo");
    }

    nh = 0;
    if (route_lookup(0x0902000Au, &nh) != &fake || nh != 0x0902000Au) {
        failed |= route_fail("on-link next hop is not the destination");
    }

    nh = 0;
    if (route_lookup(0x08080808u, &nh) != &fake || nh != 0x0202000Au) {
        failed |= route_fail("default route next hop is not the gateway");
    }

    // The one that catches a first-match table: 10.0.2.1 matches both
    // the /24 and the /0, and the /24 must win.
    nh = 0;
    if (route_lookup(0x0102000Au, &nh) != &fake || nh != 0x0102000Au) {
        failed |= route_fail("/0 beat /24");
    }

    route_flush_dev(&fake);
    if (route_lookup(0x08080808u, &nh) != 0) {
        failed |= route_fail("flush left a route behind");
    }

    for (int i = 0; i < ROUTE_MAX; i++) { table[i] = saved[i]; }

    serial_write_string(failed ? "[route] FAILED\n" : "[route] ALL PASSED\n");
}
