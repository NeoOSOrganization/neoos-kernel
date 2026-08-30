#ifndef NEOOS_FD_TABLE_H
#define NEOOS_FD_TABLE_H

#include <stdint.h>
#include "../lock.h"

/*
 * 2-level sparse file descriptor table
 *
 * Scales from 16 FDs to 10,000+ FDs per process efficiently.
 * Uses lazy allocation: only allocates level-2 slots when needed.
 *
 * Level 1: 32 buckets (always allocated)
 * Level 2: 512-entry slots per bucket (allocated on demand)
 * Max FDs: 32 * 512 = 16,384
 *
 * Lookup: bucket = fd / 512, slot = fd % 512
 * All accesses are O(1) average case.
 *
 * Lock hierarchy:
 *   bucket_lock[i] protects buckets[i]->slots
 *   Rank: LOCK_RANK_FDTABLE (acquired before thread_table locks)
 */

#define FD_TABLE_BUCKETS       32     // Level 1: bucket count
#define FD_TABLE_SLOTS         512    // Level 2: slots per bucket
#define FD_TABLE_MAX           (FD_TABLE_BUCKETS * FD_TABLE_SLOTS)  // 16,384

// Forward declare file_descriptor (defined in proc.h)
struct file_descriptor;

// Level 2: slots for one bucket
struct fd_bucket {
    struct spinlock lock;
    struct file_descriptor *slots;  // NULL until first use, then 512-entry array
    int slot_count;                 // count of in-use FDs in this bucket
};

// Level 1: the table itself
struct fd_table {
    struct fd_bucket buckets[FD_TABLE_BUCKETS];
    int fd_count;           // total open FDs across all buckets
    struct spinlock count_lock;
    int next_alloc;         // hint for alloc_fd (bucket index)
};

// Forward declare vnode for use in callbacks
struct vnode;
struct process;

// Initialize an fd_table for a process
void fd_table_init(struct fd_table *table);

// Get a file descriptor entry (returns NULL if out of range or not in use)
struct file_descriptor *fd_table_get(struct fd_table *table, int fd);

// Allocate the next available FD >= 3 (stdin/stdout/stderr reserved)
// Returns FD number, or -EMFILE if table full
int fd_table_alloc(struct fd_table *table);

// Mark FD as unused and release vnode reference if held
void fd_table_close(struct fd_table *table, int fd);

// Direct FD setter (used during process startup for FDs 0-2)
// WARNING: Must NOT be called while holding other locks
int fd_table_put(struct fd_table *table, int fd, struct vnode *vn, int writable);

// Free all FD entries and allocated buckets (during process cleanup)
void fd_table_free(struct fd_table *table);

// Copy FD table from parent to child (used in fork)
int fd_table_dup(struct fd_table *dst, struct fd_table *src);

#endif
