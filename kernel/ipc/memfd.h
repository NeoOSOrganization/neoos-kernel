#ifndef NEOOS_MEMFD_H
#define NEOOS_MEMFD_H

#include <stdint.h>

// memfd_create(name, flags) -- an anonymous shared memory object,
// addressed by a file descriptor and by nothing else.
//
// This exists for the compositor. A client sizes one with ftruncate,
// maps it MAP_SHARED, draws into it and passes the DESCRIPTOR to the
// window manager over a unix socket; the manager maps the same object
// and composites from it. Nothing is named in any filesystem, so there
// is no stale entry to clean up after a crash and no way for an
// unrelated process to open another application's surface.

#define MFD_CLOEXEC        0x0001   // Linux values
#define MFD_ALLOW_SEALING  0x0002

// Returns a new fd, or a negative errno.
int64_t memfd_create_fd(const char *name, uint64_t name_len, unsigned flags);

// Live objects, for leak checks: SCM_RIGHTS passing takes references on
// these, and a socket closed with an undelivered message is exactly the
// case that leaks one.
uint64_t memfd_live_count(void);

void memfd_selftest(void);

#endif
