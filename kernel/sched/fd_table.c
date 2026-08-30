#include "fd_table.h"
#include "proc.h"
#include "../mm/heap.h"
#include "../fs/vfs.h"
#include "../serial.h"
#include "../errno.h"

static inline unsigned fd_bucket(int fd) {
    return (unsigned)fd / FD_TABLE_SLOTS;
}

static inline unsigned fd_slot(int fd) {
    return (unsigned)fd % FD_TABLE_SLOTS;
}

void fd_table_init(struct fd_table *table) {
    if (!table) return;

    // Initialize all buckets (level 1)
    for (int i = 0; i < FD_TABLE_BUCKETS; i++) {
        spin_init(&table->buckets[i].lock, LOCK_RANK_FDTABLE, "fd_bucket");
        table->buckets[i].slots = 0;  // lazy allocation
        table->buckets[i].slot_count = 0;
    }

    table->fd_count = 0;
    table->next_alloc = 0;
    // count_lock has higher rank to prevent inversions with bucket locks
    spin_init(&table->count_lock, LOCK_RANK_SIGQUEUE, "fd_count");
}

struct file_descriptor *fd_table_get(struct fd_table *table, int fd) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX) {
        return 0;
    }

    unsigned bucket_idx = fd_bucket(fd);
    unsigned slot_idx = fd_slot(fd);

    struct fd_bucket *b = &table->buckets[bucket_idx];

    // If bucket hasn't been allocated yet, FD doesn't exist
    if (!b->slots) {
        return 0;
    }

    struct file_descriptor *f = &b->slots[slot_idx];
    return f->in_use ? f : 0;
}

int fd_table_alloc(struct fd_table *table) {
    if (!table) return -EMFILE;

    for (int attempt = 0; attempt < FD_TABLE_BUCKETS; attempt++) {
        unsigned bucket_idx = table->next_alloc;
        struct fd_bucket *b = &table->buckets[bucket_idx];

        uint64_t bucket_flags = spin_lock_irqsave(&b->lock);

        // Lazy allocate level-2 slots if needed
        if (!b->slots) {
            spin_unlock_irqrestore(&b->lock, bucket_flags);

            // Allocate 512-entry slot array (without holding any locks)
            b->slots = (struct file_descriptor *)kmalloc(
                FD_TABLE_SLOTS * sizeof(struct file_descriptor));
            if (!b->slots) {
                return -EMFILE;  // OOM
            }

            // Zero-initialize all slots
            for (int i = 0; i < FD_TABLE_SLOTS; i++) {
                b->slots[i].in_use = 0;
                b->slots[i].vn = 0;
                b->slots[i].position = 0;
                b->slots[i].writable = 0;
            }

            // Retry the bucket lock acquisition after allocation
            bucket_flags = spin_lock_irqsave(&b->lock);
        }

        // Find first unused slot in this bucket
        for (unsigned slot_idx = 0; slot_idx < FD_TABLE_SLOTS; slot_idx++) {
            if (!b->slots[slot_idx].in_use) {
                int fd = bucket_idx * FD_TABLE_SLOTS + slot_idx;

                // Skip FDs 0-2 (stdin, stdout, stderr) on first allocation
                if (fd < 3) continue;

                // Mark as in-use (caller will fill in vnode, position, writable)
                b->slots[slot_idx].in_use = 1;
                b->slot_count++;

                // Update next_alloc hint for next search
                if (slot_idx + 1 < FD_TABLE_SLOTS) {
                    table->next_alloc = bucket_idx;
                } else {
                    table->next_alloc = (bucket_idx + 1) % FD_TABLE_BUCKETS;
                }

                // Update global FD count
                uint64_t count_flags = spin_lock_irqsave(&table->count_lock);
                table->fd_count++;
                spin_unlock_irqrestore(&table->count_lock, count_flags);

                spin_unlock_irqrestore(&b->lock, bucket_flags);
                return fd;
            }
        }

        // This bucket is full, try next one
        spin_unlock_irqrestore(&b->lock, bucket_flags);
        table->next_alloc = (bucket_idx + 1) % FD_TABLE_BUCKETS;
    }

    return -EMFILE;  // All buckets full
}

void fd_table_close(struct fd_table *table, int fd) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX) {
        return;
    }

    unsigned bucket_idx = fd_bucket(fd);
    unsigned slot_idx = fd_slot(fd);

    struct fd_bucket *b = &table->buckets[bucket_idx];

    if (!b->slots) {
        return;  // Bucket not allocated, FD doesn't exist
    }

    uint64_t bucket_flags = spin_lock_irqsave(&b->lock);

    struct file_descriptor *f = &b->slots[slot_idx];
    if (f->in_use) {
        // Release vnode reference if held
        if (f->vn) {
            vnode_put(f->vn);
            f->vn = 0;
        }

        f->in_use = 0;
        f->position = 0;
        f->writable = 0;

        b->slot_count--;

        // Update global FD count
        uint64_t count_flags = spin_lock_irqsave(&table->count_lock);
        table->fd_count--;
        spin_unlock_irqrestore(&table->count_lock, count_flags);
    }

    spin_unlock_irqrestore(&b->lock, bucket_flags);
}

