#include "drivers/block/ata_id.h"
#include "drivers/char/serial.h"

// Two characters per word, the first in the HIGH byte; the field is
// space-padded on the right.
static void get_str(const uint16_t *w, int first, int words, char *out) {
    int n = 0;
    for (int k = 0; k < words; k++) {
        out[n++] = (char)(w[first + k] >> 8);
        out[n++] = (char)(w[first + k] & 0xFF);
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) { n--; }
    out[n] = 0;
}

void ata_id_parse(const uint16_t w[256], struct ata_id *out) {
    out->atapi = (w[0] & 0x8000) != 0;
    out->lba48 = (w[83] & (1u << 10)) != 0;
    uint64_t lba28 = (uint64_t)w[61] << 16 | w[60];
    uint64_t lba48 = (uint64_t)w[103] << 48 | (uint64_t)w[102] << 32 | (uint64_t)w[101] << 16 | w[100];
    out->sectors = (out->lba48 && lba48) ? lba48 : lba28;

    // Word 106 is valid only when bits 15:14 read 01; bit 12 then says
    // words 117-118 hold the logical sector size, in WORDS.
    out->logical_sector = 512;
    if ((w[106] & 0xC000) == 0x4000 && (w[106] & (1u << 12))) {
        uint32_t words = (uint32_t)w[118] << 16 | w[117];
        if (words) { out->logical_sector = words * 2; }
    }

    out->ncq = (w[76] & (1u << 8)) != 0;
    out->queue_depth = out->ncq ? (uint8_t)((w[75] & 0x1F) + 1) : 1;
    out->fua = (w[84] & (1u << 6)) != 0;
    out->write_cache = (w[85] & (1u << 5)) != 0;
    get_str(w, 10, 10, out->serial);
    get_str(w, 27, 20, out->model);
}

static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

// ATA strings store two characters per word, the FIRST in the high byte.
static void put_str(uint16_t *w, int first, int words, const char *s) {
    int i = 0;
    for (int k = 0; k < words; k++) {
        char hi = s[i] ? s[i++] : ' ';
        char lo = s[i] ? s[i++] : ' ';
        w[first + k] = (uint16_t)((uint8_t)hi << 8 | (uint8_t)lo);
    }
}

void ata_id_selftest(void) {
    static uint16_t w[256];
    struct ata_id id;
    const char *why = 0;

    for (int i = 0; i < 256; i++) { w[i] = 0; }
    w[60] = 0xFFFF; w[61] = 0x0FFF;                 // LBA28: 0x0FFFFFFF
    w[83] = 1u << 10;                               // LBA48 supported
    w[100] = 0; w[101] = 0; w[102] = 1; w[103] = 0; // 1 << 32 sectors
    w[106] = 0x5000;                                // valid, logical sector > 256 words
    w[117] = 2048; w[118] = 0;                      // 2048 words = 4096 bytes
    w[76] = 1u << 8; w[75] = 31;                    // NCQ, depth 32
    w[84] = 1u << 6;                                // FUA
    w[85] = 1u << 5;                                // write cache on
    put_str(w, 10, 10, "NEOOSSCRATCH");
    put_str(w, 27, 20, "QEMU HARDDISK");
    ata_id_parse(w, &id);
    if      (id.sectors != 1ULL << 32)             { why = "lba48 sector count"; }
    else if (!id.lba48)                            { why = "lba48 flag"; }
    else if (id.logical_sector != 4096)            { why = "logical sector size"; }
    else if (!id.ncq || id.queue_depth != 32)      { why = "ncq depth"; }
    else if (!id.fua || !id.write_cache)           { why = "fua / write cache"; }
    else if (id.atapi)                             { why = "atapi on a disk"; }
    else if (!str_eq(id.model, "QEMU HARDDISK"))   { why = "model string"; }
    else if (!str_eq(id.serial, "NEOOSSCRATCH"))   { why = "serial string"; }

    if (!why) {
        w[83] = 0; w[106] = 0; w[76] = 0; w[0] = 0x8580;   // LBA28 only; a packet device
        ata_id_parse(w, &id);
        if      (id.sectors != 0x0FFFFFFF)         { why = "lba28 sector count"; }
        else if (id.lba48)                         { why = "lba48 without word 83"; }
        else if (id.logical_sector != 512)         { why = "default sector size"; }
        else if (id.ncq || id.queue_depth != 1)    { why = "ncq without word 76"; }
        else if (!id.atapi)                        { why = "packet device"; }
    }
    if (why) {
        serial_write_string("[ata_id] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[ata_id] selftest passed\n");
}
