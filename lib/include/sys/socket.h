#ifndef NEOOS_SYS_SOCKET_H
#define NEOOS_SYS_SOCKET_H

#include <stdint.h>

typedef uint32_t socklen_t;

#define AF_UNSPEC 0
#define AF_INET   2
#define PF_INET   AF_INET

#define SOCK_STREAM 1   // accepted by the header, refused by the kernel: no TCP yet
#define SOCK_DGRAM  2

// Deliberately NO MSG_* constants. Every flag NeoOS could name --
// MSG_PEEK, MSG_DONTWAIT, MSG_TRUNC, MSG_WAITALL -- is unimplemented,
// and a constant for a flag the kernel ignores is a promise it does not
// keep. The `flags` argument exists so the signatures match POSIX, and
// must be 0.

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

// Creates a socket. Returns a file descriptor, or a negative errno.h
// code: -EAFNOSUPPORT for anything but AF_INET, -EPROTONOSUPPORT for
// anything but SOCK_DGRAM.
int socket(int domain, int type, int protocol);

int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);

// `flags` must be 0. A datagram longer than `len` is truncated and the
// remainder discarded, and the return value is what was delivered --
// there is no MSG_TRUNC to report the excess.
int64_t sendto(int fd, const void *buf, uint64_t len, int flags,
               const struct sockaddr *dest, socklen_t dest_len);
int64_t recvfrom(int fd, void *buf, uint64_t len, int flags,
                 struct sockaddr *src, socklen_t *src_len);

// send/recv are sendto/recvfrom with no address, and require a
// connected socket. So do read() and write() on a socket fd.
int64_t send(int fd, const void *buf, uint64_t len, int flags);
int64_t recv(int fd, void *buf, uint64_t len, int flags);

#endif