// Direct FD setter: used during process startup for FDs 0-2
// Assumes FD table bucket for this FD is not yet allocated
// WARNING: Must NOT be called while holding any other locks
int fd_table_put(struct fd_table *table, int fd, struct vnode *vn, int writable) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX || !vn) {
        return 0;
    }

    unsigned bucket_idx = fd_bucket(fd);
    unsigned slot_idx = fd_slot(fd);

    struct fd_bucket *b = &table->buckets[bucket_idx];

    uint64_t bucket_flags = spin_lock_irqsave(&b->lock);

    // Lazy allocate if needed
    if (!b->slots) {
        spin_unlock_irqrestore(&b->lock, bucket_flags);

        b->slots = (struct file_descriptor *)kmalloc(
            FD_TABLE_SLOTS * sizeof(struct file_descriptor));
        if (!b->slots) {
            return 0;  // OOM
        }

        // Zero-initialize
        for (int i = 0; i < FD_TABLE_SLOTS; i++) {
            b->slots[i].in_use = 0;
            b->slots[i].vn = 0;
            b->slots[i].position = 0;
            b->slots[i].writable = 0;
        }

        bucket_flags = spin_lock_irqsave(&b->lock);
    }

    // Set the FD
    if (!b->slots[slot_idx].in_use) {
        b->slots[slot_idx].in_use = 1;
        b->slots[slot_idx].vn = vn;
        b->slots[slot_idx].position = 0;
        b->slots[slot_idx].writable = writable;
        b->slot_count++;

        uint64_t count_flags = spin_lock_irqsave(&table->count_lock);
        table->fd_count++;
        spin_unlock_irqrestore(&table->count_lock, count_flags);
    }

    spin_unlock_irqrestore(&b->lock, bucket_flags);
    return 1;
}

void fd_table_free(struct fd_table *table) {
    if (!table) return;

    // Close all open FDs (which releases vnode references)
    for (int bucket_idx = 0; bucket_idx < FD_TABLE_BUCKETS; bucket_idx++) {
        struct fd_bucket *b = &table->buckets[bucket_idx];

        if (b->slots) {
            for (int slot_idx = 0; slot_idx < FD_TABLE_SLOTS; slot_idx++) {
                struct file_descriptor *f = &b->slots[slot_idx];
                if (f->in_use && f->vn) {
                    vnode_put(f->vn);
                    f->vn = 0;
                }
            }
            kfree(b->slots);
            b->slots = 0;
        }
    }

    table->fd_count = 0;
}

int fd_table_dup(struct fd_table *dst, struct fd_table *src) {
    if (!dst || !src) return 0;

    // Iterate through all buckets in source table
    for (int bucket_idx = 0; bucket_idx < FD_TABLE_BUCKETS; bucket_idx++) {
        struct fd_bucket *src_b = &src->buckets[bucket_idx];

        if (!src_b->slots) {
            continue;  // This bucket empty in source, skip it
        }

        // Allocate corresponding bucket in destination
        struct fd_bucket *dst_b = &dst->buckets[bucket_idx];

        uint64_t dst_flags = spin_lock_irqsave(&dst_b->lock);

        if (!dst_b->slots) {
            spin_unlock_irqrestore(&dst_b->lock, dst_flags);

            dst_b->slots = (struct file_descriptor *)kmalloc(
                FD_TABLE_SLOTS * sizeof(struct file_descriptor));
            if (!dst_b->slots) {
                return 0;  // OOM
            }

            // Zero-initialize
            for (int i = 0; i < FD_TABLE_SLOTS; i++) {
                dst_b->slots[i].in_use = 0;
                dst_b->slots[i].vn = 0;
                dst_b->slots[i].position = 0;
                dst_b->slots[i].writable = 0;
            }

            dst_flags = spin_lock_irqsave(&dst_b->lock);
        }

        // Copy all slots from source bucket to destination
        uint64_t src_flags = spin_lock_irqsave(&src_b->lock);

        for (int slot_idx = 0; slot_idx < FD_TABLE_SLOTS; slot_idx++) {
            struct file_descriptor *src_f = &src_b->slots[slot_idx];
            struct file_descriptor *dst_f = &dst_b->slots[slot_idx];

            if (src_f->in_use) {
                dst_f->in_use = 1;
                dst_f->vn = src_f->vn;
                dst_f->position = 0;  // NOT shared across fork per docs/stdlib.md
                dst_f->writable = src_f->writable;

                // Increment vnode reference since child now holds it
                if (dst_f->vn) {
                    dst_f->vn->refcount++;
                }

                dst_b->slot_count++;
            }
        }

        spin_unlock_irqrestore(&src_b->lock, src_flags);
        spin_unlock_irqrestore(&dst_b->lock, dst_flags);

        // Update destination fd_count
        uint64_t count_flags = spin_lock_irqsave(&dst->count_lock);
        dst->fd_count += dst_b->slot_count;
        spin_unlock_irqrestore(&dst->count_lock, count_flags);
    }

    return 1;  // Success
}
