#ifndef NEOOS_PID_ALLOC_H
#define NEOOS_PID_ALLOC_H

#include <stdint.h>
#include "sync/lock.h"

/*
 * PID *number* allocator: a bitmap of the whole pid space with a rising
 * cursor over it. It does not map pids to processes -- that is
 * proc_table_lookup()'s bucketed hash, which has per-bucket locks and
 * hands back a reference.
 *
 * A radix tree (insert/remove/lookup) lived here until CS2. Nothing
 * ever called it, so it was deleted rather than fixed; see pid_alloc.c.
 *
 * PID 0 is reserved for idle, 1+ for real processes.
 *
 * The cursor RISES and wraps; freed pids are not reused until it comes
 * back round to them. That is deliberate on two counts. It is what makes
 * wraparound work at all -- the previous allocator simply stopped, and
 * returned 0 for every fork after the millionth process. And it is what
 * puts distance between a pid's death and its rebirth: the free list it
 * replaced was LIFO, so the pid freed most recently was the very next
 * one handed out, and a signal aimed at a process that had just exited
 * landed on an unrelated new one. userland/sigstorm.c has a real
 * instance of that, caught in a test run.
 */

#define MAX_PIDS   (1 << 20)             // ~1M pids
#define PID_WORDS  (MAX_PIDS / 64)       // 16,384 words = 128 KiB

struct pid_allocator {
    // One bit per pid; 1 = in use. Static rather than heap: allocation
    // must not be able to fail on the exit path, and the old free list
    // kmalloc'd an entry per pid_free and silently LEAKED the pid when
    // that allocation failed -- which is exactly when a machine can
    // least afford to lose pids.
    uint64_t bitmap[PID_WORDS];
    int      cursor;                     // next pid to try; rises and wraps
    int      wrapped;                    // has the cursor been round at least once
    struct spinlock lock;
};

// Initialize PID allocator
// Must be called before any PID allocation
void pid_allocator_init(struct pid_allocator *alloc);

// Allocate a PID. Returns 0 only when every pid in the space is in use.
int pid_alloc(struct pid_allocator *alloc);

// Release a PID. Cannot fail.
void pid_free(struct pid_allocator *alloc, int pid);

// 1 if the cursor has wrapped at least once, i.e. pids are being reused.
int pid_alloc_has_wrapped(struct pid_allocator *alloc);

void pid_alloc_selftest(void);

#endif
