#ifndef NEOOS_NETINET_IN_H
#define NEOOS_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17
#define IPPROTO_TCP  6    // named for completeness; no TCP implementation

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7F000001u   // host order, as on Linux

// Linux's x86-64 layout, byte for byte: 16 bytes, family at 0, port at
// 2, address at 4, eight bytes of padding. It crosses the syscall
// boundary, so the layout is ABI -- see docs/abi-compatibility.md.
struct in_addr {
    uint32_t s_addr;      // NETWORK byte order
};

struct sockaddr_in {
    uint16_t       sin_family;
    uint16_t       sin_port;   // NETWORK byte order
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

#endif
