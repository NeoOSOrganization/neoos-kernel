#ifndef NEOOS_THREAD_TABLE_H
#define NEOOS_THREAD_TABLE_H

#include <stdint.h>
#include "sync/lock.h"
#include "sync/rcu.h"

/*
 * Per-process thread hash table
 *
 * Replaces the old linked-list approach (thread->proc_next chains).
 * Provides O(1) average thread lookup by TID within a process.
 *
 * Structure (per process):
 *   Hash buckets protected by per-bucket spinlocks
 *   Readers use RCU (no lock needed for lookup)
 *   Writers take bucket lock, then use RCU for deferred cleanup
 *
 * TID allocation: Simple counter per process (0..MAX_THREADS_PER_PROC)
 */

#define THREAD_HASH_BUCKETS 16    // Per-process; tunable
#define MAX_THREADS_PER_PROC 16   // Current hard limit (Phase 2 will increase)

struct thread;
struct process;

struct thread_bucket {
    struct spinlock lock;
    struct thread *head;           // Bucket chain (linked list)
};

struct thread_table {
    struct thread_bucket buckets[THREAD_HASH_BUCKETS];
    int thread_count;              // Approximate count
    int next_tid;                  // Next TID to allocate
    struct spinlock count_lock;
};

// Initialize thread table for a process
void thread_table_init(struct thread_table *table);

// Hash function: TID -> bucket index
static inline unsigned thread_hash(int tid) {
    return (unsigned)tid % THREAD_HASH_BUCKETS;
}

// Allocate new TID
// Returns new TID >= 1, or 0 on failure (max threads reached)
int thread_table_alloc_tid(struct thread_table *table);

// Insert thread into table
void thread_table_insert(struct thread_table *table, int tid, struct thread *t);

// Lookup thread by TID (RCU-protected, no lock needed)
struct thread *thread_table_lookup(struct thread_table *table, int tid);

// Remove thread from table (takes bucket lock, uses RCU for cleanup)
void thread_table_remove(struct thread_table *table, struct thread *t);

#endif
