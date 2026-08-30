#include "thread_table.h"
#include "proc.h"
#include "../mm/heap.h"
#include "../serial.h"

void thread_table_init(struct thread_table *table) {
    if (!table) return;

    // Initialize all buckets
    for (int i = 0; i < THREAD_HASH_BUCKETS; i++) {
        spin_init(&table->buckets[i].lock, LOCK_RANK_THREAD, "thread_bucket");
        table->buckets[i].head = 0;
    }

    // Initialize thread count and TID allocator
    table->thread_count = 0;
    table->next_tid = 1;  // TID 0 reserved (for idle threads)
    spin_init(&table->count_lock, LOCK_RANK_SIGQUEUE, "thread_count");
}

int thread_table_alloc_tid(struct thread_table *table) {
    if (!table) return 0;

    uint64_t f = spin_lock_irqsave(&table->count_lock);

    if (table->next_tid >= MAX_THREADS_PER_PROC) {
        spin_unlock_irqrestore(&table->count_lock, f);
        return 0;  // Out of TID space
    }

    int tid = table->next_tid++;
    spin_unlock_irqrestore(&table->count_lock, f);

    return tid;
}

void thread_table_insert(struct thread_table *table, int tid, struct thread *t) {
    if (!table || !t || tid <= 0) {
        return;
    }

    unsigned bucket = thread_hash(tid);
    struct thread_bucket *b = &table->buckets[bucket];

    uint64_t f = spin_lock_irqsave(&b->lock);

    // Insert at head of bucket chain
    t->tid_next_hash = b->head;
    rcu_assign_pointer(b->head, t);

    // Update thread count
    uint64_t f2 = spin_lock_irqsave(&table->count_lock);
    table->thread_count++;
    spin_unlock_irqrestore(&table->count_lock, f2);

    spin_unlock_irqrestore(&b->lock, f);
}

struct thread *thread_table_lookup(struct thread_table *table, int tid) {
    if (!table || tid <= 0) {
        return 0;
    }

    unsigned bucket = thread_hash(tid);
    struct thread_bucket *b = &table->buckets[bucket];

    // RCU read-side: No lock needed
    struct thread *t = rcu_dereference(b->head);

    while (t) {
        if (t->tid == tid) {
            return t;
        }
        t = rcu_dereference(t->tid_next_hash);
    }

    return 0;
}

/*
 * Callback invoked after RCU grace period
 * Completes thread destruction (free struct and resources)
 */
static void thread_rcu_free(struct rcu_head *rh) {
    struct thread *t = container_of(rh, struct thread, rcu);

    // Free thread structure and extended state
    if (t->xstate) {
        kfree(t->xstate);
    }
    kfree(t);
}

void thread_table_remove(struct thread_table *table, struct thread *t) {
    if (!table || !t || t->tid <= 0) {
        return;
    }

    unsigned bucket = thread_hash(t->tid);
    struct thread_bucket *b = &table->buckets[bucket];

    uint64_t f = spin_lock_irqsave(&b->lock);

    // Find and unlink from bucket
    struct thread **link = &b->head;
    while (*link && *link != t) {
        link = &(*link)->tid_next_hash;
    }

    if (*link == t) {
        *link = t->tid_next_hash;

        // Update thread count
        uint64_t f2 = spin_lock_irqsave(&table->count_lock);
        table->thread_count--;
        spin_unlock_irqrestore(&table->count_lock, f2);

        // Defer destruction until RCU grace period
        spin_unlock_irqrestore(&b->lock, f);
        call_rcu(&t->rcu, thread_rcu_free);
    } else {
        spin_unlock_irqrestore(&b->lock, f);
    }
}
