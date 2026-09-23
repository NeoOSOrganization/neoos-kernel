// Partition-table scanner: GPT first (a GPT disk's MBR is a protective
// placeholder), then MBR primaries. Runs once per whole disk, from
// blockdev_register_disk. A disk with neither is left whole.
#include "block/part.h"
#include "block/blockdev.h"
#include "block/ramblk.h"
#include "lib/crc32.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "errno.h"
#include "drivers/char/serial.h"

#define GPT_MAX_ENTRIES 128
#define GPT_ENTRY_SIZE  128

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static void note(struct blockdev *d, const char *msg, uint32_t n) {
    if (d->flags & BLOCKDEV_HIDDEN) { return; }
    serial_write_string("part: ");
    serial_write_string(d->name);
    serial_write_string(": ");
    if (n) { serial_write_string("entry "); serial_write_hex64(n); serial_write_string(" rejected: "); }
    serial_write_string(msg);
    serial_write_string("\n");
}

struct accepted { uint64_t first, last; };

// Registers [first, first+count) as partition `no` unless it is empty,
// runs past the disk, or overlaps an entry already accepted on it.
static void accept(struct blockdev *d, struct accepted *acc, int *nacc, uint32_t no,
                   uint64_t first, uint64_t count, const uint8_t type[16], const uint8_t uuid[16]) {
    if (count == 0) { note(d, "zero length", no); return; }
    if (first >= d->sector_count || count > d->sector_count - first) { note(d, "past end of disk", no); return; }
    uint64_t last = first + count - 1;
    for (int i = 0; i < *nacc; i++) {
        if (first <= acc[i].last && last >= acc[i].first) { note(d, "overlaps another entry", no); return; }
    }
    if (blockdev_register_part(d, no, first, count, type, uuid) == 0) {
        acc[*nacc].first = first;
        acc[*nacc].last = last;
        (*nacc)++;
    }
}

// Reads the GPT header at `lba` into `hdr` and its entry array into
// `ents`. Returns 1 only if both CRCs check out.
static int gpt_load(struct blockdev *d, uint64_t lba, uint8_t *hdr, uint8_t *ents) {
    if (lba == 0 || lba >= d->sector_count) { return 0; }
    if (blockdev_read(d, lba, 1, hdr) != 0) { return 0; }
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) { if (hdr[i] != (uint8_t)sig[i]) { return 0; } }
    uint32_t hsize = get32(hdr + 12);
    if (hsize < 92 || hsize > d->sector_size) { return 0; }
    uint32_t want = get32(hdr + 16);
    uint8_t save[4];
    for (int i = 0; i < 4; i++) { save[i] = hdr[16 + i]; hdr[16 + i] = 0; }
    uint32_t got = crc32(0, hdr, hsize);
    for (int i = 0; i < 4; i++) { hdr[16 + i] = save[i]; }
    if (got != want) { return 0; }

    uint32_t n = get32(hdr + 80), esz = get32(hdr + 84);
    if (esz != GPT_ENTRY_SIZE || n == 0 || n > GPT_MAX_ENTRIES) { return 0; }
    uint64_t ent_lba = get64(hdr + 72);
    uint32_t bytes = n * esz;
    uint32_t secs = (bytes + d->sector_size - 1) / d->sector_size;
    if (ent_lba >= d->sector_count || secs > d->sector_count - ent_lba) { return 0; }
    if (blockdev_read(d, ent_lba, secs, ents) != 0) { return 0; }
    return crc32(0, ents, bytes) == get32(hdr + 88);
}

