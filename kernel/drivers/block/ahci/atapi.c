// ATAPI (packet) devices behind AHCI -- optical drives -- registered as
// read-only srN block devices, as Linux's sr driver does: major 11, one
// minor, never partitioned. SCSI commands travel in PACKET (0xA0) with
// DMA; a CHECK CONDITION is explained by REQUEST SENSE.
//
// Capacity is read once at probe: no media-change detection (a
// documented divergence, docs/stdlib.md).
#include "drivers/block/ahci/ahci.h"
#include "drivers/block/ata_id.h"
#include "block/blockdev.h"
#include "time/ktime.h"
#include "drivers/char/serial.h"
#include "errno.h"

#define AHCI_MAX_SR 4

struct ahci_sr {
    struct blockdev bdev;       // first: a blockdev * is an ahci_sr *
    struct ahci_link *link;
};
static struct ahci_sr srs[AHCI_MAX_SR];
static int nsr;

// SCSI sense keys.
#define SK_NOT_READY       0x2
#define SK_UNIT_ATTENTION  0x6
#define ASC_NO_MEDIUM      0x3A

struct sense { uint8_t key, asc, ascq; };

static void packet(struct ahci_req *r, struct ahci_link *l, const uint8_t cdb[12], void *buf, uint32_t len) {
    ahci_req_init(r);
    r->fis[0] = 0x27;
    r->fis[1] = (uint8_t)(0x80 | (l->pmp & 0xF));
    r->fis[2] = ATA_PACKET;
    r->fis[3] = len ? 0x01 : 0x00;             // FEATURES bit 0: data by DMA
    // Byte count limit: meaningful only for PIO, but harmless and what
    // libata sends.
    uint32_t lim = len > 0xFFFE ? 0xFFFE : len;
    r->fis[5] = (uint8_t)lim;
    r->fis[6] = (uint8_t)(lim >> 8);
    for (int i = 0; i < 12; i++) { r->cdb[i] = cdb[i]; }
    r->atapi = 1;
    r->buf = buf;
    r->len = len;
}

// REQUEST SENSE, after a command ended in CHECK CONDITION. The sense key
// is also in the error register's top nibble, which is the fallback.
static void get_sense(struct ahci_link *l, uint8_t tfd_error, struct sense *s) {
    static uint8_t buf[18] __attribute__((aligned(4)));
    static const uint8_t cdb[12] = { 0x03, 0, 0, 0, 18, 0 };
    struct ahci_req r;
    packet(&r, l, cdb, buf, 18);
    s->key = tfd_error >> 4;
    s->asc = s->ascq = 0;
    if (ahci_exec(l, &r) == 0) {
        s->key = buf[2] & 0xF;
        s->asc = buf[12];
        s->ascq = buf[13];
    }
}

static void log_sense(struct ahci_link *l, const char *what, const struct sense *s) {
    ahci_log_port(l->port);
    serial_write_string(what);
    serial_write_string(": sense ");
    serial_write_hex64(s->key);
    serial_write_string("/");
    serial_write_hex64(s->asc);
    serial_write_string("/");
    serial_write_hex64(s->ascq);
    serial_write_string("\n");
}

// Runs a packet command; on CHECK CONDITION fetches the sense and maps
// it: NOT READY is -ENOMEDIUM, anything else -EIO.
static int run(struct ahci_link *l, const uint8_t cdb[12], void *buf, uint32_t len, struct sense *s) {
    struct ahci_req r;
    packet(&r, l, cdb, buf, len);
    int rc = ahci_exec(l, &r);
    s->key = s->asc = s->ascq = 0;
    if (rc != -EIO) { return rc; }
    get_sense(l, r.tfd_error, s);
    return s->key == SK_NOT_READY ? -ENOMEDIUM : -EIO;
}

