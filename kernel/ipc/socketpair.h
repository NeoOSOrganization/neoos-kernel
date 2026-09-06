#ifndef NEOOS_SOCKETPAIR_H
#define NEOOS_SOCKETPAIR_H

// socketpair(2) -- AF_UNIX only, backed by two pipes (see socketpair.c
// and pipe.h's exposed _ep functions for why: a connected pair of
// local, bidirectional byte streams is exactly two of pipe.c's
// existing ring buffers, cross-wired, and reusing that proven code
// beats inventing new duplex-socket internals for a feature whose
// only real caller so far (curl's multi-handle wakeup mechanism) just
// needs a pair of fds it can write a byte into and poll() on.
//
// Divergence from Linux, recorded in docs/stdlib.md: no SCM_RIGHTS
// fd-passing and no genuine out-of-band data, matching the limits
// plain pipes already have on NeoOS.

// domain must be AF_UNIX; type is SOCK_STREAM or SOCK_DGRAM (both
// treated identically -- a pipe has no datagram framing, and nothing
// this milestone needs relies on message boundaries), optionally
// OR'd with SOCK_NONBLOCK/SOCK_CLOEXEC exactly like socket(2)'s own
// type argument. protocol must be 0. Fills fds[0]/fds[1] with the two
// connected ends and returns 0, or a negative errno.
int socketpair_create(int domain, int type, int protocol, int fds[2]);

void socketpair_selftest(void);

#endif
