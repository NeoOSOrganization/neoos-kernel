#include "dev/fbcon.h"
#include "dev/fb.h"
#include "dev/serial.h"

extern const uint8_t font8x16[256][16];

#define GLYPH_W 8
#define GLYPH_H 16
#define FG 0x00c8c8c8u   // light grey
#define BG 0x00000000u   // black

static uint32_t cols, rows, cx, cy;

static inline volatile uint32_t *pixel(uint32_t px, uint32_t py) {
    return (volatile uint32_t *)(fb.virt + (uint64_t)py * fb.pitch + (uint64_t)px * 4);
}

static void put_glyph(uint32_t gx, uint32_t gy, unsigned char ch) {
    const uint8_t *g = font8x16[ch];
    for (uint32_t y = 0; y < GLYPH_H; y++) {
        uint8_t bits = g[y];
        for (uint32_t x = 0; x < GLYPH_W; x++) {
            *pixel(gx * GLYPH_W + x, gy * GLYPH_H + y) =
                (bits >> (7 - x)) & 1 ? FG : BG;
        }
    }
}

static void scroll_one(void) {
    volatile uint8_t *base = fb.virt;
    uint64_t row_bytes = (uint64_t)fb.pitch * GLYPH_H;
    uint64_t moved = (uint64_t)(rows - 1) * row_bytes;
    for (uint64_t i = 0; i < moved; i++) { base[i] = base[i + row_bytes]; }
    for (uint64_t i = moved; i < (uint64_t)rows * row_bytes; i++) { base[i] = 0; }
}

void fbcon_clear(void) {
    if (!fb.present) { return; }
    for (uint64_t i = 0; i < fb.size; i++) { fb.virt[i] = 0; }
    cx = cy = 0;
}

void fbcon_init(void) {
    if (!fb.present) { return; }
    cols = fb.width / GLYPH_W;
    rows = fb.height / GLYPH_H;
    fbcon_clear();
    serial_write_string("[fbcon] ");
    serial_write_hex64(cols);
    serial_write_string("x");
    serial_write_hex64(rows);
    serial_write_string(" cells\n");
}

void fbcon_putc(char c) {
    if (!fb.present) { return; }
    if (c == '\n')      { cx = 0; cy++; }
    else if (c == '\r') { cx = 0; }
    else if (c == '\b') { if (cx) { cx--; put_glyph(cx, cy, ' '); } }
    else if (c == '\t') { cx = (cx + 8u) & ~7u; }
    else                { put_glyph(cx, cy, (unsigned char)c); cx++; }

    if (cx >= cols) { cx = 0; cy++; }
    if (cy >= rows) { scroll_one(); cy = rows - 1; }
}

void fbcon_write(const char *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { fbcon_putc(s[i]); }
}

void fbcon_selftest(void) {
    if (!fb.present) {
        serial_write_string("[fbcon] selftest skipped (no framebuffer)\n");
        return;
    }
    // Draw 'A' at the origin and read back a pixel that its glyph sets
    // (row 9, column 1 -- the left stem of the crossbar area).
    put_glyph(0, 0, 'A');
    volatile uint32_t *p = pixel(1, 9);
    uint32_t got = *p;
    fbcon_clear();
    if (got != FG) {
        serial_write_string("[fbcon] selftest FAILED: glyph readback ");
        serial_write_hex64(got);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[fbcon] selftest passed\n");
}