static int sr_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) {
    struct ahci_sr *sr = (struct ahci_sr *)d;
    uint32_t per = AHCI_CHUNK_BYTES / d->sector_size;
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > per ? per : count;
        uint8_t cdb[12] = { 0x28, 0,
                            (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                            0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        struct sense s;
        int rc = run(sr->link, cdb, p, n * d->sector_size, &s);
        if (rc) {
            if (rc == -EIO || rc == -ENOMEDIUM) { log_sense(sr->link, "read error", &s); }
            return rc;
        }
        lba += n;
        count -= n;
        p += (uint64_t)n * d->sector_size;
    }
    return 0;
}

static int sr_write(struct blockdev *d, uint64_t lba, uint32_t c, const void *b) {
    (void)d; (void)lba; (void)c; (void)b;
    return -EROFS;
}
static int sr_flush(struct blockdev *d) { (void)d; return 0; }

static const struct blockdev_ops sr_ops = { .read = sr_read, .write = sr_write, .flush = sr_flush };

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int ahci_atapi_attach(struct ahci_link *l) {
    if (nsr == AHCI_MAX_SR) { return -ENOSPC; }
    struct ahci_sr *sr = &srs[nsr];
    static uint16_t idbuf[256];
    struct ata_id id;
    struct ahci_req r;
    ahci_req_init(&r);
    ahci_fis_rw(r.fis, ATA_IDENTIFY_PACKET, 0, 0, l->pmp);
    r.fis[7] = 0;
    r.buf = idbuf;
    r.len = sizeof idbuf;
    if (ahci_exec(l, &r) != 0) {
        ahci_log_port(l->port);
        serial_write_string("IDENTIFY PACKET FAILED\n");
        return -EIO;
    }
    ata_id_parse(idbuf, &id);

    // TEST UNIT READY until the reset's UNIT ATTENTION has been reported
    // (QEMU raises one) or the drive says there is no disc.
    static const uint8_t tur[12] = { 0x00 };
    struct sense s;
    int rc = 0;
    for (int tries = 0; tries < 3; tries++) {
        rc = run(l, tur, 0, 0, &s);
        if (rc == 0 || s.key != SK_UNIT_ATTENTION) { break; }
    }
    uint64_t blocks = 0;
    uint32_t bsize = 2048;
    if (rc == 0) {
        static uint8_t cap[8] __attribute__((aligned(4)));
        static const uint8_t rcap[12] = { 0x25 };
        rc = run(l, rcap, cap, 8, &s);
        if (rc == 0) {
            blocks = (uint64_t)be32(cap) + 1;
            bsize = be32(cap + 4);
        }
    }
    if (rc == -ENOMEDIUM || (rc && s.key == SK_NOT_READY && s.asc == ASC_NO_MEDIUM)) {
        blocks = 0;                             // an empty drive still gets its node
        rc = 0;
    }
    if (rc) {
        log_sense(l, "not ready -- FAILED", &s);
        return rc;
    }
    if (bsize != 512 && bsize != 2048 && bsize != 4096) {
        ahci_log_port(l->port);
        serial_write_string("unsupported block size ");
        ahci_log_dec(bsize);
        serial_write_string(", skipped\n");
        return -EINVAL;
    }

    sr->link = l;
    sr->bdev.sector_size = bsize;
    sr->bdev.sector_count = blocks;
    sr->bdev.ops = &sr_ops;
    sr->bdev.priv = sr;
    sr->bdev.driver = "ahci";
    sr->bdev.flags = BLOCKDEV_RO | BLOCKDEV_NOPART;
    if (blockdev_alloc_name("sr", sr->bdev.name) != 0) { return -ENOSPC; }
    nsr++;

    ahci_log_port(l->port);
    serial_write_string(sr->bdev.name);
    serial_write_string(" ");
    ahci_log_dec(blocks);
    serial_write_string(" blocks of ");
    ahci_log_dec(bsize);
    serial_write_string(blocks ? "" : " (no medium)");
    serial_write_string(" model \"");
    serial_write_string(id.model);
    serial_write_string("\"\n");
    return blockdev_register_disk(&sr->bdev);
}
