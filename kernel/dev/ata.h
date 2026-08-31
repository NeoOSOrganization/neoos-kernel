#ifndef NEOOS_ATA_H
#define NEOOS_ATA_H

#include <stdint.h>
#include "sync/lock.h"

#define ATA_SECTOR_SIZE 512

// Serializes PIO command sequences. Exposed so blkcache_selftest can
// assert its rank -- the cache holds its own lock (BLOCKDEV, 7) while
// calling into the driver (DRIVER, 8) on a miss.
extern struct spinlock ata_lock;

// Initialises ata_lock. Must run before the first ata_* call.
void ata_init(void);

struct ata_identify_info {
    uint32_t sector_count;
};

// `drive` selects the primary channel's master (0) or slave (1).
// Both share port base 0x1F0 and IRQ 14; drive select is bit 4 of the
// byte written to ATA_DRIVE_HEAD, so a second drive needs no new
// controller, IRQ wiring, or port range.

// Issues IDENTIFY DEVICE to the given drive. Returns 1 on success
// (info->sector_count filled in), 0 on failure (logged).
int ata_identify(uint8_t drive, struct ata_identify_info *info);

// Reads `count` (1-255) sectors starting at `lba` into `buffer`, which
// must be at least count * ATA_SECTOR_SIZE bytes. Returns 1 on success,
// 0 on failure (logged to serial).
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buffer);

// Writes `count` (1-255) sectors of `buffer` (must be at least count *
// ATA_SECTOR_SIZE bytes) to disk starting at `lba`, flushing the
// drive's cache before returning so the write is durable. Returns 1
// on success, 0 on failure (logged to serial).
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void *buffer);

#endif
