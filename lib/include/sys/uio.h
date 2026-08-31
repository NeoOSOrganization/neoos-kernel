#ifndef NEOOS_SYS_UIO_H
#define NEOOS_SYS_UIO_H

#include <stdint.h>

// Linux's layout: a pointer and a length, in that order.
struct iovec {
    void    *iov_base;
    uint64_t iov_len;
};

// At most 16 vectors; more returns -EINVAL rather than being silently
// truncated. Linux's IOV_MAX is 1024.
#define IOV_MAX 16

// The terminal ioctls musl probes with. NeoOS answers -ENOTTY to both.
#define TIOCGWINSZ 0x5413
#define TCGETS     0x5401

int64_t writev(int fd, const struct iovec *iov, int iovcnt);
int64_t readv(int fd, const struct iovec *iov, int iovcnt);

#endif
