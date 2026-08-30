#include "proc_table.h"
#include "proc.h"
#include "../mm/heap.h"
#include "../serial.h"

struct proc_table global_proc_table;

void proc_table_init(void) {
    // Initialize all buckets
    for (int i = 0; i < PROC_HASH_BUCKETS; i++) {
        spin_init(&global_proc_table.buckets[i].lock, LOCK_RANK_PROCTABLE, "proc_bucket");
        global_proc_table.buckets[i].head = 0;
    }

    // Initialize process count
    global_proc_table.process_count = 0;
    spin_init(&global_proc_table.count_lock, LOCK_RANK_PROCTABLE, "proc_count");

    // Initialize PID allocator
    pid_allocator_init(&global_proc_table.pid_alloc);

    serial_write_string("[proc_table] initialized with ");
    serial_write_hex64(PROC_HASH_BUCKETS);
    serial_write_string(" buckets\n");
}

struct process *proc_table_lookup(int pid) {
    if (pid <= 0) {
        return 0;
    }

    unsigned bucket = proc_hash(pid);
    struct proc_bucket *b = &global_proc_table.buckets[bucket];

    // RCU read-side: No lock needed
    // rcu_read_lock() / rcu_read_unlock() should wrap this in caller if needed
    struct process *p = rcu_dereference(b->head);

    while (p) {
        if (p->pid == pid) {
            return p;
        }
        p = rcu_dereference(p->proc_next_hash);
    }

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
    rcu_assign_pointer(b->head, proc);

    // Update process count
    uint64_t f2 = spin_lock_irqsave(&global_proc_table.count_lock);
    global_proc_table.process_count++;
    spin_unlock_irqrestore(&global_proc_table.count_lock, f2);

    spin_unlock_irqrestore(&b->lock, f);
}

/*
 * Callback invoked after RCU grace period
 * Completes process destruction (free struct and PID)
 */
static void proc_rcu_free(struct rcu_head *rh) {
    struct process *proc = container_of(rh, struct process, rcu);

    // Free PID for reuse
    pid_free(&global_proc_table.pid_alloc, proc->pid);

    // Free process structure
    kfree(proc);
}

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

        // Defer destruction until RCU grace period
        spin_unlock_irqrestore(&b->lock, f);
        call_rcu(&proc->rcu, proc_rcu_free);
    } else {
        spin_unlock_irqrestore(&b->lock, f);
    }
}

int proc_table_alloc_pid_zero(void) {
    // PID 0 is reserved for idle; try to allocate it specifically
    return pid_alloc_specific(&global_proc_table.pid_alloc, 0);
}
