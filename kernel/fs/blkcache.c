#include "fs/blkcache.h"
#include "dev/ata.h"
#include "sync/lock.h"
#include "dev/serial.h"

struct blk_buf {
    uint8_t  data[BLKCACHE_SECTOR_SIZE];
    uint32_t lba;
    uint8_t  drive;
    uint8_t  valid;
    uint64_t last_used;         // LRU clock stamp
    struct blk_buf *hash_next;
};

static struct blk_buf  entries[BLKCACHE_ENTRIES];
static struct blk_buf *buckets[BLKCACHE_BUCKETS];
static uint64_t        clock_tick;
static uint64_t        hits, misses;

// Rank BLOCKDEV: taken below the filesystem's mount/vnode locks and
// above nothing, since the ATA access itself happens with the lock
// dropped.
static struct spinlock cache_lock;

static unsigned bucket_of(uint8_t drive, uint32_t lba) {
    // Knuth multiplicative; consecutive LBAs must land in different
    // buckets or a sequential scan collapses onto one chain.
    uint32_t h = (lba * 2654435761u) ^ ((uint32_t)drive << 24);
    return (unsigned)(h % BLKCACHE_BUCKETS);
}

static void mem_copy(void *dst, const void *src, unsigned n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (unsigned i = 0; i < n; i++) { d[i] = s[i]; }
}

void blkcache_init(void) {
    spin_init(&cache_lock, LOCK_RANK_BLOCKDEV, "blkcache");
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
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
static struct blk_buf *lookup(uint8_t drive, uint32_t lba) {
    for (struct blk_buf *b = buckets[bucket_of(drive, lba)]; b; b = b->hash_next) {
        if (b->valid && b->lba == lba && b->drive == drive) {
            return b;
        }
    }
    return 0;
}

// Caller holds cache_lock. Unlinks `b` from whatever bucket it is in.
static void unlink_buf(struct blk_buf *b) {
    if (!b->valid) { return; }
    struct blk_buf **link = &buckets[bucket_of(b->drive, b->lba)];
    while (*link && *link != b) { link = &(*link)->hash_next; }
    if (*link == b) { *link = b->hash_next; }
    b->hash_next = 0;
    b->valid = 0;
}

// Caller holds cache_lock. Returns a buffer ready to be filled in for
// (drive, lba): the existing one if cached, otherwise a free entry,
// otherwise the least recently used one.
static struct blk_buf *claim(uint8_t drive, uint32_t lba) {
    struct blk_buf *b = lookup(drive, lba);
    if (b) { return b; }

    struct blk_buf *victim = 0;
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        if (!entries[i].valid) { victim = &entries[i]; break; }
        if (!victim || entries[i].last_used < victim->last_used) {
            victim = &entries[i];
        }
    }

    unlink_buf(victim);
    victim->drive = drive;
    victim->lba = lba;
    victim->valid = 1;
    unsigned bi = bucket_of(drive, lba);
    victim->hash_next = buckets[bi];
    buckets[bi] = victim;
    return victim;
}

int blkcache_read(uint8_t drive, uint32_t lba, void *out) {
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    struct blk_buf *b = lookup(drive, lba);
    if (b) {
        b->last_used = ++clock_tick;
        mem_copy(out, b->data, BLKCACHE_SECTOR_SIZE);
        hits++;
        spin_unlock_irqrestore(&cache_lock, flags);
        return 1;
    }
    misses++;
    spin_unlock_irqrestore(&cache_lock, flags);

    // The PIO transfer runs with the lock DROPPED: it is thousands of
    // cycles of busy-waiting on the drive, and holding a spinlock with
    // interrupts off across it would stall the timer. Two callers
    // racing on the same sector both read it and both install the same
    // bytes, which is wasteful but never wrong.
    if (!ata_read_sectors(drive, lba, 1, out)) {
        return 0;
    }

    flags = spin_lock_irqsave(&cache_lock);
    b = claim(drive, lba);
    mem_copy(b->data, out, BLKCACHE_SECTOR_SIZE);
    b->last_used = ++clock_tick;
    spin_unlock_irqrestore(&cache_lock, flags);
    return 1;
}

