#ifndef NEOOS_ARPA_INET_H
#define NEOOS_ARPA_INET_H

#include <stdint.h>
#include <netinet/in.h>

// Host/network byte order. x86 is little-endian and the wire is
// big-endian, so all four of these are real conversions here rather
// than the no-ops they are on a big-endian machine.
uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);

// Dotted-quad to a network-order address. Returns 0xFFFFFFFF on a
// malformed string, which is inet_addr's documented (and famously
// awkward) way of failing -- 255.255.255.255 is indistinguishable from
// an error. Kept because that IS the interface.
uint32_t inet_addr(const char *s);

// Writes a dotted quad for `addr` (network order) into `out`, which
// must have room for 16 bytes. Returns `out`. This is inet_ntop's
// shape rather than inet_ntoa's static buffer, which is not thread-safe
// and which NeoOS has no reason to reproduce.
char *inet_ntoa_r(uint32_t addr_n, char *out);

#endif
