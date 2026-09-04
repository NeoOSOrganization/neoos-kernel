#ifndef NEOOS_SYS_SOCKET_H
#define NEOOS_SYS_SOCKET_H

#include <stdint.h>

typedef uint32_t socklen_t;

#define AF_UNSPEC 0
#define AF_INET   2
#define PF_INET   AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

// accept4/socket flags, and shutdown's `how`. Linux's values.
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

// Socket options. Linux's values, because a program compiled against
// its own headers passes these as plain integers.
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define IPPROTO_TCP  6
#define TCP_NODELAY  1

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

// SOCK_STREAM. See docs/stdlib.md for the three recorded divergences:
// a five-second MSL, SO_SNDBUF/SO_RCVBUF accepted but not honoured, and
// no SO_LINGER or SO_KEEPALIVE.
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int shutdown(int fd, int how);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len);

#endif