// Linux's rule (block/partitions/efi.c, without the "gpt" boot option):
// a GPT is only trusted when the MBR says the disk is GPT -- a protective
// 0xEE entry -- or the primary header at LBA 1 carries the signature. A
// valid backup GPT alone is not enough: a disk re-imaged with an MBR over
// an old GPT disk keeps a stale backup at its end.
static int gpt_expected(struct blockdev *d, uint8_t *sec) {
    if (blockdev_read(d, 0, 1, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA) {
        for (int i = 0; i < 4; i++) { if (sec[446 + 16 * i + 4] == 0xEE) { return 1; } }
    }
    if (blockdev_read(d, 1, 1, sec) == 0) {
        static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
        int ok = 1;
        for (int i = 0; i < 8; i++) { if (sec[i] != (uint8_t)sig[i]) { ok = 0; break; } }
        if (ok) { return 1; }
    }
    return 0;
}

static int scan_gpt(struct blockdev *d, uint8_t *sec, uint8_t *ents) {
    uint64_t backup_lba = d->sector_count - 1;
    int ok = gpt_load(d, 1, sec, ents);
    if (!ok) {
        // A primary that at least carries the signature names its
        // backup; otherwise the backup is the last sector by definition.
        if (sec[0] == 'E' && sec[1] == 'F' && sec[2] == 'I') {
            uint64_t alt = get64(sec + 32);
            if (alt > 1 && alt < d->sector_count) { backup_lba = alt; }
        }
        ok = gpt_load(d, backup_lba, sec, ents);
        if (ok) { note(d, "primary GPT invalid, using backup", 0); }
    }
    if (!ok) { return 0; }

    struct accepted acc[GPT_MAX_ENTRIES];
    int nacc = 0;
    uint32_t n = get32(sec + 80);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = ents + i * GPT_ENTRY_SIZE;
        int empty = 1;
        for (int k = 0; k < 16; k++) { if (e[k]) { empty = 0; break; } }
        if (empty) { continue; }
        uint64_t first = get64(e + 32), last = get64(e + 40);
        if (last < first) { note(d, "last before first", i + 1); continue; }
        accept(d, acc, &nacc, i + 1, first, last - first + 1, e, e + 16);
    }
    return 1;
}

static void scan_mbr(struct blockdev *d, uint8_t *sec) {
    if (blockdev_read(d, 0, 1, sec) != 0) { return; }
    if (sec[510] != 0x55 || sec[511] != 0xAA) { return; }
    // Linux's test (block/partitions/msdos.c): every boot indicator must
    // be 0x00 or 0x80. A FAT or other filesystem boot sector on an
    // unpartitioned disk also ends in 55 AA, but its "entries" are boot
    // code and message text, and fail this.
    for (int i = 0; i < 4; i++) {
        uint8_t boot = sec[446 + 16 * i];
        if (boot != 0x00 && boot != 0x80) { return; }
    }
    for (int i = 0; i < 4; i++) {
        if (sec[446 + 16 * i + 4] == 0xEE) { note(d, "protective MBR but no valid GPT", 0); return; }
    }
    struct accepted acc[4];
    int nacc = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = sec + 446 + 16 * i;
        uint8_t type = p[4];
        if (type == 0) { continue; }
        if (type == 0x05 || type == 0x0F || type == 0x85) { note(d, "extended partition skipped", 0); continue; }
        uint8_t t[16] = { type };
        accept(d, acc, &nacc, (uint32_t)i + 1, get32(p + 8), get32(p + 12), t, 0);
    }
}

void part_scan(struct blockdev *disk) {
    // One sector (up to 4 KiB) plus the 16 KiB entry array.
    uint64_t sec_phys = pmm_alloc(0), ent_phys = pmm_alloc(2);
    if (!sec_phys || !ent_phys) {
        if (sec_phys) { pmm_free(sec_phys, 0); }
        if (ent_phys) { pmm_free(ent_phys, 2); }
        note(disk, "scan FAILED: out of memory", 0);
        return;
    }
    uint8_t *sec = (uint8_t *)phys_to_virt(sec_phys);
    uint8_t *ents = (uint8_t *)phys_to_virt(ent_phys);
    if (disk->sector_count >= 3 && gpt_expected(disk, sec) && scan_gpt(disk, sec, ents)) {
        // GPT found and registered.
    } else {
        scan_mbr(disk, sec);
    }
    pmm_free(sec_phys, 0);
    pmm_free(ent_phys, 2);
}

// ---- selftest -------------------------------------------------------------
#define T_SECT 1024       // 512 KiB RAM disk
#define T_SS   512

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); } }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v >> (8 * i)); } }

struct tent { int index; uint64_t first, last; };

