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
 */

#define BLKCACHE_SECTOR_SIZE 512
#define BLKCACHE_ENTRIES     128   // 128 * 512 = 64KiB
#define BLKCACHE_BUCKETS     64

void blkcache_init(void);

// Reads one sector, from cache if present. Returns 1 on success, 0 if
// the underlying read failed (the cache is left untouched).
int blkcache_read(uint8_t drive, uint32_t lba, void *out);

// Writes one sector through to the disk and updates the cache to
// match. Returns 1 on success; on failure the cached copy is dropped
// rather than left disagreeing with the disk.
int blkcache_write(uint8_t drive, uint32_t lba, const void *in);

// Drops every cached sector for a drive. Called on umount, so a later
// remount cannot be served stale metadata.
void blkcache_invalidate_drive(uint8_t drive);

// Cumulative counters since boot, for the mount-time log line.
void blkcache_stats(uint64_t *out_hits, uint64_t *out_misses);

void blkcache_selftest(void);

#endif
