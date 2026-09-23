#ifndef NEOOS_RAMBLK_H
#define NEOOS_RAMBLK_H

#include <stdint.h>

struct blockdev;

// A RAM-backed block device for selftests, always BLOCKDEV_HIDDEN.
// Created unregistered; the caller registers it if it wants a scan.
struct blockdev *ramblk_create(const char *name, uint32_t sector_size, uint64_t sectors);
// Unregisters (if registered) and frees.
void     ramblk_destroy(struct blockdev *d);
uint8_t *ramblk_data(struct blockdev *d);
// Device-level read commands issued so far -- lets a cache test tell a
// hit from a miss.
uint64_t ramblk_reads(struct blockdev *d);

#endif
