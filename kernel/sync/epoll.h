#ifndef NEOOS_EPOLL_H
#define NEOOS_EPOLL_H

#include <stdint.h>
#include "sync/lock.h"

struct file_descriptor;
struct file_ops;
struct process;
struct fd_table;

// epoll(7)'s registration set, built on top of poll_core() rather than
// as a separate readiness mechanism: an epoll object is just a stored
// (fd, events, data) list that epoll_wait() turns into exactly the
// pollfd[] array poll()/select() already build, then calls the SAME
// core scan-and-sleep loop (kernel/syscall/sys_poll.c's poll_core).
// Found missing chasing a dotnet NativeAOT TCP socket example: .NET's
// SocketAsyncEngine uses epoll_create1/epoll_ctl/epoll_wait even for a
// single synchronous connect+send+recv.
//
// EPOLLET is honoured (see below). It was not, at first, on the theory
// that level-triggered is a strict superset -- more notifications,
// never fewer -- and therefore safe for any caller. That is wrong for a
// caller WRITTEN against edge-triggered. SocketAsyncEngine's loop is
// "epoll_wait -> hand the fd to the thread pool -> straight back to
// epoll_wait", and it is correct only because ET does not report the
// same readiness twice: under LT the work item has not run yet, the fd
// is still ready, and the engine dispatches it again and again until
// the pool is full of duplicates and nothing is served. That was the
// ASP.NET Core concurrency hang.
#define EPOLLET       0x80000000u
// The bits of an events mask that ARE poll(2)'s, one for one (EPOLLIN
// == POLLIN, and so on up through EPOLLRDHUP). Everything above is an
// epoll-only mode flag and must be stripped before the mask reaches
// poll_core, which reads it as a 16-bit short.
#define EPOLL_POLL_BITS 0x0000ffffu

struct epoll_entry {
    int fd;
    uint32_t events;      // EPOLLIN/EPOLLOUT/... requested
    uint64_t data;         // epoll_data_t, opaque, returned unchanged

    // Edge-triggered bookkeeping, meaningless unless events has EPOLLET.
    // last_ready is the readiness already reported to userland and not
    // yet re-armed; last_seq is the object's readiness counter
    // (file_ready_seq) as of that report, which is what distinguishes
    // "still ready, already told you" from "went away and came back".
    // have_seq is 0 until the first scan, and stays 0 for an object that
    // offers no counter at all (then the registration degrades to
    // level-triggered rather than risking a missed edge).
    uint32_t last_ready;
    uint64_t last_seq;
    int      have_seq;
    // Bumped on ADD and on MOD. An epoll_wait that started before a
    // re-registration must not write its stale edge state back onto the
    // new one -- and a reused fd number would otherwise inherit a
    // consumed edge and never be reported again.
    uint32_t gen;

    struct epoll_entry *next;
};

struct epoll_obj {
    struct spinlock lock;
    struct epoll_entry *list;
    int nfds;
    int refs;
    uint32_t gen_ctr;          // source of epoll_entry.gen
    struct fd_table *owner;    // the process fd table this epoll belongs to
    struct epoll_obj *g_next;  // global chain, for epoll_forget_fd()
};

extern const struct file_ops epoll_file_ops;

// Creates a fresh epoll object and installs it on a new fd in the
// calling process. Returns the fd, or a negative errno.
int epoll_create(int flags);

// EPOLL_CTL_ADD/MOD/DEL against `epfd`'s registration list. `event` is
// NULL only for EPOLL_CTL_DEL (event is unused there, matching Linux).
int epoll_ctl_do(struct file_descriptor *epf, int op, int fd, uint32_t events, uint64_t data);

// Drop every registration for `fd` from every epoll object owned by
// `owner`. Called from fd_table_close() so that closing a descriptor
// removes it from any epoll set automatically -- Linux semantics that
// .NET's SocketAsyncEngine relies on (it never EPOLL_CTL_DELs a socket
// it is about to close, and reuses the freed fd number for the next
// connection).
void epoll_forget_fd(struct fd_table *owner, int fd);

#endif
