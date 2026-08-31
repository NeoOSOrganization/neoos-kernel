#include "dev/ata.h"
#include "arch/io.h"
#include "dev/serial.h"

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7

#define ATA_POLL_MAX_ITERATIONS 100000
// CACHE_FLUSH maps to a real host fsync() on the backing disk image --
// orders of magnitude slower than the in-memory register transitions
// every other wait in this file deals with, so it gets its own, much
// larger, budget.
#define ATA_POLL_MAX_ITERATIONS_FLUSH 100000000

// Bounded poll -- a drive that never reaches the requested status
// within this many reads is treated as a hardware failure, logged and
// reported to the caller, rather than hanging forever.
static int ata_wait_status_bounded(uint8_t mask, uint8_t value, uint32_t max_iterations) {
    for (uint32_t i = 0; i < max_iterations; i++) {
        if ((inb(ATA_STATUS) & mask) == value) {
            return 1;
        }
    }
    return 0;
}

static int ata_wait_status(uint8_t mask, uint8_t value) {
    return ata_wait_status_bounded(mask, value, ATA_POLL_MAX_ITERATIONS);
}

// "ERR bit set" on its own says a command failed but not which command,
// against what, or why -- and the failure is rare enough (roughly one
// boot in eighty) that a reproduction with no detail in it is a wasted
// run. The Error register distinguishes the cases that matter: ABRT
// (0x04) is a command the drive refused, IDNF (0x10) an LBA outside the
// disk, UNC (0x40) a bad sector.
static void ata_report_error(const char *what, uint8_t drive, uint32_t lba,
                             uint8_t count, uint8_t sector, uint8_t status) {
    serial_write_string("[ata] ");
    serial_write_string(what);
    serial_write_string(" FAILED: ERR bit set, drive=");
    serial_write_hex64(drive);
    serial_write_string(" lba=");
    serial_write_hex64(lba);
    serial_write_string(" count=");
    serial_write_hex64(count);
    serial_write_string(" sector=");
    serial_write_hex64(sector);
    serial_write_string(" status=");
    serial_write_hex64(status);
    serial_write_string(" error=");
    serial_write_hex64(inb(ATA_ERROR));
    serial_write_string("\n");
}

static int ata_identify_locked(uint8_t drive, struct ata_identify_info *info) {
    outb(ATA_DRIVE_HEAD, 0xA0 | ((drive & 1) << 4));
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0) {
        serial_write_string("[ata] identify FAILED: no drive present, drive=");
        serial_write_hex64(drive);
        serial_write_string("\n");
        return 0;
    }
    if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
        serial_write_string("[ata] identify FAILED: BSY never cleared\n");
        return 0;
    }
    if (!ata_wait_status(ATA_STATUS_DRQ, ATA_STATUS_DRQ)) {
        serial_write_string("[ata] identify FAILED: DRQ never set\n");
        return 0;
    }

    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(ATA_DATA);
    }

    info->sector_count = (uint32_t)identify_data[61] << 16 | identify_data[60];

    serial_write_string("[ata] drive identified, drive=");
    serial_write_hex64(drive);
    serial_write_string(" sectors=");
    serial_write_hex64(info->sector_count);
    serial_write_string(" (");
    serial_write_hex64((uint64_t)info->sector_count * ATA_SECTOR_SIZE / (1024 * 1024));
    serial_write_string(" MiB)\n");
    return 1;
}

static int ata_read_sectors_locked(uint8_t drive, uint32_t lba, uint8_t count, void *buffer) {
    uint16_t *out = (uint16_t *)buffer;

    outb(ATA_DRIVE_HEAD, 0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)); // LBA mode
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
            serial_write_string("[ata] read FAILED: BSY never cleared\n");
            return 0;
        }
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            ata_report_error("read", drive, lba, count, s, status);
            return 0;
        }
        if (!(status & ATA_STATUS_DRQ) && !ata_wait_status(ATA_STATUS_DRQ, ATA_STATUS_DRQ)) {
            serial_write_string("[ata] read FAILED: DRQ never set\n");
            return 0;
        }

        for (int i = 0; i < 256; i++) {
            out[(uint32_t)s * 256 + i] = inw(ATA_DATA);
        }
    }
    return 1;
}

static int ata_write_sectors_locked(uint8_t drive, uint32_t lba, uint8_t count, const void *buffer) {
    const uint16_t *in = (const uint16_t *)buffer;

    outb(ATA_DRIVE_HEAD, 0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)); // LBA mode
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
            serial_write_string("[ata] write FAILED: BSY never cleared\n");
            return 0;
        }
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            ata_report_error("write", drive, lba, count, s, status);
            return 0;
        }
        if (!(status & ATA_STATUS_DRQ) && !ata_wait_status(ATA_STATUS_DRQ, ATA_STATUS_DRQ)) {
            serial_write_string("[ata] write FAILED: DRQ never set\n");
            return 0;
        }

        for (int i = 0; i < 256; i++) {
            outw(ATA_DATA, in[(uint32_t)s * 256 + i]);
        }
    }

    // The drive raises BSY again while it commits the last sector's
    // data before it's ready for a new command -- without waiting
    // here, CACHE_FLUSH can be issued into that window.
    if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
        serial_write_string("[ata] write FAILED: BSY never cleared after last sector\n");
        return 0;
    }

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (!ata_wait_status_bounded(ATA_STATUS_BSY, 0, ATA_POLL_MAX_ITERATIONS_FLUSH)) {
        serial_write_string("[ata] write FAILED: cache flush BSY never cleared\n");
        return 0;
    }
    return 1;
}

// ---- locked public entry points -------------------------------------
//
// A PIO command is a SEQUENCE of port writes -- drive select, sector
// count, the three LBA bytes, then the command byte. Two CPUs
// interleaving those sequences issue a command neither one asked for,
// against a drive/LBA neither one chose. One lock over the whole drive
// is the only correct granularity here: the hardware has one set of
// registers.

struct spinlock ata_lock;

void ata_init(void) {
    spin_init(&ata_lock, LOCK_RANK_DRIVER, "ata");
}

int ata_identify(uint8_t drive, struct ata_identify_info *info) {
    uint64_t f = spin_lock_irqsave(&ata_lock);
    int rc = ata_identify_locked(drive, info);
    spin_unlock_irqrestore(&ata_lock, f);
    return rc;
}

int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buffer) {
    uint64_t f = spin_lock_irqsave(&ata_lock);
    int rc = ata_read_sectors_locked(drive, lba, count, buffer);
    spin_unlock_irqrestore(&ata_lock, f);
    return rc;
}

int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void *buffer) {
    uint64_t f = spin_lock_irqsave(&ata_lock);
    int rc = ata_write_sectors_locked(drive, lba, count, buffer);
    spin_unlock_irqrestore(&ata_lock, f);
    return rc;
}
