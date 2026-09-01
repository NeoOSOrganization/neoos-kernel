#include "sched/pid_alloc.h"
#include "mm/heap.h"
#include "drivers/char/serial.h"

static void *pid_lookup_internal(struct pid_radix_node *node, int pid, int level) {
    if (!node) return 0;

    int idx = (pid >> (level * PID_RADIX_BITS)) & (PID_RADIX_SIZE - 1);

    if (level == 0) {
        return node->entries[idx];
    }

    struct pid_radix_node *child = node->children[idx];
    return pid_lookup_internal(child, pid, level - 1);
}

static int pid_insert_internal(struct pid_radix_node **node_ptr, int pid, void *process, int level) {
    if (!*node_ptr) {
        *node_ptr = (struct pid_radix_node *)kmalloc(sizeof(struct pid_radix_node));
        if (!*node_ptr) return -1;
        for (int i = 0; i < PID_RADIX_SIZE; i++) {
            (*node_ptr)->entries[i] = 0;
            (*node_ptr)->children[i] = 0;
        }
    }

    int idx = (pid >> (level * PID_RADIX_BITS)) & (PID_RADIX_SIZE - 1);

    if (level == 0) {
        if ((*node_ptr)->entries[idx] != 0) {
            return -1;  // Already in use
        }
        (*node_ptr)->entries[idx] = process;
        return 0;
    }

    return pid_insert_internal(&(*node_ptr)->children[idx], pid, process, level - 1);
}

static void pid_remove_internal(struct pid_radix_node *node, int pid, int level) {
    if (!node) return;

    int idx = (pid >> (level * PID_RADIX_BITS)) & (PID_RADIX_SIZE - 1);

    if (level == 0) {
        node->entries[idx] = 0;
        return;
    }

    struct pid_radix_node *child = node->children[idx];
    pid_remove_internal(child, pid, level - 1);
}

void pid_allocator_init(struct pid_allocator *alloc) {
    alloc->root = 0;
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

int pid_alloc_specific(struct pid_allocator *alloc, int pid) {
    if (pid <= 0 || pid >= MAX_PIDS) {
        return -1;
    }

    uint64_t f = spin_lock_irqsave(&alloc->lock);

    // Check if PID already in use
    if (pid_lookup_internal(alloc->root, pid, 2) != 0) {
        spin_unlock_irqrestore(&alloc->lock, f);
        return -1;
    }

    spin_unlock_irqrestore(&alloc->lock, f);
    return 0;
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

void *pid_lookup(struct pid_allocator *alloc, int pid) {
    if (pid <= 0 || pid >= MAX_PIDS) {
        return 0;
    }

    // RCU read lock would go here in full implementation
    // For now, single-CPU so no lock needed for read
    return pid_lookup_internal(alloc->root, pid, 2);
}

void pid_insert(struct pid_allocator *alloc, int pid, void *process) {
    if (pid <= 0 || pid >= MAX_PIDS || !process) {
        return;
    }

    uint64_t f = spin_lock_irqsave(&alloc->lock);
    pid_insert_internal(&alloc->root, pid, process, 2);
    spin_unlock_irqrestore(&alloc->lock, f);
}

void pid_remove(struct pid_allocator *alloc, int pid) {
    if (pid <= 0 || pid >= MAX_PIDS) {
        return;
    }

    uint64_t f = spin_lock_irqsave(&alloc->lock);
    pid_remove_internal(alloc->root, pid, 2);
    spin_unlock_irqrestore(&alloc->lock, f);
}
