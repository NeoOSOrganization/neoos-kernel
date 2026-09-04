#ifndef NEOOS_ROUTE_H
#define NEOOS_ROUTE_H

#include <stdint.h>

// The routing table, which exists because there is now more than one
// device to send to.
//
// Until D2 there was one interface and "routing" was a membership test:
// 127.0.0.0/8 is loopback's, everything else is unreachable. With a NIC
// there are three questions instead of one -- which device, what source
// address, and WHICH ADDRESS TO RESOLVE -- and only the third is
// surprising. A packet for 8.8.8.8 goes out of eth0 addressed, at the
// link layer, to the GATEWAY's MAC. That distinction does not exist
// above this file and cannot be reconstructed below it, so route_lookup
// answers both halves in one call.

// Four is the minimum that works -- 127/8, the all-ones broadcast, the
// local subnet and a default -- and a table with no spare slot fails by
// silently refusing the last route somebody adds. Eight.
#define ROUTE_MAX 8

struct netdev;

// Adds a route. `gw_n == 0` means "on link": the destination is reached
// directly and is its own next hop. Longest prefix wins. Adding a route
// with the same dst/mask replaces it. Returns 0, or -ENOSPC.
int route_add(uint32_t dst_n, uint32_t mask_n, uint32_t gw_n, struct netdev *dev);

// Removes every route through `dev`. What a DHCP client calls before
// installing a new lease, so that a changed address cannot leave a
// route to the old subnet behind.
void route_flush_dev(struct netdev *dev);

// The device to send through, and -- in *next_hop_n -- the address the
// link layer must resolve: the destination itself when the route is on
// link, the gateway when it is not. Returns 0 when nothing matches, and
// leaves *next_hop_n untouched.
struct netdev *route_lookup(uint32_t dst_n, uint32_t *next_hop_n);

// Installs 127.0.0.0/8 -> loopback. Called from net_init once the
// loopback device exists.
void route_init(void);

void route_selftest(void);

#endif
