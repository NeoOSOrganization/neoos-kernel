#ifndef NEOOS_SOCKET_H
#define NEOOS_SOCKET_H

#include <stdint.h>

// Linux's values for everything a program passes in or reads back.
#define AF_UNSPEC 0
#define AF_INET   2

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

// accept4/socket flags, and shutdown's `how`. Linux's values.
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

// Socket option levels and names. Linux's values, because a program
// compiled against its own <sys/socket.h> passes these as integers and
// no shim can retrofit a number.
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define TCP_NODELAY  1

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7F000001u   // host order; 127.0.0.1

// Linux's x86-64 layouts, byte for byte. These cross the syscall
// boundary, so the field order, the sizes and the padding are all part
// of the ABI and cannot be reshaped -- see docs/abi-compatibility.md.
struct k_sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} __attribute__((packed));

struct k_in_addr {
    uint32_t s_addr;        // network order
} __attribute__((packed));

struct k_sockaddr_in {
    uint16_t         sin_family;
    uint16_t         sin_port;   // network order
    struct k_in_addr sin_addr;
    uint8_t          sin_zero[8];
} __attribute__((packed));

struct file_descriptor;
struct file_ops;

// The syscall entry points. Each returns 0 or a count on success and a
// negative errno on failure, as every other NeoOS syscall does.
int64_t socket_create(int domain, int type, int protocol);
int64_t socket_bind(int fd, const struct k_sockaddr *addr, uint32_t len);
int64_t socket_connect(int fd, const struct k_sockaddr *addr, uint32_t len);
int64_t socket_getsockname(int fd, struct k_sockaddr *addr, uint32_t *len);
int64_t socket_sendto(int fd, const void *buf, uint64_t len, int flags,
                      const struct k_sockaddr *dest, uint32_t dest_len);
int64_t socket_recvfrom(int fd, void *buf, uint64_t len, int flags,
                        struct k_sockaddr *src, uint32_t *src_len);
int64_t socket_listen(int fd, int backlog);
int64_t socket_accept4(int fd, struct k_sockaddr *addr, uint32_t *len, int flags);
int64_t socket_shutdown(int fd, int how);
int64_t socket_getpeername(int fd, struct k_sockaddr *addr, uint32_t *len);
int64_t socket_setsockopt(int fd, int level, int opt, const void *val, uint32_t len);
int64_t socket_getsockopt(int fd, int level, int opt, void *val, uint32_t *len);

const struct file_ops *socket_file_ops(void);

void socket_init(void);
void socket_selftest(void);

#endif
