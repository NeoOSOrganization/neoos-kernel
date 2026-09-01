#ifndef NEOOS_FD_TABLE_H
#define NEOOS_FD_TABLE_H

#include <stdint.h>
#include "sync/lock.h"

/*
 * 2-level sparse file descriptor table
 *
 * Replaces the flat 16-entry files[] array that used to live in
 * struct process. Scales to 16,384 fds per process without paying for
 * them up front: only the level-2 arrays a process actually reaches
 * are allocated.
 *
 * Level 1: 32 buckets (inline in the table, always present)
 * Level 2: 512 slots per bucket (allocated on first use)
 * Max fds: 32 * 512 = 16,384
 *
 * Lookup: bucket = fd / 512, slot = fd % 512. O(1).
 *
 * Locking: buckets[i].lock protects buckets[i], rank LOCK_RANK_FDTABLE.
 * Reads (fd_table_get) are lockless -- a slot array is installed once
 * and never swapped while the process lives.
 */

#define FD_TABLE_BUCKETS       32     // Level 1: bucket count
#define FD_TABLE_SLOTS         512    // Level 2: slots per bucket
#define FD_TABLE_MAX           (FD_TABLE_BUCKETS * FD_TABLE_SLOTS)  // 16,384

// fds 0/1/2 are opened on /dev/CONSOLE at process creation and are
// never handed out by fd_table_alloc.
#define FD_STDIO_COUNT         3

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
    int next_alloc;         // hint: bucket index to start the next scan at
};

// Forward declare vnode for use in callbacks
struct vnode;
struct process;

// Initialize an fd_table for a process
void fd_table_init(struct fd_table *table);

// Get a file descriptor entry (returns NULL if out of range or not in use)
struct file_descriptor *fd_table_get(struct fd_table *table, int fd);

// Reserve the lowest available fd >= 3 (stdin/stdout/stderr excluded).
// The slot is marked in-use with a NULL vnode; the caller fills it in,
// or hands it back with fd_table_close on failure.
// Returns the fd number, or -EMFILE if the table is full.
int fd_table_alloc(struct fd_table *table);

// Release an fd, dropping its vnode reference if it holds one.
void fd_table_close(struct fd_table *table, int fd);

// Direct fd setter (used during process startup for fds 0-2).
// Returns 1 if the slot was placed, 0 if it was taken or on OOM.
int fd_table_put(struct fd_table *table, int fd, struct vnode *vn, int writable);

// Close every fd and free the level-2 arrays. Leaves the table itself
// valid (and empty) so the caller can free it separately.
void fd_table_free(struct fd_table *table);

// Copy a parent's table into a freshly initialised child table (fork).
// Returns 1 on success, 0 on OOM.
int fd_table_dup(struct fd_table *dst, struct fd_table *src);

// dup2/dup3 primitive: point newfd at oldfd's open object, closing
// whatever newfd held. WILL target fds 0/1/2. Returns newfd or -errno.
int fd_table_dup2(struct fd_table *table, int oldfd, int newfd);

#endif
