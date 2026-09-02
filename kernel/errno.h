#ifndef NEOOS_KERNEL_ERRNO_H
#define NEOOS_KERNEL_ERRNO_H

// Kernel-side mirror of lib/include/errno.h's numeric values (the two
// trees don't share headers, so these are duplicated, not included --
// both use real Linux errno numbers for familiarity, with no need for
// binary compatibility with anything).

#define EPERM   1
#define ENXIO   6
#define E2BIG   7
#define EINTR  4
#define ESRCH   3
#define EAGAIN  11
#define EDEADLK 35
#define ENAMETOOLONG 36
#define ERANGE  34
#define ENOMEM  12
#define ECHILD  10
#define ETIMEDOUT 110
#define ENOSYS  38
#define ENOENT  2
#define EBADF   9
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOSPC  28
#define EIO     5
#define ENOTTY  25
#define EFAULT  14
#define EPIPE   32
#define ENOBUFS 105
#define EAFNOSUPPORT 97
#define EPROTONOSUPPORT 93
#define EADDRINUSE 98
#define ECONNREFUSED 111
#define ENOTCONN 107
#define EISCONN 106
#define EMSGSIZE 90
#define EOVERFLOW 75
#define ESPIPE  29
#define ENETUNREACH 101
#define EADDRNOTAVAIL 99
#define EDESTADDRREQ 89
#define ENOTSOCK 88

#endif
