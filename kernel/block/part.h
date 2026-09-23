#ifndef NEOOS_PART_H
#define NEOOS_PART_H

struct blockdev;

// Scans a whole disk for a GPT, then an MBR, registering each valid
// entry as a partition blockdev. Called once by blockdev_register_disk.
void part_scan(struct blockdev *disk);
void part_selftest(void);

#endif
