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

static void slots_clear(struct file_descriptor *slots, int n) {
    for (int i = 0; i < n; i++) {
        slots[i].in_use   = 0;
        slots[i].vn       = 0;
        slots[i].position = 0;
        slots[i].writable = 0;
    }
}

// Returns the bucket's level-2 slot array, allocating it on first use.
// MUST be called with the bucket lock held. kmalloc's heap lock ranks
// ABOVE LOCK_RANK_FDTABLE, so taking it here is legal ascending order --
// and doing the allocation inside the lock is what makes the lazy
// allocation atomic. (The earlier version dropped the lock to allocate,
// which let two callers install two arrays and leak one of them, with
// the loser's file descriptors silently vanishing.)
static struct file_descriptor *bucket_slots(struct fd_bucket *b) {
    if (b->slots) {
        return b->slots;
    }
    struct file_descriptor *slots = (struct file_descriptor *)kmalloc(
        FD_TABLE_SLOTS * sizeof(struct file_descriptor));
    if (!slots) {
        return 0;
    }
    slots_clear(slots, FD_TABLE_SLOTS);
    b->slots = slots;
    return slots;
}

void fd_table_init(struct fd_table *table) {
    if (!table) return;

    for (int i = 0; i < FD_TABLE_BUCKETS; i++) {
        spin_init(&table->buckets[i].lock, LOCK_RANK_FDTABLE, "fd_bucket");
        table->buckets[i].slots = 0;  // lazy allocation
        table->buckets[i].slot_count = 0;
    }

    table->next_alloc = 0;
}

// Lockless, like the flat files[] array it replaces. `slots` is only
// ever installed, never swapped or freed while the process runs, so a
// racing allocation can make this miss a brand-new fd but can never
// make it read freed memory.
struct file_descriptor *fd_table_get(struct fd_table *table, int fd) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX) {
        return 0;
    }

    struct fd_bucket *b = &table->buckets[fd_bucket(fd)];
    if (!b->slots) {
        return 0;   // bucket never allocated, so the fd cannot be open
    }

    struct file_descriptor *f = &b->slots[fd_slot(fd)];
    return f->in_use ? f : 0;
}

int fd_table_alloc(struct fd_table *table) {
    if (!table) return -EMFILE;

    for (int attempt = 0; attempt < FD_TABLE_BUCKETS; attempt++) {
        unsigned bucket_idx =
            (unsigned)((table->next_alloc + attempt) % FD_TABLE_BUCKETS);
        struct fd_bucket *b = &table->buckets[bucket_idx];

        uint64_t flags = spin_lock_irqsave(&b->lock);

        struct file_descriptor *slots = bucket_slots(b);
        if (!slots) {
            spin_unlock_irqrestore(&b->lock, flags);
            return -EMFILE;   // OOM
        }

        // Bucket 0's first three slots are fds 0/1/2, handed out by
        // fd_table_put at process creation and never by this allocator.
        unsigned first = (bucket_idx == 0) ? FD_STDIO_COUNT : 0;
        for (unsigned slot_idx = first; slot_idx < FD_TABLE_SLOTS; slot_idx++) {
            if (slots[slot_idx].in_use) { continue; }

            slots[slot_idx].in_use   = 1;  // caller fills in vnode/position
            slots[slot_idx].vn       = 0;
            slots[slot_idx].position = 0;
            slots[slot_idx].writable = 0;
            b->slot_count++;

            // A hint only; a stale value costs a wasted scan, never
            // correctness, so it needs no lock of its own.
            table->next_alloc = (int)bucket_idx;

            spin_unlock_irqrestore(&b->lock, flags);
            return (int)(bucket_idx * FD_TABLE_SLOTS + slot_idx);
        }

        spin_unlock_irqrestore(&b->lock, flags);
    }

    return -EMFILE;   // every bucket full
}

void fd_table_close(struct fd_table *table, int fd) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX) {
        return;
    }

    struct fd_bucket *b = &table->buckets[fd_bucket(fd)];
    if (!b->slots) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&b->lock);

    struct vnode *vn = 0;
    struct file_descriptor *f = &b->slots[fd_slot(fd)];
    if (f->in_use) {
        vn = f->vn;
        f->in_use   = 0;
        f->vn       = 0;
        f->position = 0;
        f->writable = 0;
        b->slot_count--;
    }

    spin_unlock_irqrestore(&b->lock, flags);

    // Deliberately outside the bucket lock: vnode_put writes the inode
    // back through the filesystem, which takes locks of LOWER rank
    // (BLOCKDEV, DRIVER) and can sleep. Dropping the last reference
    // while holding a rank-9 spinlock would trip the rank checker.
    if (vn) {
        vnode_put(vn);
    }
}

