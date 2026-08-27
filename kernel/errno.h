#ifndef NEOOS_KERNEL_ERRNO_H
#define NEOOS_KERNEL_ERRNO_H

// Kernel-side mirror of lib/include/errno.h's numeric values (the two
// trees don't share headers, so these are duplicated, not included --
// both use real Linux errno numbers for familiarity, with no need for
// binary compatibility with anything).

#define EPERM   1
#define EINTR  4
#define ESRCH   3
#define EAGAIN  11
#define EDEADLK 35
#define ENOMEM  12
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

#endif
