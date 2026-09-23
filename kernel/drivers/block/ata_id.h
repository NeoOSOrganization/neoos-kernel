#ifndef NEOOS_ATA_ID_H
#define NEOOS_ATA_ID_H

#include <stdint.h>

// What an IDENTIFY DEVICE / IDENTIFY PACKET DEVICE record says about a
// drive, parsed once so that legacy ATA and AHCI describe a disk the
// same way. Word numbers are ACS-3's.
struct ata_id {
    uint64_t sectors;          // LBA48 count if supported, else LBA28 (words 60-61)
    uint32_t logical_sector;   // bytes; 512 unless word 106 says otherwise
    uint8_t  lba48;            // word 83 bit 10
    uint8_t  ncq;              // word 76 bit 8
    uint8_t  fua;              // word 84 bit 6: WRITE DMA FUA EXT, NCQ FUA
    uint8_t  write_cache;      // word 85 bit 5: volatile write cache enabled
    uint8_t  queue_depth;      // word 75 bits 4:0, plus one; 1 without NCQ
    uint8_t  atapi;            // word 0 bit 15 (packet device)
    char     serial[21];       // words 10-19, trailing spaces trimmed
    char     model[41];        // words 27-46, trailing spaces trimmed
};

void ata_id_parse(const uint16_t w[256], struct ata_id *out);
void ata_id_selftest(void);

#endif
