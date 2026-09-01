#include "sched/pid_alloc.h"

// This allocator hands out PID *numbers* only. Process lookup by pid is
// proc_table_lookup()'s bucketed hash (kernel/sched/proc_table.c), which
// has per-bucket locks and refcounted results. A radix tree with
// insert/remove/lookup lived here until CS2 and was deleted: nothing
// ever called those three functions, so the tree was never populated,
// and its lookup path read shared nodes with no lock at all -- a race
// that was only ever unreachable by accident.
#include "mm/heap.h"
#include "drivers/char/serial.h"

void pid_allocator_init(struct pid_allocator *alloc) {
    alloc->free_list = 0;
    alloc->next_pid = 1;  // PID 0 reserved for idle
    spin_init(&alloc->lock, LOCK_RANK_PROCTABLE, "pid_alloc");
}

int pid_alloc(struct pid_allocator *alloc) {
    uint64_t f = spin_lock_irqsave(&alloc->lock);

    // Try to reuse a freed PID first
    if (alloc->free_list) {
        struct pid_free_entry *entry = alloc->free_list;
        int pid = entry->pid;
        alloc->free_list = entry->next;
        kfree(entry);
        spin_unlock_irqrestore(&alloc->lock, f);
        return pid;
    }

    // Allocate new PID
    int pid = alloc->next_pid;
    if (pid >= MAX_PIDS) {
        // PID space exhausted; would need wraparound + collision handling
        spin_unlock_irqrestore(&alloc->lock, f);
        return 0;
    }

    alloc->next_pid++;
    spin_unlock_irqrestore(&alloc->lock, f);

    return pid;
}

void pid_free(struct pid_allocator *alloc, int pid) {
    if (pid <= 0) return;

    uint64_t f = spin_lock_irqsave(&alloc->lock);

    // Add to free-list for reuse
    struct pid_free_entry *entry = (struct pid_free_entry *)kmalloc(sizeof(struct pid_free_entry));
    if (entry) {
        entry->pid = pid;
        entry->next = alloc->free_list;
        alloc->free_list = entry;
    }

    spin_unlock_irqrestore(&alloc->lock, f);
}

void pid_alloc_selftest(void) {
    struct pid_allocator a;
    pid_allocator_init(&a);

    // Distinctness: a run of allocations with nothing freed must never
    // repeat a pid.
    int ids[64];
    for (int i = 0; i < 64; i++) {
        ids[i] = pid_alloc(&a);
        if (ids[i] <= 0) {
            serial_write_string("[pid] selftest FAILED: allocation returned 0\n");
            return;
        }
        for (int j = 0; j < i; j++) {
            if (ids[j] == ids[i]) {
                serial_write_string("[pid] selftest FAILED: duplicate live pid\n");
                return;
            }
        }
    }

    // Reuse: a freed pid comes back before next_pid grows again.
    int freed = ids[10];
    pid_free(&a, freed);
    int reused = pid_alloc(&a);
    if (reused != freed) {
        serial_write_string("[pid] selftest FAILED: freed pid not reused\n");
        return;
    }

    // Drain and refill without handing back anything still outstanding.
    for (int i = 0; i < 64; i++) {
        if (ids[i] != freed) { pid_free(&a, ids[i]); }
    }
    pid_free(&a, reused);
    for (int i = 0; i < 64; i++) {
        int p = pid_alloc(&a);
        if (p <= 0) {
            serial_write_string("[pid] selftest FAILED: refill returned 0\n");
            return;
        }
    }

    serial_write_string("[pid] selftest passed\n");
}
