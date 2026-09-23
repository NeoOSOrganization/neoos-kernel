#include "drivers/block/ata.h"
#include "drivers/block/ata_id.h"
#include "arch/io.h"
#include "drivers/char/serial.h"
#include "time/ktime.h"
#include "block/blockdev.h"
#include "sync/lock.h"
#include "errno.h"

struct ata_identify_info {
    uint32_t sector_count;
};

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

// Status polls are bounded in TIME, not in loop iterations. An
// iteration count measures how fast this CPU runs inb, which says
// nothing about the drive: QEMU clears BSY only when the host-side
// write of the backing file completes, and with a dozen VMs copying
// disk images at once that took longer than 100000 polls -- the
// gauntlet's recurring "[ata] write FAILED: BSY never cleared" on a
// drive that was merely slow. 5 s is orders of magnitude above a
// healthy PIO transition and still reports a dead drive promptly.
#define ATA_POLL_TIMEOUT_NS       5000000000ULL
// CACHE_FLUSH maps to a real host fsync() on the backing disk image --
// orders of magnitude slower than the in-memory register transitions
// every other wait in this file deals with, so it gets Linux's 30 s
// command timeout.
#define ATA_POLL_TIMEOUT_NS_FLUSH 30000000000ULL

// Bounded poll -- a drive that never reaches the requested status
// within timeout_ns is treated as a hardware failure, logged and
// reported to the caller, rather than hanging forever. Runs after
// timer_init, so ktime is calibrated.
static int ata_wait_status_bounded(uint8_t mask, uint8_t value, uint64_t timeout_ns) {
    uint64_t deadline = ktime_after_ns(timeout_ns);
    for (;;) {
        if ((inb(ATA_STATUS) & mask) == value) {
            return 1;
        }
        if (ktime_get_ns() >= deadline) {
            // One last look: the deadline may have passed while this
            // CPU was not running at all.
            return (inb(ATA_STATUS) & mask) == value;
        }
    }
}

