#include "sched/thread_table.h"
#include "sched/proc.h"
#include "mm/heap.h"
#include "dev/serial.h"

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
    __atomic_store_n(&b->head, t, __ATOMIC_RELEASE);

    // Update thread count
    uint64_t f2 = spin_lock_irqsave(&table->count_lock);
    table->thread_count++;
    spin_unlock_irqrestore(&table->count_lock, f2);

    spin_unlock_irqrestore(&b->lock, f);
}

// Lookup by tid under the bucket lock. NOTE: does NOT take a reference
// -- today the only caller path that matters (thread_join) walks the
// legacy p->threads list under p->lock instead, and this table is a
// secondary index. If a caller ever needs the result past p->lock it
// must thread_get() it here first.
struct thread *thread_table_lookup(struct thread_table *table, int tid) {
    if (!table || tid <= 0) {
        return 0;
    }

    struct thread_bucket *b = &table->buckets[thread_hash(tid)];
    uint64_t f = spin_lock_irqsave(&b->lock);
    for (struct thread *t = b->head; t; t = t->tid_next_hash) {
        if (t->tid == tid) { spin_unlock_irqrestore(&b->lock, f); return t; }
    }
    spin_unlock_irqrestore(&b->lock, f);
    return 0;
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

        spin_unlock_irqrestore(&b->lock, f);
        // No free here: the thread struct is owned by the legacy
        // p->threads / p->zombies list reference and freed by
        // thread_put() when that list drops it. This table is just a
        // secondary index; unlinking the bucket is all that is needed.
    } else {
        spin_unlock_irqrestore(&b->lock, f);
    }
}
