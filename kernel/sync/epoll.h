#ifndef NEOOS_EPOLL_H
#define NEOOS_EPOLL_H

#include <stdint.h>
#include "sync/lock.h"

struct file_descriptor;
struct file_ops;
struct process;

// epoll(7)'s registration set, built on top of poll_core() rather than
// as a separate readiness mechanism: an epoll object is just a stored
// (fd, events, data) list that epoll_wait() turns into exactly the
// pollfd[] array poll()/select() already build, then calls the SAME
// core scan-and-sleep loop (kernel/syscall/sys_poll.c's poll_core).
// Found missing chasing a dotnet NativeAOT TCP socket example: .NET's
// SocketAsyncEngine uses epoll_create1/epoll_ctl/epoll_wait even for a
// single synchronous connect+send+recv. No edge-triggered (EPOLLET)
// support -- every registration is treated as level-triggered, which
// is a strict subset of Linux's semantics (a caller that only uses
// level-triggered mode, the default and by far the common case, sees
// no difference).
struct epoll_entry {
    int fd;
    uint32_t events;      // EPOLLIN/EPOLLOUT/... requested
    uint64_t data;         // epoll_data_t, opaque, returned unchanged
    struct epoll_entry *next;
};

struct epoll_obj {
    struct spinlock lock;
    struct epoll_entry *list;
    int nfds;
    int refs;
};

extern const struct file_ops epoll_file_ops;

// Creates a fresh epoll object and installs it on a new fd in the
// calling process. Returns the fd, or a negative errno.
int epoll_create(int flags);

// EPOLL_CTL_ADD/MOD/DEL against `epfd`'s registration list. `event` is
// NULL only for EPOLL_CTL_DEL (event is unused there, matching Linux).
int epoll_ctl_do(struct file_descriptor *epf, int op, int fd, uint32_t events, uint64_t data);

#endif
