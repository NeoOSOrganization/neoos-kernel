#ifndef NEOOS_ERRNO_H
#define NEOOS_ERRNO_H

// Every open/read/write/close/lseek/mkdir/unlink call in this library
// returns its negative error code directly, e.g. open() on a missing
// path returns -ENOENT -- there is no separate settable errno
// variable. spawn/wait/getpid are unaffected and keep their existing
// plain -1-on-failure convention.

#define EPERM   1
#define E2BIG   7
#define EINTR   4
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
