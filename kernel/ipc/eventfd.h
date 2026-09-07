#ifndef NEOOS_EVENTFD_H
#define NEOOS_EVENTFD_H

#include <stdint.h>

// eventfd2(initval, flags) -- Linux's counter-in-an-fd wake primitive
// (libuv/Node, .NET SocketAsyncEngine, tokio, Go's netpoller fallback).
// An 8-byte counter: read drains it (or subtracts 1 in EFD_SEMAPHORE
// mode), write adds to it, poll reports POLLIN when non-zero and
// POLLOUT when a further add of 1 would not overflow.
//
// Linux flag values.
#define EFD_SEMAPHORE 0x00000001
#define EFD_CLOEXEC   0x00080000
#define EFD_NONBLOCK  0x00000800

// Creates an eventfd and installs it in the current process's fd table.
// Returns the new fd, or a negative errno.
int  eventfd_create(unsigned int initval, int flags);

void eventfd_selftest(void);   // "[eventfd] selftest ..."

#endif
