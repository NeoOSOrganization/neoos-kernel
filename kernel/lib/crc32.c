#include "lib/crc32.h"

static uint32_t table[256];
static int ready;

static void build(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) { c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
        table[i] = c;
    }
    ready = 1;
}

uint32_t crc32(uint32_t crc, const void *buf, uint64_t len) {
    if (!ready) { build(); }
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (uint64_t i = 0; i < len; i++) { crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8); }
    return ~crc;
}
