#ifndef NEOOS_POLL_HEAD_H
#define NEOOS_POLL_HEAD_H

#include <stdint.h>
#include "sync/lock.h"

/*
 * Per-object poll registration (CS5.2).
 *
 * Before this, every poll/select caller slept on ONE global broadcast
 * queue, and ANY readiness change anywhere woke all of them to re-scan.
 * A poller interested in nothing that just happened was woken anyway.
 * Measured over a whole boot: 5894 broadcasts, 276 poll wakeups, 266 of
 * which found nothing ready -- 96% pure waste.
 *
 * A poll head is a list of the pollers currently interested in ONE
 * object. poll_core registers on the head of each fd it was asked
 * about, and readiness on that object wakes only those registrations.
 * It is Linux's poll_wait() split, with the object's own waitq left
 * alone -- blocking read/write still uses that, unchanged.
 *
 * Registrations live on POLL_CORE'S STACK, for exactly as long as the
 * poll call: there is nothing to allocate and nothing to leak, and the
 * unregister on the way out is what keeps a freed frame off the list.
 * That is also why poll_core must unregister on EVERY exit path,
 * including -EINTR.
 *
 * An object with no poll head is not broken, only unconverted: poll_core
 * falls back to the global broadcast for that fd, and asks to be woken
 * by it. The broadcast therefore shrinks as objects are converted
 * rather than being replaced in one step.
 */

struct thread;

struct poll_head {
    struct spinlock  lock;
    struct poll_reg *list;
};

struct poll_reg {
    struct poll_head *head;
    struct thread    *t;
    struct poll_reg  *next;
};

void poll_head_init(struct poll_head *h, const char *name);

// A single shared head that is never notified, for objects that are
// ALWAYS ready -- a regular file, /dev/null, the framebuffer. Their
// readiness cannot change, so there is nothing to be woken about, and
// without this they would keep their poller on the global broadcast
// waiting for an event that can never concern them.
struct poll_head *poll_head_always_ready(void);

// Adds `r` to `h`'s list on behalf of `t`. `r` must outlive the
// registration -- poll_core's stack frame does.
void poll_head_register(struct poll_head *h, struct poll_reg *r, struct thread *t);

// Removes a registration. Safe to call on one that was never registered
// (r->head == 0), which is what lets poll_core unregister a whole array
// without tracking how far it got.
void poll_head_unregister(struct poll_reg *r);

// This object became ready: wake every poller registered on it, and
// nobody else.
void poll_head_notify(struct poll_head *h);

#endif