// Direct fd setter, used at process creation for fds 0-2. Must not be
// called while holding another lock of rank >= LOCK_RANK_FDTABLE.
int fd_table_put(struct fd_table *table, int fd, struct vnode *vn, int writable) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX || !vn) {
        return 0;
    }

    struct fd_bucket *b = &table->buckets[fd_bucket(fd)];
    unsigned slot_idx = fd_slot(fd);

    uint64_t flags = spin_lock_irqsave(&b->lock);

    struct file_descriptor *slots = bucket_slots(b);
    if (!slots) {
        spin_unlock_irqrestore(&b->lock, flags);
        return 0;   // OOM
    }

    int placed = 0;
    if (!slots[slot_idx].in_use) {
        slots[slot_idx].in_use   = 1;
        slots[slot_idx].vn       = vn;
        slots[slot_idx].position = 0;
        slots[slot_idx].writable = writable;
        b->slot_count++;
        placed = 1;
    }

    spin_unlock_irqrestore(&b->lock, flags);
    return placed;
}

void fd_table_free(struct fd_table *table) {
    if (!table) return;

    for (int bucket_idx = 0; bucket_idx < FD_TABLE_BUCKETS; bucket_idx++) {
        struct fd_bucket *b = &table->buckets[bucket_idx];

        // Detach the array first, then walk it unlocked: vnode_put may
        // sleep, so it cannot run under the bucket lock (see
        // fd_table_close). Nothing else can reach the array once
        // b->slots is cleared.
        uint64_t flags = spin_lock_irqsave(&b->lock);
        struct file_descriptor *slots = b->slots;
        b->slots = 0;
        b->slot_count = 0;
        spin_unlock_irqrestore(&b->lock, flags);

        if (!slots) { continue; }

        for (int slot_idx = 0; slot_idx < FD_TABLE_SLOTS; slot_idx++) {
            if (slots[slot_idx].in_use && slots[slot_idx].vn) {
                vnode_put(slots[slot_idx].vn);
                slots[slot_idx].vn = 0;
            }
        }
        kfree(slots);
    }
}

int fd_table_dup(struct fd_table *dst, struct fd_table *src) {
    if (!dst || !src) return 0;

    for (int bucket_idx = 0; bucket_idx < FD_TABLE_BUCKETS; bucket_idx++) {
        struct fd_bucket *src_b = &src->buckets[bucket_idx];
        struct fd_bucket *dst_b = &dst->buckets[bucket_idx];

        if (!src_b->slots) {
            continue;   // empty in the parent, so nothing to inherit
        }

        // Only the source is locked. `dst` belongs to a process that
        // does not exist yet -- nothing else can reach it -- and taking
        // two locks of the same rank is an inversion the rank checker
        // panics on.
        uint64_t flags = spin_lock_irqsave(&src_b->lock);

        struct file_descriptor *dst_slots = bucket_slots(dst_b);
        if (!dst_slots) {
            spin_unlock_irqrestore(&src_b->lock, flags);
            return 0;   // OOM; caller unwinds via fd_table_free
        }

        for (int slot_idx = 0; slot_idx < FD_TABLE_SLOTS; slot_idx++) {
            struct file_descriptor *s = &src_b->slots[slot_idx];
            if (!s->in_use) { continue; }

            dst_slots[slot_idx] = *s;   // position included: the child
                                        // inherits the offset, then the
                                        // two diverge (docs/stdlib.md)
            // The copy duplicates the vnode POINTER, so the child owes
            // the cache its own reference; without this the first close
            // on either side would free a vnode the other still holds.
            if (dst_slots[slot_idx].vn) {
                dst_slots[slot_idx].vn->refcount++;
            }
            dst_b->slot_count++;
        }

        spin_unlock_irqrestore(&src_b->lock, flags);
    }

    return 1;
}

int fd_table_count(struct fd_table *table) {
    if (!table) return 0;

    int total = 0;
    for (int i = 0; i < FD_TABLE_BUCKETS; i++) {
        struct fd_bucket *b = &table->buckets[i];
        uint64_t flags = spin_lock_irqsave(&b->lock);
        total += b->slot_count;
        spin_unlock_irqrestore(&b->lock, flags);
    }
    return total;
}
