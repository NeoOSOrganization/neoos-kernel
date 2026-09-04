#ifndef NEOOS_DNSPROBE_H
#define NEOOS_DNSPROBE_H

// NOT a resolver. Thirty hardcoded bytes and an assertion on the
// response header.
//
// It exists because it is the cheapest honest proof that a UDP datagram
// left this machine and an answer came back: DHCP proves the same wire,
// but through a path with a broadcast, a server on the same host stack,
// and a reply the client half-expects. A query to slirp's DNS at
// 10.0.2.3 is an ordinary unicast round trip through the route table,
// the ARP cache and the link layer, and nothing about it is special.

void dns_probe_selftest(void);

#endif
