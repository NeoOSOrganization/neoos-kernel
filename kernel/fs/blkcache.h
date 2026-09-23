#ifndef NEOOS_BLKCACHE_H
#define NEOOS_BLKCACHE_H

#include <stdint.h>

/*
 * Sector cache sitting between the filesystems and the ATA driver.
 *
 * Every FAT access this kernel makes is a single 512-byte sector, and
 * the same sectors are read over and over: a FAT16 sector holds 256
 * chain entries, so walking a chain or scanning for a free cluster
 * re-reads one sector hundreds of times in a row. Before this cache,
 * finding a free cluster on the 32MiB test volume cost up to 16,384
 * PIO reads of 64 distinct sectors.
 *
 * Write-through, deliberately. A write-back cache would also collapse
 * the read-modify-write that every FAT entry update performs, but it
 * would put the on-disk filesystem behind RAM with no journal and no
 * flush-on-power-loss story. The reads are where the cost actually is.
 *
 * Not a page cache: it caches disk sectors, not file contents, and
 * knows nothing about files.
 *
 * Keyed by (whole-disk blockdev, whole-disk LBA): a sector read through
 * /dev/sda1 and through /dev/sda is one entry, so the two can never
 * disagree.
 */

struct blockdev;

#define BLKCACHE_MAX_SECTOR 4096   // largest logical sector size (4Kn NVMe)
#define BLKCACHE_ENTRIES    128    // 128 * 4 KiB = 512 KiB, from the PMM
#define BLKCACHE_BUCKETS    64

void blkcache_init(void);

// One logical sector of `d` (a disk or a partition). Return 0 or a
// negative errno; -EIO for an LBA outside `d`. A failed write drops the
// cached copy rather than leave it disagreeing with the disk.
int  blkcache_read(struct blockdev *d, uint64_t lba, void *out);
int  blkcache_write(struct blockdev *d, uint64_t lba, const void *in);

// Multi-sector transfers straight to the device, for bulk I/O (raw
// device reads, ext2 blocks) that would otherwise evict the metadata
// the cache exists for. Coherent: a multi-write updates any cached
// sector in its range; a multi-read cannot see stale data because the
// cache is write-through.
int  blkcache_read_multi(struct blockdev *d, uint64_t lba, uint32_t count, void *out);
int  blkcache_write_multi(struct blockdev *d, uint64_t lba, uint32_t count, const void *in);

// Drops every cached sector of `d`'s whole disk. Called on umount and
// when a disk is unregistered.
void blkcache_invalidate(struct blockdev *d);

// Cumulative counters since boot, for the mount-time log line.
void blkcache_stats(uint64_t *out_hits, uint64_t *out_misses);

void blkcache_selftest(void);

#endif
