#include "fs/blkcache.h"
#include "block/blockdev.h"
#include "block/ramblk.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "errno.h"
#include "sync/lock.h"
#include "drivers/char/serial.h"

struct blk_buf {
    uint8_t *data;              // BLKCACHE_MAX_SECTOR bytes, from the pool
    struct blockdev *dev;       // the WHOLE disk
    uint64_t lba;               // whole-disk LBA
    uint8_t  valid;
    uint64_t last_used;         // LRU clock stamp
    struct blk_buf *hash_next;
};

static struct blk_buf  entries[BLKCACHE_ENTRIES];
static struct blk_buf *buckets[BLKCACHE_BUCKETS];
static uint64_t        clock_tick;
static uint64_t        hits, misses;

// Rank BLOCKDEV: taken below the filesystem's mount/vnode locks and
// above nothing, since the device access itself happens with the lock
// dropped.
static struct spinlock cache_lock;

static unsigned bucket_of(struct blockdev *dev, uint64_t lba) {
    // Knuth multiplicative; consecutive LBAs must land in different
    // buckets or a sequential scan collapses onto one chain.
    uint32_t h = ((uint32_t)lba * 2654435761u) ^ (uint32_t)((uintptr_t)dev >> 4);
    return (unsigned)(h % BLKCACHE_BUCKETS);
}

static void mem_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) { d[i] = s[i]; }
}

void blkcache_init(void) {
    spin_init(&cache_lock, LOCK_RANK_BLOCKDEV, "blkcache");
    // One 512 KiB block backs every entry: 4 KiB each, the largest
    // logical sector any device here can have.
    uint64_t pool = pmm_alloc(7);
    if (!pool) { serial_write_string("[blkcache] init FAILED: no memory for the pool\n"); }
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        entries[i].data = pool ? (uint8_t *)phys_to_virt(pool) + (uint64_t)i * BLKCACHE_MAX_SECTOR : 0;
        entries[i].valid = 0;
        entries[i].hash_next = 0;
        entries[i].last_used = 0;
    }
    for (int i = 0; i < BLKCACHE_BUCKETS; i++) { buckets[i] = 0; }
    clock_tick = 0;
    hits = 0;
    misses = 0;
}

// Caller holds cache_lock.
static struct blk_buf *lookup(struct blockdev *dev, uint64_t lba) {
    for (struct blk_buf *b = buckets[bucket_of(dev, lba)]; b; b = b->hash_next) {
        if (b->valid && b->lba == lba && b->dev == dev) { return b; }
    }
    return 0;
}

// Caller holds cache_lock. Unlinks `b` from whatever bucket it is in.
static void unlink_buf(struct blk_buf *b) {
    if (!b->valid) { return; }
    struct blk_buf **link = &buckets[bucket_of(b->dev, b->lba)];
    while (*link && *link != b) { link = &(*link)->hash_next; }
    if (*link == b) { *link = b->hash_next; }
    b->hash_next = 0;
    b->valid = 0;
}

// Caller holds cache_lock. Returns a buffer ready to be filled in for
// (dev, lba): the existing one if cached, otherwise a free entry,
// otherwise the least recently used one.
static struct blk_buf *claim(struct blockdev *dev, uint64_t lba) {
    struct blk_buf *b = lookup(dev, lba);
    if (b) { return b; }

    struct blk_buf *victim = 0;
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        if (!entries[i].valid) { victim = &entries[i]; break; }
        if (!victim || entries[i].last_used < victim->last_used) {
            victim = &entries[i];
        }
    }

    unlink_buf(victim);
    victim->dev = dev;
    victim->lba = lba;
    victim->valid = 1;
    unsigned bi = bucket_of(dev, lba);
    victim->hash_next = buckets[bi];
    buckets[bi] = victim;
    return victim;
}

// Translates (d, lba, count) to the whole disk, bounds-checked against d.
static int resolve(struct blockdev *d, uint64_t lba, uint32_t count,
                   struct blockdev **whole, uint64_t *dlba) {
    if (count == 0 || lba >= d->sector_count || count > d->sector_count - lba) { return -EIO; }
    *whole = blockdev_whole(d);
    *dlba  = blockdev_disk_lba(d, lba);
    return 0;
}