// Writes a GPT (primary at LBA 1, entries at 2..33; backup entries at
// T_SECT-33.., backup header at T_SECT-1) plus a protective MBR.
static void gpt_build(uint8_t *disk, const struct tent *e, int n) {
    for (uint64_t i = 0; i < (uint64_t)T_SECT * T_SS; i++) { disk[i] = 0; }
    uint8_t *mbr = disk;
    mbr[446 + 4] = 0xEE; put32(mbr + 446 + 8, 1); put32(mbr + 446 + 12, T_SECT - 1);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    // The entry array is built in place at the primary location, then
    // copied to the backup location.
    uint8_t *ents = disk + 2 * T_SS;
    for (int i = 0; i < n; i++) {
        uint8_t *p = ents + e[i].index * 128;
        p[0] = 0xAF; p[1] = 0x3D; p[2] = 0xC6; p[3] = 0x0F;   // any non-zero type GUID
        p[16] = (uint8_t)(i + 1);                              // unique GUID
        put64(p + 32, e[i].first); put64(p + 40, e[i].last);
    }
    uint32_t ecrc = crc32(0, ents, 128 * 128);
    for (int i = 0; i < 128 * 128; i++) { disk[(T_SECT - 33) * T_SS + i] = ents[i]; }
    for (int copy = 0; copy < 2; copy++) {
        uint64_t hdr_lba = copy ? T_SECT - 1 : 1, alt = copy ? 1 : T_SECT - 1;
        uint64_t ent_lba = copy ? T_SECT - 33 : 2;
        uint8_t *h = disk + hdr_lba * T_SS;
        static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
        for (int i = 0; i < 8; i++) { h[i] = (uint8_t)sig[i]; }
        put32(h + 8, 0x00010000); put32(h + 12, 92);
        put64(h + 24, hdr_lba); put64(h + 32, alt);
        put64(h + 40, 34); put64(h + 48, T_SECT - 34);
        put64(h + 72, ent_lba); put32(h + 80, 128); put32(h + 84, 128); put32(h + 88, ecrc);
        put32(h + 16, 0);
        put32(h + 16, crc32(0, h, 92));
    }
}

static void mbr_build(uint8_t *disk, const uint8_t types[4], const uint32_t start[4], const uint32_t count[4]) {
    for (uint64_t i = 0; i < (uint64_t)T_SECT * T_SS; i++) { disk[i] = 0; }
    for (int i = 0; i < 4; i++) {
        uint8_t *p = disk + 446 + 16 * i;
        p[4] = types[i]; put32(p + 8, start[i]); put32(p + 12, count[i]);
    }
    disk[510] = 0x55; disk[511] = 0xAA;
}

// Registers a hidden RAM disk built by `fill`, returns it (caller destroys).
static struct blockdev *scan_disk(const char *name, void (*fill)(uint8_t *, void *), void *arg) {
    struct blockdev *d = ramblk_create(name, T_SS, T_SECT);
    if (!d) { return 0; }
    fill(ramblk_data(d), arg);
    blockdev_register_disk(d);
    return d;
}

static int has(const char *n, uint64_t start, uint64_t count) {
    struct blockdev *p = blockdev_find(n);
    return p && p->start_lba == start && p->sector_count == count;
}

struct gcase { const struct tent *e; int n; int corrupt; };
static void fill_gpt(uint8_t *disk, void *arg) {
    struct gcase *c = (struct gcase *)arg;
    gpt_build(disk, c->e, c->n);
    if (c->corrupt & 1) { disk[1 * T_SS + 16] ^= 0xFF; }            // primary header CRC
    if (c->corrupt & 2) { disk[(T_SECT - 1) * T_SS + 16] ^= 0xFF; } // backup header CRC
}
struct mcase { uint8_t t[4]; uint32_t s[4], c[4]; };
static void fill_mbr(uint8_t *disk, void *arg) { struct mcase *m = arg; mbr_build(disk, m->t, m->s, m->c); }
static void fill_zero(uint8_t *disk, void *arg) { (void)arg; for (uint64_t i = 0; i < (uint64_t)T_SECT * T_SS; i++) { disk[i] = 0; } }

