#include "sched/fd_table.h"
#include "sched/proc.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "drivers/char/serial.h"
#include "errno.h"

static inline unsigned fd_bucket(int fd) {
    return (unsigned)fd / FD_TABLE_SLOTS;
}

static inline unsigned fd_slot(int fd) {
    return (unsigned)fd % FD_TABLE_SLOTS;
}

static void slot_reset(struct file_descriptor *f) {
    f->in_use   = 0;
    // Reset to the vnode implementation rather than to null: a freshly
    // allocated slot is a plain file until something says otherwise,
    // and every existing caller of fd_table_alloc/put assumes exactly
    // that. A pipe overwrites it.
    f->ops      = &vnode_file_ops;
    f->priv     = 0;
    f->vn       = 0;
    f->position = 0;
    f->writable = 0;
    // Vnode-backed fds have always been readable regardless of open
    // mode; only pipes distinguish the two ends. See docs/stdlib.md.
    f->readable = 1;
    f->nonblock = 0;
}

static void slots_clear(struct file_descriptor *slots, int n) {
    for (int i = 0; i < n; i++) { slot_reset(&slots[i]); }
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

}

// Lockless, like the flat files[] array it replaces. `slots` is only
// ever installed, never swapped or freed while the process runs, so a
// racing allocation can make this miss a brand-new fd but can never
// make it read freed memory.
struct file_descriptor *fd_table_lock_slot(struct fd_table *table, int fd, uint64_t *flags) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX) { return 0; }
    struct fd_bucket *b = &table->buckets[fd_bucket(fd)];
    if (!b->slots) { return 0; }

    uint64_t f = spin_lock_irqsave(&b->lock);
    struct file_descriptor *fdp = &b->slots[fd_slot(fd)];
    if (!fdp->in_use) {
        spin_unlock_irqrestore(&b->lock, f);
        return 0;
    }
    *flags = f;
    return fdp;
}

void fd_table_unlock_slot(struct fd_table *table, int fd, uint64_t flags) {
    if (!table || fd < 0 || fd >= FD_TABLE_MAX) { return; }
    spin_unlock_irqrestore(&table->buckets[fd_bucket(fd)].lock, flags);
}

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
    return fd_table_alloc_from(table, 0);
}

