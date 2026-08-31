#include "sched/proc_table.h"
#include "sched/proc.h"
#include "mm/heap.h"
#include "dev/serial.h"

struct proc_table global_proc_table;

void proc_table_init(void) {
    // Initialize all buckets
    for (int i = 0; i < PROC_HASH_BUCKETS; i++) {
        spin_init(&global_proc_table.buckets[i].lock, LOCK_RANK_PROCTABLE, "proc_bucket");
        global_proc_table.buckets[i].head = 0;
    }

    // Initialize process count (higher rank: acquired after bucket locks)
    global_proc_table.process_count = 0;
    spin_init(&global_proc_table.count_lock, LOCK_RANK_PROCESS, "proc_count");

    // Initialize PID allocator
    pid_allocator_init(&global_proc_table.pid_alloc);

    global_proc_table.iter_gen = 0;
    spin_init(&global_proc_table.iter_lock, LOCK_RANK_PROCTABLE, "proc_iter");

    serial_write_string("[proc_table] initialized with ");
    serial_write_hex64(PROC_HASH_BUCKETS);
    serial_write_string(" buckets\n");
}

// Returns the process with a reference held (p->ref raised), or NULL.
// The caller must proc_put() the result. Holding the bucket lock across
// the proc_get() is what makes this safe against a concurrent
// proc_reap()/proc_put() freeing the struct: proc_table_remove() takes
// the same bucket lock to unlink, so it cannot be mid-free here.
struct process *proc_table_lookup(int pid) {
    if (pid <= 0) {
        return 0;
    }

    struct proc_bucket *b = &global_proc_table.buckets[proc_hash(pid)];
    uint64_t f = spin_lock_irqsave(&b->lock);
    for (struct process *p = b->head; p; p = p->proc_next_hash) {
        if (p->pid == pid) {
            proc_get(p);
            spin_unlock_irqrestore(&b->lock, f);
            return p;
        }
    }
    spin_unlock_irqrestore(&b->lock, f);
    return 0;
}

int proc_table_alloc_pid(void) {
    return pid_alloc(&global_proc_table.pid_alloc);
}

void proc_table_insert(int pid, struct process *proc) {
    if (pid <= 0 || !proc) {
        return;
    }

    unsigned bucket = proc_hash(pid);
    struct proc_bucket *b = &global_proc_table.buckets[bucket];

    uint64_t f = spin_lock_irqsave(&b->lock);

    // Insert at head of bucket chain
    proc->proc_next_hash = b->head;
    __atomic_store_n(&b->head, proc, __ATOMIC_RELEASE);

    // Update process count
    uint64_t f2 = spin_lock_irqsave(&global_proc_table.count_lock);
    global_proc_table.process_count++;
    spin_unlock_irqrestore(&global_proc_table.count_lock, f2);

    spin_unlock_irqrestore(&b->lock, f);
}

// Unlinks proc from its bucket and drops the process table's reference
// on it (proc_put frees the struct + pid when it was the last one). A
// lookup that is mid-flight holds its own reference and does the real
// free. Safe against proc_table_lookup because both take the bucket
// lock.
void proc_table_remove(struct process *proc) {
    if (!proc || proc->pid <= 0) {
        return;
    }

    unsigned bucket = proc_hash(proc->pid);
    struct proc_bucket *b = &global_proc_table.buckets[bucket];

    uint64_t f = spin_lock_irqsave(&b->lock);

    // Find and unlink from bucket
    struct process **link = &b->head;
    while (*link && *link != proc) {
        link = &(*link)->proc_next_hash;
    }

    if (*link == proc) {
        *link = proc->proc_next_hash;

        // Update process count
        uint64_t f2 = spin_lock_irqsave(&global_proc_table.count_lock);
        global_proc_table.process_count--;
        spin_unlock_irqrestore(&global_proc_table.count_lock, f2);

        spin_unlock_irqrestore(&b->lock, f);
        proc_put(proc);   // drops the table's reference
    } else {
        spin_unlock_irqrestore(&b->lock, f);
    }
}

void proc_table_for_each_ref(void (*fn)(struct process *, void *), void *ctx) {
    uint64_t gf = spin_lock_irqsave(&global_proc_table.iter_lock);
    uint32_t gen = ++global_proc_table.iter_gen;
    spin_unlock_irqrestore(&global_proc_table.iter_lock, gf);

    for (int i = 0; i < PROC_HASH_BUCKETS; i++) {
        struct proc_bucket *b = &global_proc_table.buckets[i];
        for (;;) {
            struct process *batch[8];
            int n = 0;
            uint64_t f = spin_lock_irqsave(&b->lock);
            for (struct process *p = b->head; p && n < 8; p = p->proc_next_hash) {
                if (p->iter_gen == gen) { continue; }
                p->iter_gen = gen;
                proc_get(p);
                batch[n++] = p;
            }
            spin_unlock_irqrestore(&b->lock, f);

            // fn runs with NO lock held: it may take p->lock, deliver a
            // signal, or reap the process. The iteration's own ref keeps
            // p alive until the matching proc_put below.
            for (int k = 0; k < n; k++) {
                fn(batch[k], ctx);
                proc_put(batch[k]);
            }
            if (n < 8) { break; }   // bucket fully stamped this pass
        }
    }
}

int proc_table_alloc_pid_zero(void) {
    // PID 0 is reserved for idle; try to allocate it specifically
    return pid_alloc_specific(&global_proc_table.pid_alloc, 0);
}