void part_selftest(void) {
    const char *why = 0;
    struct blockdev *d;
    if (crc32(0, "123456789", 9) != 0xCBF43926u) { why = "crc32 known answer"; goto done; }

    // GPT: entries at index 0 and 2 (index 1 empty -- numbering keeps the gap).
    static const struct tent two[] = { { 0, 64, 127 }, { 2, 128, 191 } };
    struct gcase g = { two, 2, 0 };
    d = scan_disk("pga", fill_gpt, &g);
    if (!d || !has("pga1", 64, 64) || !has("pga3", 128, 64) || blockdev_find("pga2")) { why = "gpt valid"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    g.corrupt = 1;                                    // bad primary, good backup
    d = scan_disk("pgb", fill_gpt, &g);
    if (!d || !has("pgb1", 64, 64) || !has("pgb3", 128, 64)) { why = "gpt backup fallback"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    g.corrupt = 3;                                    // both bad, protective MBR -> nothing
    d = scan_disk("pgc", fill_gpt, &g);
    if (!d || blockdev_find("pgc1")) { why = "protective MBR without GPT registered a partition"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    static const struct tent bad[] = { { 0, 64, 127 }, { 1, 100, 150 }, { 2, 900, 5000 } };
    struct gcase gb = { bad, 3, 0 };                  // overlap + past end
    d = scan_disk("pgd", fill_gpt, &gb);
    if (!d || !has("pgd1", 64, 64) || blockdev_find("pgd2") || blockdev_find("pgd3")) { why = "gpt bad entries"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    struct mcase m = { { 0x83, 0x83, 0x05, 0 }, { 64, 256, 512, 0 }, { 128, 128, 64, 0 } };
    d = scan_disk("pma", fill_mbr, &m);
    if (!d || !has("pma1", 64, 128) || !has("pma2", 256, 128) || blockdev_find("pma3")) { why = "mbr primaries/extended"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    struct mcase mo = { { 0x83, 0x83, 0x83, 0 }, { 64, 100, 1000, 0 }, { 128, 50, 100, 0 } };
    d = scan_disk("pmb", fill_mbr, &mo);             // overlap + past end
    if (!d || !has("pmb1", 64, 128) || blockdev_find("pmb2") || blockdev_find("pmb3")) { why = "mbr bad entries"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    struct mcase mf = { { 0x83, 0, 0, 0 }, { 64, 0, 0, 0 }, { 128, 0, 0, 0 } };
    d = ramblk_create("pfa", T_SS, T_SECT);
    if (d) {
        mbr_build(ramblk_data(d), mf.t, mf.s, mf.c);
        ramblk_data(d)[446 + 16] = 'T';   // entry 2's boot byte is message text
        blockdev_register_disk(d);
        if (blockdev_find("pfa1")) { why = "boot-code sector taken for an MBR"; }
    } else { why = "ramblk pfa"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    // A disk re-imaged with an MBR over an old GPT disk: the backup GPT
    // at the end is still valid, but LBA 1 has no signature and the MBR
    // has no protective entry. The MBR is the real table (Linux's rule).
    d = ramblk_create("pgs", T_SS, T_SECT);
    if (d) {
        struct mcase mm = { { 0x83, 0, 0, 0 }, { 300, 0, 0, 0 }, { 100, 0, 0, 0 } };
        uint8_t *raw = ramblk_data(d);
        gpt_build(raw, two, 2);
        // Replace the protective MBR with a real one and wipe the primary
        // header; the backup GPT at the end stays valid.
        for (int i = 0; i < 4; i++) {
            uint8_t *p = raw + 446 + 16 * i;
            for (int k = 0; k < 16; k++) { p[k] = 0; }
            if (mm.t[i]) { p[4] = mm.t[i]; put32(p + 8, mm.s[i]); put32(p + 12, mm.c[i]); }
        }
        for (int i = 0; i < T_SS; i++) { raw[1 * T_SS + i] = 0; }      // no primary header
        blockdev_register_disk(d);
        if (!has("pgs1", 300, 100) || blockdev_find("pgs3")) { why = "stale backup GPT beat a real MBR"; }
    } else { why = "ramblk pgs"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    d = scan_disk("pza", fill_zero, 0);
    if (!d || blockdev_find("pza1")) { why = "bare disk"; }
    ramblk_destroy(d);

done:
    if (why) {
        serial_write_string("[part] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[part] selftest passed\n");
}