int blkcache_read(struct blockdev *d, uint64_t lba, void *out) {
    struct blockdev *w;
    uint64_t dl;
    int rc = resolve(d, lba, 1, &w, &dl);
    if (rc) { return rc; }
    uint32_t ss = w->sector_size;

    uint64_t flags = spin_lock_irqsave(&cache_lock);
    struct blk_buf *b = lookup(w, dl);
    if (b) {
        b->last_used = ++clock_tick;
        mem_copy(out, b->data, ss);
        hits++;
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }
    misses++;
    spin_unlock_irqrestore(&cache_lock, flags);

    // The device transfer runs with the lock DROPPED: for PIO it is
    // thousands of cycles of busy-waiting, and holding a spinlock with
    // interrupts off across it would stall the timer. Two callers
    // racing on the same sector both read it and both install the same
    // bytes, which is wasteful but never wrong.
    rc = blockdev_read(w, dl, 1, out);
    if (rc) { return rc; }

    flags = spin_lock_irqsave(&cache_lock);
    b = claim(w, dl);
    mem_copy(b->data, out, ss);
    b->last_used = ++clock_tick;
    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int blkcache_write(struct blockdev *d, uint64_t lba, const void *in) {
    return blkcache_write_multi(d, lba, 1, in);
}

int blkcache_read_multi(struct blockdev *d, uint64_t lba, uint32_t count, void *out) {
    struct blockdev *w;
    uint64_t dl;
    int rc = resolve(d, lba, count, &w, &dl);
    return rc ? rc : blockdev_read(w, dl, count, out);
}

int blkcache_write_multi(struct blockdev *d, uint64_t lba, uint32_t count, const void *in) {
    struct blockdev *w;
    uint64_t dl;
    int rc = resolve(d, lba, count, &w, &dl);
    if (rc) { return rc; }
    // Disk first, then cache: a failed write must not leave the cache
    // holding bytes the disk never received.
    rc = blockdev_write(w, dl, count, in);
    uint32_t ss = w->sector_size;

    uint64_t flags = spin_lock_irqsave(&cache_lock);
    if (count == 1 && rc == 0) {
        struct blk_buf *b = claim(w, dl);
        mem_copy(b->data, in, ss);
        b->last_used = ++clock_tick;
    } else {
        // Refresh (or, on failure, drop) any cached sector in range.
        for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
            struct blk_buf *b = &entries[i];
            if (!b->valid || b->dev != w || b->lba < dl || b->lba >= dl + count) { continue; }
            if (rc == 0) { mem_copy(b->data, (const uint8_t *)in + (b->lba - dl) * ss, ss); }
            else         { unlink_buf(b); }
        }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
    return rc;
}

void blkcache_invalidate(struct blockdev *d) {
    struct blockdev *w = blockdev_whole(d);
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        if (entries[i].valid && entries[i].dev == w) { unlink_buf(&entries[i]); }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
}

void blkcache_stats(uint64_t *out_hits, uint64_t *out_misses) {
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    if (out_hits)   { *out_hits = hits; }
    if (out_misses) { *out_misses = misses; }
    spin_unlock_irqrestore(&cache_lock, flags);
}

// Hermetic: RAM disks only, so it proves the cache's own rules --
// a partition and its disk share one entry, writes (single and multi)
// keep cached copies current, eviction really evicts, and a 4 KiB-
// sector device works -- without depending on what is on the boot disk.
void blkcache_selftest(void) {
    const char *why = 0;
    uint8_t *a = 0, *b = 0;
    struct blockdev *p = 0;
    uint8_t *raw = 0;
    uint64_t r0, r1;
    struct blockdev *d = ramblk_create("bct", 512, 256);
    struct blockdev *q = ramblk_create("bcq", 4096, 16);
    uint64_t pa = pmm_alloc(0), pb = pmm_alloc(0);
    if (!d || !q || !pa || !pb) { why = "setup"; goto out; }
    a = (uint8_t *)phys_to_virt(pa);
    b = (uint8_t *)phys_to_virt(pb);

    // One MBR partition at LBA 64, so "bct1" and "bct" alias sector 70.
    raw = ramblk_data(d);
    raw[446 + 4] = 0x83; raw[446 + 8] = 64; raw[446 + 12] = 128; raw[510] = 0x55; raw[511] = 0xAA;
    for (int i = 0; i < 512; i++) { raw[70 * 512 + i] = (uint8_t)i; }
    blockdev_register_disk(d);
    p = blockdev_find("bct1");
    if (!p) { why = "partition not found"; goto out; }

    r0 = ramblk_reads(d);
    if (blkcache_read(p, 6, a) != 0 || a[3] != 3)         { why = "read via partition"; goto out; }
    if (ramblk_reads(d) != r0 + 1)                        { why = "miss did not reach device"; goto out; }
    if (blkcache_read(d, 70, b) != 0 || b[3] != 3)        { why = "read via disk"; goto out; }
    if (ramblk_reads(d) != r0 + 1)                        { why = "partition/disk alias missed the cache"; goto out; }

    for (int i = 0; i < 512; i++) { a[i] = 0xA5; }
    if (blkcache_write(d, 70, a) != 0)                    { why = "write via disk"; goto out; }
    if (blkcache_read(p, 6, b) != 0 || b[0] != 0xA5)      { why = "write not visible via partition"; goto out; }

    for (int i = 0; i < 3 * 512; i++) { a[i] = 0x3C; }
    if (blkcache_write_multi(d, 69, 3, a) != 0)           { why = "write_multi"; goto out; }
    if (blkcache_read(p, 6, b) != 0 || b[0] != 0x3C)      { why = "write_multi left a stale entry"; goto out; }
    if (raw[71 * 512] != 0x3C)                            { why = "write_multi missed the device"; goto out; }

    // Evict sector 70, read it again: from the device, same bytes.
    for (uint64_t lba = 100; lba < 100 + BLKCACHE_ENTRIES + 8; lba++) {
        if (blkcache_read(d, lba, b) != 0)                { why = "eviction sweep"; goto out; }
    }
    r1 = ramblk_reads(d);
    if (blkcache_read(d, 70, b) != 0 || b[0] != 0x3C)     { why = "reread after eviction"; goto out; }
    if (ramblk_reads(d) != r1 + 1)                        { why = "evicted sector served from cache"; goto out; }

    // A 4 KiB-sector device round-trips whole sectors.
    for (int i = 0; i < 4096; i++) { a[i] = (uint8_t)(i * 7); }
    blockdev_register_disk(q);
    if (blkcache_write(q, 3, a) != 0 || blkcache_read(q, 3, b) != 0) { why = "4Kn io"; goto out; }
    for (int i = 0; i < 4096; i++) { if (b[i] != (uint8_t)(i * 7)) { why = "4Kn data"; goto out; } }

out:
    if (pa) { pmm_free(pa, 0); }
    if (pb) { pmm_free(pb, 0); }
    ramblk_destroy(d);
    ramblk_destroy(q);
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    hits = 0; misses = 0;      // the sweep's misses would drown the boot's real numbers
    spin_unlock_irqrestore(&cache_lock, flags);
    if (why) {
        serial_write_string("[blkcache] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[blkcache] selftest passed\n");
}
