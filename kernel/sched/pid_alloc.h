#ifndef NEOOS_PID_ALLOC_H
#define NEOOS_PID_ALLOC_H

#include <stdint.h>
#include "sync/lock.h"

/*
 * PID *number* allocator: a free list of recently-freed ids over a
 * rising next_pid. It does not map pids to processes -- that is
 * proc_table_lookup()'s bucketed hash, which has per-bucket locks and
 * hands back a reference.
 *
 * A radix tree (insert/remove/lookup) lived here until CS2. Nothing
 * ever called it, so it was deleted rather than fixed; see pid_alloc.c.
 *
 * PID 0 is reserved for idle, 1+ for real processes. Allocation does
 * not yet wrap around at MAX_PIDS -- CS4 owns that.
 */

#define MAX_PIDS (1 << 20)     // Support up to ~1M PIDs (2^20)

struct pid_free_entry {
    int pid;
    struct pid_free_entry *next;
};

struct pid_allocator {
    struct pid_free_entry *free_list;
    int next_pid;                      // Hint for next allocation
    struct spinlock lock;
};

// Initialize PID allocator
// Must be called before any PID allocation
void pid_allocator_init(struct pid_allocator *alloc);

// Allocate a new PID
// Returns 0 on failure (out of memory), else a new unique PID
int pid_alloc(struct pid_allocator *alloc);

// Free a PID (add to reuse free-list)
void pid_free(struct pid_allocator *alloc, int pid);

void pid_alloc_selftest(void);

#endif
