#ifndef NEOOS_PROC_TABLE_H
#define NEOOS_PROC_TABLE_H

#include <stdint.h>
#include "sync/lock.h"
#include "sched/pid_alloc.h"

/*
 * Process hash table with per-bucket locking.
 *
 * The sole process store (proc_list is gone). O(1) average lookup.
 * Lookups take the bucket lock and return a ref'd pointer; removal
 * takes the same lock and drops the table's reference. Iteration is
 * proc_table_for_each_ref (batched, ref'd, no lock held in the
 * callback).
 *
 * Structure:
 *   Hash buckets protected by per-bucket spinlocks
 *   Readers use RCU (no lock needed for lookup)
 *   Writers take bucket lock, then use RCU for deferred cleanup
 *
 * Hash function: Mix PID with random seed for distribution
 */

#define PROC_HASH_BUCKETS 256    // 2^8 buckets; tunable for load factor

struct process;  // Forward declaration

struct proc_bucket {
    struct spinlock lock;
    struct process *head;        // Bucket chain (linked list)
};

struct proc_table {
    struct proc_bucket buckets[PROC_HASH_BUCKETS];
    int process_count;           // Approximate count (for stats)
    struct spinlock count_lock;  // Protect process_count
    struct pid_allocator pid_alloc;
    // proc_table_for_each_ref's generation counter. Bumped once per
    // iteration under iter_lock; each visited process is stamped with
    // the value so a multi-batch bucket scan does not double-visit.
    uint32_t iter_gen;
    struct spinlock iter_lock;   // LOCK_RANK_PROCTABLE
};

/*
 * Global process table (singleton)
 * Initialized once at kernel startup
 */
extern struct proc_table global_proc_table;

// Initialize process table and PID allocator
void proc_table_init(void);

// Hash function: PID -> bucket index
static inline unsigned proc_hash(int pid) {
    // Simple hash: PID modulo bucket count
    // Could add randomization for security later
    return (unsigned)pid % PROC_HASH_BUCKETS;
}

// Lookup process by PID. Takes the bucket lock and returns the process
// with a reference held (p->ref raised), or NULL. The caller MUST
// proc_put() the result.
struct process *proc_table_lookup(int pid);

// Allocate new PID and reserve entry (takes bucket lock)
// Returns new PID, or 0 on failure
int proc_table_alloc_pid(void);

// Insert process into table after allocation
// pid must have been allocated via proc_table_alloc_pid
void proc_table_insert(int pid, struct process *proc);

// Remove process from table (takes bucket lock, uses RCU for cleanup)
// Process freed after grace period
void proc_table_remove(struct process *proc);

// Calls fn(p, ctx) for every process present during the walk, with a
// reference held on p and NO lock held. fn must not insert into or
// remove from the table directly, and must not call
// proc_table_for_each_ref again (not reentrant). Iteration order is
// unspecified. A process created or removed mid-walk may or may not be
// visited; fn must re-check p->state.
void proc_table_for_each_ref(void (*fn)(struct process *p, void *ctx), void *ctx);

// Allocate PID 0 specifically (for idle process)

#endif
