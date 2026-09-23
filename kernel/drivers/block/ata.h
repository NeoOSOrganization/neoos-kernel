#ifndef NEOOS_ATA_H
#define NEOOS_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

// Legacy IDE: primary channel, PIO, LBA28. Registers each drive it
// finds as a block device ("sdX"); nothing outside this file drives the
// hardware directly any more.

// Initialises the driver lock. Must run before ata_probe.
void ata_init(void);

// Identifies master and slave and registers each drive present.
void ata_probe(void);

#endif