static int ata_wait_status(uint8_t mask, uint8_t value) {
    return ata_wait_status_bounded(mask, value, ATA_POLL_TIMEOUT_NS);
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

static void ata_log_drive(uint8_t drive, const char *what) {
    serial_write_string("[ata] drive ");
    serial_write_hex64(drive);
    serial_write_string(what);
}

static int ata_identify_locked(uint8_t drive, struct ata_identify_info *info) {
    outb(ATA_DRIVE_HEAD, 0xA0 | ((drive & 1) << 4));
    // A channel with no devices at all floats high (0xFF) on real
    // hardware; QEMU answers 0. Either way there is nothing to ask, and
    // an empty slot is normal (the disks may be on AHCI or NVMe), so this
    // is not a failure.
    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF || st == 0) {
        ata_log_drive(drive, " not present\n");
        return 0;
    }
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0) {
        ata_log_drive(drive, " not present\n");
        return 0;
    }
    if (!ata_wait_status(ATA_STATUS_BSY, 0)) {
        serial_write_string("[ata] identify FAILED: BSY never cleared\n");
        return 0;
    }
    // A packet (ATAPI) device, or a SATA device behind a bridge, aborts
    // IDENTIFY DEVICE and leaves its signature in the LBA registers. It
    // is not a disk this driver can use -- not a failure either.
    uint8_t mid = inb(ATA_LBA_MID), high = inb(ATA_LBA_HIGH);
    if ((inb(ATA_STATUS) & ATA_STATUS_ERR) || mid || high) {
        ata_log_drive(drive, ": not an ATA disk (sig ");
        serial_write_hex64(mid);
        serial_write_string("/");
        serial_write_hex64(high);
        serial_write_string("), skipped\n");
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
    struct ata_id id;
    ata_id_parse(identify_data, &id);
    // PIO here is LBA28, whatever the drive supports.
    info->sector_count = (uint32_t)(id.sectors > 0x0FFFFFFF ? 0x0FFFFFFF : id.sectors);

    serial_write_string("[ata] drive identified, drive=");
    serial_write_hex64(drive);
    serial_write_string(" sectors=");
    serial_write_hex64(info->sector_count);
    serial_write_string(" (");
    serial_write_hex64((uint64_t)info->sector_count * ATA_SECTOR_SIZE / (1024 * 1024));
    serial_write_string(" MiB) model \"");
    serial_write_string(id.model);
    serial_write_string("\"\n");
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
    if (!ata_wait_status_bounded(ATA_STATUS_BSY, 0, ATA_POLL_TIMEOUT_NS_FLUSH)) {
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

static struct spinlock ata_lock;

void ata_init(void) {
    spin_init(&ata_lock, LOCK_RANK_DRIVER, "ata");
}

// ---- block device glue -----------------------------------------------

#define ATA_LBA28_LIMIT (1ULL << 28)

struct ata_disk {
    struct blockdev bdev;       // first: a blockdev * is an ata_disk *
    uint8_t drive;
};
static struct ata_disk disks[2];

static int ata_flush_locked(uint8_t drive) {
    outb(ATA_DRIVE_HEAD, 0xE0 | ((drive & 1) << 4));
    if (!ata_wait_status(ATA_STATUS_BSY, 0)) { return 0; }
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_status_bounded(ATA_STATUS_BSY, 0, ATA_POLL_TIMEOUT_NS_FLUSH);
}

// The command set is LBA28 with an 8-bit sector count: split into
// commands of at most 255 sectors, each a complete sequence under the
// lock, releasing between them.
static int ata_xfer(struct blockdev *d, uint64_t lba, uint32_t count, void *buf, int wr) {
    uint8_t drive = ((struct ata_disk *)d)->drive;
    if (lba + count > ATA_LBA28_LIMIT) { return -EIO; }
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint8_t n = count > 255 ? 255 : (uint8_t)count;
        uint64_t f = spin_lock_irqsave(&ata_lock);
        int ok = wr ? ata_write_sectors_locked(drive, (uint32_t)lba, n, p)
                    : ata_read_sectors_locked(drive, (uint32_t)lba, n, p);
        spin_unlock_irqrestore(&ata_lock, f);
        if (!ok) { return -EIO; }
        lba += n;
        count -= n;
        p += (uint32_t)n * ATA_SECTOR_SIZE;
    }
    return 0;
}

static int ata_bread(struct blockdev *d, uint64_t lba, uint32_t c, void *b) {
    return ata_xfer(d, lba, c, b, 0);
}
static int ata_bwrite(struct blockdev *d, uint64_t lba, uint32_t c, const void *b) {
    return ata_xfer(d, lba, c, (void *)b, 1);
}
static int ata_bflush(struct blockdev *d) {
    uint64_t f = spin_lock_irqsave(&ata_lock);
    int ok = ata_flush_locked(((struct ata_disk *)d)->drive);
    spin_unlock_irqrestore(&ata_lock, f);
    return ok ? 0 : -EIO;
}

static const struct blockdev_ops ata_ops = { .read = ata_bread, .write = ata_bwrite, .flush = ata_bflush };

void ata_probe(void) {
    for (uint8_t drive = 0; drive < 2; drive++) {
        struct ata_identify_info info;
        uint64_t f = spin_lock_irqsave(&ata_lock);
        int ok = ata_identify_locked(drive, &info);
        spin_unlock_irqrestore(&ata_lock, f);
        if (!ok) { continue; }
        struct ata_disk *ad = &disks[drive];
        ad->drive = drive;
        if (blockdev_alloc_name("sd", ad->bdev.name) != 0) { return; }
        ad->bdev.sector_size = ATA_SECTOR_SIZE;
        ad->bdev.sector_count = info.sector_count;
        ad->bdev.ops = &ata_ops;
        ad->bdev.driver = "ata";
        ad->bdev.priv = ad;
        blockdev_register_disk(&ad->bdev);
    }
}
