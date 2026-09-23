// AHCI selftests. `ahci_selftest` runs on every boot: FIS encoding, the
// scatter-gather builder, and concurrent queued reads against the first
// AHCI disk (read-only -- it writes nothing). The scratch-disk test
// (error injection) runs only under `make ahcitest`.
#include "drivers/block/ahci/ahci.h"
#include "block/blockdev.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/mmio.h"
#include "drivers/char/serial.h"
#include "errno.h"

static struct ahci_link *scratch_link;
static struct blockdev *scratch_dev;

void ahci_note_scratch(struct ahci_link *l, struct blockdev *d) { scratch_link = l; scratch_dev = d; }

static int bytes_eq(const uint8_t *a, const uint8_t *b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { if (a[i] != b[i]) { return 0; } }
    return 1;
}

static uint32_t prd_dbc(const uint8_t *t, int i) { return (*(const uint32_t *)(t + 0x80 + i * 16 + 12) & 0x3FFFFF) + 1; }
static uint64_t prd_dba(const uint8_t *t, int i) {
    const uint32_t *e = (const uint32_t *)(t + 0x80 + i * 16);
    return (uint64_t)e[0] | (uint64_t)e[1] << 32;
}

static const char *fis_checks(void) {
    uint8_t f[20];
    ahci_fis_ncq(f, 0, 0x123456789AULL, 16, 5, 0, 0);
    if (f[0] != 0x27 || f[1] != 0x80 || f[2] != 0x60)          { return "ncq header"; }
    if (f[3] != 16 || f[11] != 0)                               { return "ncq count in features"; }
    if (f[12] != (5 << 3))                                      { return "ncq tag in count[7:3]"; }
    if (f[4] != 0x9A || f[5] != 0x78 || f[6] != 0x56 ||
        f[8] != 0x34 || f[9] != 0x12 || f[10] != 0x00)          { return "ncq lba48"; }
    if (f[7] != 0x40)                                           { return "ncq device"; }
    ahci_fis_ncq(f, 1, 0, 256, 31, 1, 3);
    if (f[2] != 0x61 || f[7] != 0xC0)                           { return "ncq write fua"; }
    if (f[3] != 0 || f[11] != 1 || f[12] != (31 << 3))          { return "ncq count 256 / tag 31"; }
    if (f[1] != 0x83)                                           { return "pmp field"; }
    ahci_fis_rw(f, ATA_READ_DMA, 0x0ABCDEF1, 8, 0);
    if (f[7] != (0x40 | 0x0A) || f[8] != 0)                     { return "lba28 device bits"; }
    return 0;
}

static const char *prdt_checks(void) {
    static uint8_t table[AHCI_CT_SIZE];
    uint64_t big = pmm_alloc(1);
    if (!big) { return "pmm"; }
    const char *why = 0;
    int n = ahci_build_prdt(table, phys_to_virt(big), 8192, 1);
    if (n != 1 || prd_dba(table, 0) != big || prd_dbc(table, 0) != 8192)  { why = "contiguous 8 KiB"; }
    else if (ahci_build_prdt(table, (uint8_t *)phys_to_virt(big) + 1, 64, 1) != -EINVAL) { why = "odd address"; }
    else if (ahci_build_prdt(table, phys_to_virt(big), 65u * AHCI_PRD_MAX_BYTES, 1) != -E2BIG) { why = "E2BIG"; }
    pmm_free(big, 1);
    if (why) { return why; }

    // Two frames that are NOT physically adjacent, mapped at adjacent
    // virtual pages: one buffer, two PRD entries. (Leaked on purpose:
    // their uncached aliases are permanent.)
    uint64_t a = pmm_alloc(0), gap = pmm_alloc(0), b = pmm_alloc(0);
    if (!a || !gap || !b) { return "pmm"; }
    pmm_free(gap, 0);
    if (b == a + 4096) { return "frames happened to be adjacent"; }
    volatile uint8_t *va = (volatile uint8_t *)mmio_map(a, 4096);
    volatile uint8_t *vb = (volatile uint8_t *)mmio_map(b, 4096);
    if (!va || vb != va + 4096) { return "adjacent mapping"; }
    n = ahci_build_prdt(table, (const void *)(va + 2048), 4096, 1);
    if (n != 2 || prd_dba(table, 0) != a + 2048 || prd_dbc(table, 0) != 2048 ||
        prd_dba(table, 1) != b || prd_dbc(table, 1) != 2048)          { return "split across frames"; }
    return 0;
}

// Eight reads submitted before any is waited on; their data must match
// the same ranges read one at a time.
static uint8_t conc[AHCI_BATCH][16 * 512];

static const char *ncq_checks(struct blockdev *d) {
    struct ahci_link *l = ahci_disk_link(d);
    if (!l) { return "no link"; }
    if (l->port->hba->sncq && l->ncq_depth < 2) { return "NCQ not in use"; }
    struct ahci_req r[AHCI_BATCH];
    uint64_t ref = pmm_alloc(0);
    if (!ref) { return "pmm"; }
    const char *why = 0;
    int n = 0;
    for (; n < AHCI_BATCH; n++) {
        ahci_disk_build(d, &r[n], (uint64_t)n * 997, 16, conc[n], 0);
        if (ahci_submit(l, &r[n]) != 0) { why = "submit"; break; }
    }
    for (int i = 0; i < n; i++) {
        if (ahci_wait(l, &r[i]) != 0 && !why) { why = "queued read"; }
    }
    for (int i = 0; i < AHCI_BATCH && !why; i++) {
        uint8_t *rb = (uint8_t *)phys_to_virt(ref);
        if (blockdev_read(d, (uint64_t)i * 997, 8, rb) != 0 ||
            !bytes_eq(rb, conc[i], 4096))                     { why = "data mismatch (first half)"; break; }
        if (blockdev_read(d, (uint64_t)i * 997 + 8, 8, rb) != 0 ||
            !bytes_eq(rb, conc[i] + 4096, 4096))              { why = "data mismatch (second half)"; break; }
    }
    pmm_free(ref, 0);
    if (!why) {
        ahci_log_port(l->port);
        serial_write_string("ncq: max in flight ");
        ahci_log_dec(ahci_port_max_inflight(l->port));
        serial_write_string("\n");
    }
    return why;
}

static struct blockdev *first_ahci_disk;
static void find_disk(struct blockdev *d, void *arg) {
    (void)arg;
    if (!first_ahci_disk && !d->parent && d->driver && d->driver[0] == 'a' &&
        !(d->flags & BLOCKDEV_RO) && ahci_disk_link(d)) { first_ahci_disk = d; }
}

void ahci_selftest(void) {
    const char *why = fis_checks();
    if (!why) { why = prdt_checks(); }
    if (!why) {
        blockdev_foreach(find_disk, 0);
        if (first_ahci_disk) { why = ncq_checks(first_ahci_disk); }
    }
    if (why) {
        serial_write_string("[ahci] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[ahci] selftest passed\n");
}
