#ifndef NEOOS_DHCP_H
#define NEOOS_DHCP_H

#include <stdint.h>

// The DHCP client, and it is IN THE KERNEL. That is a divergence from
// Linux, where DHCP is unambiguously a userspace program, and it is
// recorded as one in docs/stdlib.md.
//
// The reason is SURFACE. A userspace client has to receive an offer
// addressed to an address it does not have yet, which needs AF_PACKET
// or raw sockets; it has to send to 255.255.255.255, which needs
// SO_BROADCAST; and it has to install a route, which needs a routing
// API. That is three pieces of user-facing kernel surface -- each with
// its own ABI obligations -- designed and documented before the machine
// can find out its own address. The kernel client needs none of them.
//
// The seam is drawn so the decision can be undone. dhcp.c talks to the
// same net_udp_output / net_udp_hook pair a socket would; replacing it
// with a userspace client later means ADDING raw sockets, not unpicking
// the stack.

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

enum dhcp_state {
    DHCP_INIT, DHCP_SELECTING, DHCP_REQUESTING,
    DHCP_BOUND, DHCP_RENEWING, DHCP_REBINDING,
};

struct dhcp_lease {
    uint32_t addr_n, mask_n, gw_n, server_n, dns_n;
    uint32_t lease_secs, t1_secs, t2_secs;
    // Zero when this is the static fallback rather than a real lease.
    // The selftest asserts on it, because otherwise a BROKEN CLIENT and
    // an ABSENT SERVER produce an identical route table and the suite
    // passes either way.
    int      from_server;
};

struct netdev;

void dhcp_start(struct netdev *dev);
enum dhcp_state dhcp_state(void);
const struct dhcp_lease *dhcp_current(void);    // 0 until bound

// Parser tests: each returns 0 if the malformed offer was ACCEPTED,
// which is the failure. Exposed so the selftest can reach a static.
int dhcp_parse_test_bad_cookie(void);
int dhcp_parse_test_truncated_options(void);
int dhcp_parse_test_wrong_xid(void);

void dhcp_selftest(void);

#endif
