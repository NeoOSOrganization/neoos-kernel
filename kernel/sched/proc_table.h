#ifndef NEOOS_PROC_TABLE_H
#define NEOOS_PROC_TABLE_H

#include <stdint.h>
#include "sync/lock.h"
#include "sync/rcu.h"
#include "sched/pid_alloc.h"

/*
 * Process hash table with per-bucket locking and RCU protection
 *
 * Replaces the old global proc_list linked list.
 * Provides O(1) average process lookup with minimal contention.
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

// Lookup process by PID (RCU-protected, no lock needed)
// Returns pointer to process struct, or NULL if not found
// Caller must use rcu_read_lock()/rcu_read_unlock() if needed
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

// Allocate PID 0 specifically (for idle process)
int proc_table_alloc_pid_zero(void);

#endif
