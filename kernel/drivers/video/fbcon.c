#include "drivers/video/fbcon.h"
#include "drivers/video/fb.h"
#include "drivers/video/fb_device.h"
#include "drivers/video/vesafb_internal.h"
#include "drivers/char/serial.h"
#include "tty/con_driver.h"
#include "sync/lock.h"

extern const uint8_t font8x16[256][16];

#define GLYPH_W 8
#define GLYPH_H 16
#define FG 0x00c8c8c8u   // light grey (default)
#define BG 0x00000000u   // black

// CON_* (0..15) -> 0x00RRGGBB, standard VGA palette.
static const uint32_t con_rgb[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

static uint32_t cols, rows, cx, cy;
static struct spinlock fbcon_lock;

static inline volatile uint32_t *pixel(uint32_t px, uint32_t py) {
    return (volatile uint32_t *)(fb.virt + (uint64_t)py * fb.pitch + (uint64_t)px * 4);
}

static void put_glyph_fg(uint32_t gx, uint32_t gy, unsigned char ch, uint32_t fg) {
    const uint8_t *g = font8x16[ch];
    for (uint32_t y = 0; y < GLYPH_H; y++) {
        uint8_t bits = g[y];
        for (uint32_t x = 0; x < GLYPH_W; x++) {
            *pixel(gx * GLYPH_W + x, gy * GLYPH_H + y) =
                (bits >> (7 - x)) & 1 ? fg : BG;
        }
    }
}

static void put_glyph(uint32_t gx, uint32_t gy, unsigned char ch) {
    put_glyph_fg(gx, gy, ch, FG);
}

// Shift the visible area up one text row. 64-bit stores -- a byte loop
// over 4 MiB is far too slow to run on every newline.
static void scroll_one(void) {
    volatile uint64_t *base = (volatile uint64_t *)fb.virt;
    uint64_t row_words = ((uint64_t)fb.pitch * GLYPH_H) / 8;
    uint64_t total_words = row_words * rows;
    for (uint64_t i = 0; i + row_words < total_words; i++) {
        base[i] = base[i + row_words];
    }
    for (uint64_t i = total_words - row_words; i < total_words; i++) {
        base[i] = 0;
    }
}

static void clear_locked(void) {
    volatile uint64_t *p = (volatile uint64_t *)fb.virt;
    for (uint64_t i = 0; i < fb.size / 8; i++) { p[i] = 0; }
    cx = cy = 0;
}

static void putc_locked_fg(char c, uint32_t fg) {
    if (c == '\n')      { cx = 0; cy++; }
    else if (c == '\r') { cx = 0; }
    else if (c == '\b') { if (cx) { cx--; put_glyph(cx, cy, ' '); } }
    else if (c == '\t') { cx = (cx + 8u) & ~7u; }
    else                { put_glyph_fg(cx, cy, (unsigned char)c, fg); cx++; }

    if (cx >= cols) { cx = 0; cy++; }
    if (cy >= rows) { scroll_one(); cy = rows - 1; }
}

static void putc_locked(char c) { putc_locked_fg(c, FG); }

void fbcon_clear(void) {
    if (!fb.present) { return; }
    uint64_t f = spin_lock_raw(&fbcon_lock);
    clear_locked();
    spin_unlock_raw(&fbcon_lock, f);
}

void fbcon_init(void) {
    if (!fb.present) { return; }
    spin_init(&fbcon_lock, LOCK_RANK_FBCON, "fbcon");
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
    uint64_t f = spin_lock_raw(&fbcon_lock);
    putc_locked(c);
    spin_unlock_raw(&fbcon_lock, f);
}

void fbcon_write(const char *s, uint64_t n) {
    if (!fb.present) { return; }
    uint64_t f = spin_lock_raw(&fbcon_lock);
    for (uint64_t i = 0; i < n; i++) { putc_locked(s[i]); }
    spin_unlock_raw(&fbcon_lock, f);
}

// ---- con_driver ----------------------------------------------------

uint32_t fbcon_cols(void) { return cols; }
uint32_t fbcon_rows(void) { return rows; }

static int fbcon_probe(void) { return fb_device_active() != 0; }

static void fbcon_init_cd(int *c, int *r) {
    fbcon_init();
    *c = (int)cols;
    *r = (int)rows;
}

static void fbcon_putc_attr(char c, uint8_t fg) {
    if (!fb.present) { return; }
    uint32_t rgb = con_rgb[fg & 15];
    uint64_t f = spin_lock_raw(&fbcon_lock);
    putc_locked_fg(c, rgb);
    spin_unlock_raw(&fbcon_lock, f);
}

// --- grid-addressed (M1c-3 VT layer) --------------------------------

// draw one glyph at cell (row,col) with fg/bg from the attr byte.
static void put_cell(uint32_t gx, uint32_t gy, unsigned char ch,
                     uint32_t fgrgb, uint32_t bgrgb) {
    const uint8_t *g = font8x16[ch];
    for (uint32_t y = 0; y < GLYPH_H; y++) {
        uint8_t bits = g[y];
        for (uint32_t x = 0; x < GLYPH_W; x++) {
            *pixel(gx * GLYPH_W + x, gy * GLYPH_H + y) =
                (bits >> (7 - x)) & 1 ? fgrgb : bgrgb;
        }
    }
}

static int cur_row = -1, cur_col = -1;   // where the software cursor is drawn

static void fbcon_putc_at(int row, int col, char ch, uint8_t attr) {
    if (!fb.present) { return; }
    if (row < 0 || col < 0 || (uint32_t)row >= rows || (uint32_t)col >= cols) { return; }
    uint32_t fg = con_rgb[attr & 15];
    uint32_t bg = con_rgb[(attr >> 4) & 15];
    uint64_t f = spin_lock_raw(&fbcon_lock);
    put_cell((uint32_t)col, (uint32_t)row, (unsigned char)(ch ? ch : ' '), fg, bg);
    if (row == cur_row && col == cur_col) { cur_row = cur_col = -1; }  // painted over
    spin_unlock_raw(&fbcon_lock, f);
}

static void fbcon_cursor(int row, int col, int visible) {
    if (!fb.present) { return; }
    uint64_t f = spin_lock_raw(&fbcon_lock);
    // erase the old cursor block by filling it solid black (the cell
    // under it is repainted by vt.c's diff when its content changes).
    if (cur_row >= 0 && (cur_row != row || cur_col != col || !visible)) {
        for (uint32_t y = 0; y < GLYPH_H; y++) {
            for (uint32_t x = 0; x < GLYPH_W; x++) {
                *pixel((uint32_t)cur_col * GLYPH_W + x, (uint32_t)cur_row * GLYPH_H + y) = BG;
            }
        }
        cur_row = cur_col = -1;
    }
    if (visible && row >= 0 && col >= 0 &&
        (uint32_t)row < rows && (uint32_t)col < cols) {
        for (uint32_t y = GLYPH_H - 3; y < GLYPH_H; y++) {   // underline-style block
            for (uint32_t x = 0; x < GLYPH_W; x++) {
                *pixel((uint32_t)col * GLYPH_W + x, (uint32_t)row * GLYPH_H + y) = FG;
            }
        }
        cur_row = row; cur_col = col;
    }
    spin_unlock_raw(&fbcon_lock, f);
}

struct con_driver fbcon_drv = {
    .name = "fbcon", .priority = 100,
    .probe = fbcon_probe, .init = fbcon_init_cd,
    .putc_attr = fbcon_putc_attr,
    .putc_at = fbcon_putc_at, .cursor = fbcon_cursor,
    .clear = fbcon_clear,
};

void fbcon_selftest(void) {
    if (!fb.present) {
        serial_write_string("[fbcon] selftest skipped (no framebuffer)\n");
        return;
    }
    uint64_t f = spin_lock_raw(&fbcon_lock);
    put_glyph(0, 0, 'A');
    volatile uint32_t *p = pixel(1, 9);   // a set bit of the 'A' crossbar
    uint32_t got = *p;
    clear_locked();
    spin_unlock_raw(&fbcon_lock, f);

    if (got != FG) {
        serial_write_string("[fbcon] selftest FAILED: glyph readback ");
        serial_write_hex64(got);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[fbcon] selftest passed\n");
}
