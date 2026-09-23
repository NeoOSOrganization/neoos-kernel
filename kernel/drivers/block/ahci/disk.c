// SATA disks behind AHCI, registered as sdX block devices.
#include "drivers/block/ahci/ahci.h"
#include "drivers/block/ata_id.h"
#include "block/blockdev.h"
#include "drivers/char/serial.h"
#include "errno.h"

#define AHCI_MAX_DISKS 16

struct ahci_disk {
    struct blockdev bdev;       // first: a blockdev * is an ahci_disk *
    struct ahci_link *link;
    struct ata_id id;
    uint8_t use_ncq, use_fua;
};
static struct ahci_disk disks[AHCI_MAX_DISKS];
static int ndisks;

static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

// Builds one chunk's request: queued when the disk and HBA both do NCQ,
// DMA EXT with LBA48, plain DMA otherwise.
static void build(struct ahci_disk *ad, struct ahci_req *r, uint64_t lba, uint32_t n, void *buf, int wr) {
    uint8_t pmp = ad->link->pmp;
    ahci_req_init(r);
    r->buf = buf;
    r->len = n * ad->bdev.sector_size;
    r->write = (uint8_t)wr;
    if (ad->use_ncq) {
        r->ncq = 1;
        ahci_fis_ncq(r->fis, wr, lba, n, 0, wr && ad->use_fua, pmp);
    } else if (ad->id.lba48) {
        uint8_t cmd = wr ? (ad->use_fua ? ATA_WRITE_DMA_FUA_EXT : ATA_WRITE_DMA_EXT) : ATA_READ_DMA_EXT;
        ahci_fis_rw(r->fis, cmd, lba, n, pmp);
    } else {
        ahci_fis_rw(r->fis, wr ? ATA_WRITE_DMA : ATA_READ_DMA, lba, n, pmp);
    }
}

static int flush_cache(struct ahci_disk *ad) {
    struct ahci_req r;
    ahci_req_init(&r);
    ahci_fis_rw(r.fis, ad->id.lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE, 0, 0, ad->link->pmp);
    r.fis[7] = 0;
    return ahci_exec(ad->link, &r);
}

// Splits into AHCI_CHUNK_BYTES commands and keeps up to AHCI_BATCH of
// them in flight -- with NCQ the device works on them together.
static int xfer(struct blockdev *d, uint64_t lba, uint32_t count, void *buf, int wr) {
    struct ahci_disk *ad = (struct ahci_disk *)d;
    uint32_t ss = d->sector_size, per = AHCI_CHUNK_BYTES / ss;
    uint8_t *p = (uint8_t *)buf;
    struct ahci_req reqs[AHCI_BATCH];
    int err = 0;
    while (count && !err) {
        int n = 0;
        for (; n < AHCI_BATCH && count; n++) {
            uint32_t c = count > per ? per : count;
            build(ad, &reqs[n], lba, c, p, wr);
            int rc = ahci_submit(ad->link, &reqs[n]);
            if (rc) { err = rc; break; }
            lba += c;
            count -= c;
            p += (uint64_t)c * ss;
        }
        for (int i = 0; i < n; i++) {
            int rc = ahci_wait(ad->link, &reqs[i]);
            if (rc && !err) { err = rc; }
        }
    }
    // Write-through, as legacy ATA: a completed write is on the media.
    // FUA does that per command; without it, a volatile cache needs a
    // flush.
    if (!err && wr && !ad->use_fua && ad->id.write_cache) { err = flush_cache(ad); }
    return err;
}

static int d_read(struct blockdev *d, uint64_t lba, uint32_t c, void *b) { return xfer(d, lba, c, b, 0); }
static int d_write(struct blockdev *d, uint64_t lba, uint32_t c, const void *b) { return xfer(d, lba, c, (void *)b, 1); }
static int d_flush(struct blockdev *d) { return flush_cache((struct ahci_disk *)d); }

static const struct blockdev_ops disk_ops = { .read = d_read, .write = d_write, .flush = d_flush };

int ahci_disk_attach(struct ahci_link *l) {
    if (ndisks == AHCI_MAX_DISKS) { return -ENOSPC; }
    struct ahci_disk *ad = &disks[ndisks];
    static uint16_t idbuf[256];
    struct ahci_req r;
    ahci_req_init(&r);
    ahci_fis_rw(r.fis, ATA_IDENTIFY, 0, 0, l->pmp);
    r.fis[7] = 0;
    r.buf = idbuf;
    r.len = sizeof idbuf;
    int rc = ahci_exec(l, &r);
    if (rc) {
        ahci_log_port(l->port);
        serial_write_string("IDENTIFY FAILED\n");
        return rc;
    }
    ata_id_parse(idbuf, &ad->id);
    if (ad->id.logical_sector != 512 && ad->id.logical_sector != 4096) {
        ahci_log_port(l->port);
        serial_write_string("unsupported logical sector size ");
        ahci_log_dec(ad->id.logical_sector);
        serial_write_string(", skipped\n");
        return -EINVAL;
    }
    ad->link = l;
    struct ahci_hba *h = l->port->hba;
    ad->use_ncq = 0;
    l->ncq_depth = 0;
    ad->use_fua = ad->id.fua && ad->id.write_cache && ad->id.lba48;
    ad->bdev.sector_size = ad->id.logical_sector;
    ad->bdev.sector_count = ad->id.sectors;
    ad->bdev.ops = &disk_ops;
    ad->bdev.priv = ad;
    ad->bdev.driver = "ahci";
    (void)h;
    if (blockdev_alloc_name("sd", ad->bdev.name) != 0) { return -ENOSPC; }
    ndisks++;

    ahci_log_port(l->port);
    serial_write_string(ad->bdev.name);
    serial_write_string(" ");
    ahci_log_dec(ad->bdev.sector_count);
    serial_write_string(" sectors ");
    ahci_log_dec(ad->bdev.sector_count * ad->bdev.sector_size >> 20);
    serial_write_string(" MiB model \"");
    serial_write_string(ad->id.model);
    serial_write_string("\" ncq=");
    ahci_log_dec(l->ncq_depth);
    serial_write_string(ad->use_fua ? " fua" : "");
    serial_write_string("\n");

    rc = blockdev_register_disk(&ad->bdev);
    if (rc == 0 && str_eq(ad->id.serial, "NEOOSSCRATCH")) { ahci_note_scratch(l, &ad->bdev); }
    return rc;
}
