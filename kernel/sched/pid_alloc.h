#ifndef NEOOS_PID_ALLOC_H
#define NEOOS_PID_ALLOC_H

#include <stdint.h>
#include "sync/lock.h"

/*
 * Process ID allocator using radix tree (IDR)
 *
 * Radix tree with 64-entry nodes supports efficient sparse allocation.
 * PIDs 0 = idle (reserved), 1+ = real processes.
 *
 * Complexity:
 *   Allocate: O(log n) tree walk + check free-list
 *   Lookup: O(log n) tree walk
 *   Free: O(log n) tree walk + add to free-list
 *
 * On single CPU with few processes, effectively O(1).
 * At 1M processes, ~4-5 tree levels.
 *
 * PID reuse: Free-list of recently-freed PIDs
 */

#define PID_RADIX_BITS 6       // 64 entries per node (2^6)
#define PID_RADIX_SIZE (1 << PID_RADIX_BITS)
#define MAX_PIDS (1 << 20)     // Support up to ~1M PIDs (2^20)

struct pid_radix_node {
    void *entries[PID_RADIX_SIZE];     // Pointers to processes or child nodes
    struct pid_radix_node *children[PID_RADIX_SIZE];
};

struct pid_free_entry {
    int pid;
    struct pid_free_entry *next;
};

struct pid_allocator {
    struct pid_radix_node *root;
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

// Allocate specific PID (for pid 0 = idle)
// Returns 0 on success, -1 if PID already in use
int pid_alloc_specific(struct pid_allocator *alloc, int pid);

// Free a PID (add to reuse free-list)
void pid_free(struct pid_allocator *alloc, int pid);

// Lookup process by PID in tree
// Returns pointer (void *) stored in tree, or NULL if not found
void *pid_lookup(struct pid_allocator *alloc, int pid);

// Insert process pointer for PID (called after pid_alloc)
// PID must have been allocated; stores the process pointer
void pid_insert(struct pid_allocator *alloc, int pid, void *process);

// Remove process pointer for PID (called before pid_free)
void pid_remove(struct pid_allocator *alloc, int pid);

#endif