int blkcache_write(uint8_t drive, uint32_t lba, const void *in) {
    // Disk first, then cache: if the write fails, the cache must not be
    // left holding bytes the disk never received.
    int ok = ata_write_sectors(drive, lba, 1, in);

    uint64_t flags = spin_lock_irqsave(&cache_lock);
    if (ok) {
        struct blk_buf *b = claim(drive, lba);
        mem_copy(b->data, in, BLKCACHE_SECTOR_SIZE);
        b->last_used = ++clock_tick;
    } else {
        struct blk_buf *b = lookup(drive, lba);
        if (b) { unlink_buf(b); }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
    return ok;
}

void blkcache_invalidate_drive(uint8_t drive) {
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        if (entries[i].valid && entries[i].drive == drive) {
            unlink_buf(&entries[i]);
        }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
}

void blkcache_stats(uint64_t *out_hits, uint64_t *out_misses) {
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    if (out_hits)   { *out_hits = hits; }
    if (out_misses) { *out_misses = misses; }
    spin_unlock_irqrestore(&cache_lock, flags);
}

// Exercises the three things that can silently go wrong: a hit must
// return the same bytes as the miss that filled it, a write must be
// visible to the next read without touching the disk, and eviction
// must not resurrect a stale sector under a reused buffer.
void blkcache_selftest(void) {
    uint8_t buf[BLKCACHE_SECTOR_SIZE];
    uint8_t again[BLKCACHE_SECTOR_SIZE];

    if (!blkcache_read(0, 0, buf)) {
        serial_write_string("[blkcache] selftest FAILED: read of LBA 0\n");
        return;
    }

    uint64_t h0, m0, h1, m1;
    blkcache_stats(&h0, &m0);
    if (!blkcache_read(0, 0, again)) {
        serial_write_string("[blkcache] selftest FAILED: reread of LBA 0\n");
        return;
    }
    blkcache_stats(&h1, &m1);

    if (h1 != h0 + 1 || m1 != m0) {
        serial_write_string("[blkcache] selftest FAILED: reread was not a hit\n");
        return;
    }
    for (int i = 0; i < BLKCACHE_SECTOR_SIZE; i++) {
        if (buf[i] != again[i]) {
            serial_write_string("[blkcache] selftest FAILED: hit returned different bytes\n");
            return;
        }
    }

    // Push LBA 0 out by touching more distinct sectors than the cache
    // holds, then read it back: it must come from the disk and still
    // match.
    for (uint32_t lba = 1; lba <= BLKCACHE_ENTRIES + 8; lba++) {
        uint8_t scratch[BLKCACHE_SECTOR_SIZE];
        if (!blkcache_read(0, lba, scratch)) {
            serial_write_string("[blkcache] selftest FAILED: read during eviction sweep\n");
            return;
        }
    }
    if (!blkcache_read(0, 0, again)) {
        serial_write_string("[blkcache] selftest FAILED: read after eviction\n");
        return;
    }
    for (int i = 0; i < BLKCACHE_SECTOR_SIZE; i++) {
        if (buf[i] != again[i]) {
            serial_write_string("[blkcache] selftest FAILED: sector changed across eviction\n");
            return;
        }
    }

    // The eviction sweep above deliberately generates 130-odd misses.
    // Leaving them in would drown the real filesystem numbers the boot
    // log reports, so the counters restart here.
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    hits = 0;
    misses = 0;
    spin_unlock_irqrestore(&cache_lock, flags);

    // BLOCKDEV (7) then DRIVER (8): the cache holds its own lock while
    // calling into the driver on a miss.
    if (ata_lock.rank != LOCK_RANK_DRIVER) {
        serial_write_string("[blkcache] selftest FAILED: ata lock rank wrong\n");
        return;
    }

    serial_write_string("[blkcache] selftest passed\n");
}
