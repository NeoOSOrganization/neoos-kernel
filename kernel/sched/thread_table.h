#ifndef NEOOS_THREAD_TABLE_H
#define NEOOS_THREAD_TABLE_H

#include <stdint.h>
#include "sync/lock.h"

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
// MAX_THREADS_PER_PROC lives in sched/proc.h, which owns the thread
// stack layout the number actually comes from. It was DUPLICATED here
// at 16, and the duplicate silently won wherever this header was
// included first -- so raising the real one had no effect there.

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

// Insert thread into table
void thread_table_insert(struct thread_table *table, int tid, struct thread *t);

// Remove thread from table (takes bucket lock, uses RCU for cleanup)
void thread_table_remove(struct thread_table *table, struct thread *t);

#endif