// The lowest free descriptor >= `from`. fd_table_alloc is this with
// from == 0; fcntl(F_DUPFD) is the only other caller, and it is the
// reason the floor exists at all.
int fd_table_alloc_from(struct fd_table *table, int from) {
    if (!table) return -EMFILE;
    if (from < 0) { from = 0; }

    // Strictly lowest-available, from fd 0 upward. Two things used to
    // break that, and a shell notices both:
    //
    //   - the scan started at a hint bucket, the one the LAST allocation
    //     came from, so an fd freed in a lower bucket was not reused
    //     until the higher ones filled;
    //   - bucket 0 skipped slots 0-2 unconditionally, so
    //     `close(0); open(f)` returned 3 rather than 0.
    //
    // The second is how POSIX redirection is built. `sh < file` is
    // exactly close-then-open, and a program that gets fd 3 back reads
    // its original stdin instead of the file. Startup is unaffected:
    // spawn places fds 0/1/2 with fd_table_put before the process is
    // ever runnable, so this allocator only ever reaches those slots
    // once the process itself has closed one.
    //
    // Full buckets are skipped by their slot_count without a scan, so
    // the common case does not walk 512 slots to find fd 4.
    for (unsigned bucket_idx = (unsigned)from / FD_TABLE_SLOTS;
         bucket_idx < FD_TABLE_BUCKETS; bucket_idx++) {
        struct fd_bucket *b = &table->buckets[bucket_idx];

        uint64_t flags = spin_lock_irqsave(&b->lock);

        if (b->slot_count >= FD_TABLE_SLOTS) {
            spin_unlock_irqrestore(&b->lock, flags);
            continue;                 // full; nothing to scan for
        }

        struct file_descriptor *slots = bucket_slots(b);
        if (!slots) {
            spin_unlock_irqrestore(&b->lock, flags);
            return -EMFILE;   // OOM
        }

        unsigned first_slot = 0;
        if ((unsigned)from / FD_TABLE_SLOTS == bucket_idx) {
            first_slot = (unsigned)from % FD_TABLE_SLOTS;
        }
        for (unsigned slot_idx = first_slot; slot_idx < FD_TABLE_SLOTS; slot_idx++) {
            if (slots[slot_idx].in_use) { continue; }

            slot_reset(&slots[slot_idx]);
            slots[slot_idx].in_use = 1;   // caller fills in the object
            b->slot_count++;

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

    // The descriptor is COPIED out and the slot cleared under the
    // lock; the object's reference is dropped afterwards, outside it.
    // vnode_put writes the inode back through the filesystem, which
    // takes lower-ranked locks and can sleep, and a pipe's close wakes
    // whoever was blocked on it -- neither is safe under a bucket lock.
    struct file_descriptor closing;
    int had = 0;

    uint64_t flags = spin_lock_irqsave(&b->lock);
    struct file_descriptor *f = &b->slots[fd_slot(fd)];
    if (f->in_use) {
        closing = *f;
        had = 1;
        slot_reset(f);
        b->slot_count--;
    }
    spin_unlock_irqrestore(&b->lock, flags);

    if (had) { file_close(&closing); }
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
        slot_reset(&slots[slot_idx]);
        slots[slot_idx].in_use   = 1;
        slots[slot_idx].vn       = vn;
        slots[slot_idx].writable = writable;
        // A device vnode (e.g. /dev/console for the standard streams)
        // needs its per-device file_ops, not the plain vnode ops --
        // devfs's own read/write paths no longer serve device I/O.
        file_bind_vnode_ops(&slots[slot_idx]);
        b->slot_count++;
        placed = 1;
    }

    spin_unlock_irqrestore(&b->lock, flags);
    return placed;
}

// dup2/dup3: make newfd refer to the same open object as oldfd. oldfd
// must be open; newfd is closed first if it held something. Unlike
// fd_table_alloc this WILL target fds 0/1/2 -- that is the point, it is
// how a process rebinds its own standard streams. Returns newfd or a
// negative errno.
//
// Narrow race: oldfd could be closed on another thread between the
// snapshot and the install. NeoOS userland is effectively
// single-threaded per fd table today; revisit if that changes.
int fd_table_dup2(struct fd_table *table, int oldfd, int newfd) {
    if (!table || oldfd < 0 || newfd < 0 ||
        oldfd >= FD_TABLE_MAX || newfd >= FD_TABLE_MAX) {
        return -EBADF;
    }

    struct fd_bucket *ob = &table->buckets[fd_bucket(oldfd)];
    struct file_descriptor src;
    uint64_t of = spin_lock_irqsave(&ob->lock);
    struct file_descriptor *os = bucket_slots(ob);
    if (!os || !os[fd_slot(oldfd)].in_use) {
        spin_unlock_irqrestore(&ob->lock, of);
        return -EBADF;
    }
    src = os[fd_slot(oldfd)];
    spin_unlock_irqrestore(&ob->lock, of);

    if (oldfd == newfd) { return newfd; }

    struct fd_bucket *nb = &table->buckets[fd_bucket(newfd)];
    struct file_descriptor closing;
    int had = 0;

    uint64_t nf = spin_lock_irqsave(&nb->lock);
    struct file_descriptor *ns = bucket_slots(nb);
    if (!ns) { spin_unlock_irqrestore(&nb->lock, nf); return -EMFILE; }
    if (ns[fd_slot(newfd)].in_use) {
        closing = ns[fd_slot(newfd)];
        had = 1;
        slot_reset(&ns[fd_slot(newfd)]);
        nb->slot_count--;
    }
    ns[fd_slot(newfd)] = src;
    ns[fd_slot(newfd)].in_use = 1;
    nb->slot_count++;
    spin_unlock_irqrestore(&nb->lock, nf);

    file_dup(&src);                 // the new slot's reference on the object
    if (had) { file_close(&closing); }
    return newfd;
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
            if (slots[slot_idx].in_use) { file_close(&slots[slot_idx]); }
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
            // The copy duplicates the OBJECT POINTER, so the child owes
            // it a reference of its own. Without this the first close on
            // either side frees something the other still holds -- and
            // for a pipe it would also miscount the ends, so the child
            // closing its copy would look like the last writer going
            // away.
            file_dup(&dst_slots[slot_idx]);
            dst_b->slot_count++;
        }

        spin_unlock_irqrestore(&src_b->lock, flags);
    }

    return 1;
}

