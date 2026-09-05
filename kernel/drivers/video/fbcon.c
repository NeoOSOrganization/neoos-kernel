#include "drivers/video/fbcon.h"
#include "drivers/video/fb_device.h"
#include "drivers/char/serial.h"
#include "tty/con_driver.h"
#include "sync/lock.h"
#include "mm/heap.h"

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
static uint32_t screen_w, screen_h;   // active mode's pixel dimensions
static uint32_t *shadow;              // screen_w * screen_h canonical XRGB8888
static struct spinlock fbcon_lock;

// One-way switch, set by the exception path before it repaints. The
// panicking CPU may hold any lock, so a rank-checked acquire here would
// call lock_panic() from inside a panic and recurse. lock.c solves the
// same problem for serial output with panic_puts(), which skips the
// lock entirely; this keeps the lock (another CPU may be mid-paint) but
// drops the rank check. Never cleared: a panic is terminal.
static volatile int fbcon_panicking;

void fbcon_enter_panic(void) { fbcon_panicking = 1; }

static uint64_t fbcon_acquire(void) {
    return fbcon_panicking ? spin_lock_raw(&fbcon_lock)
                           : spin_lock_irqsave(&fbcon_lock);
}
static void fbcon_release(uint64_t f) {
    if (fbcon_panicking) { spin_unlock_raw(&fbcon_lock, f); }
    else                 { spin_unlock_irqrestore(&fbcon_lock, f); }
}

// Renders one glyph into the shadow buffer at pixel (px,py). Never
// touches the device -- present_rect does that.
static void render_glyph(uint32_t px, uint32_t py, unsigned char ch, uint32_t fg, uint32_t bg) {
    const uint8_t *g = font8x16[ch];
    for (uint32_t y = 0; y < GLYPH_H; y++) {
        uint8_t bits = g[y];
        uint32_t *row = shadow + (py + y) * screen_w + px;
        for (uint32_t x = 0; x < GLYPH_W; x++) {
            row[x] = (bits >> (7 - x)) & 1 ? fg : bg;
        }
    }
}

// Pushes a rectangle of the shadow buffer to whatever device is
// active. The one and only place this file touches fb_device.
static void present_rect(uint32_t px, uint32_t py, uint32_t w, uint32_t h) {
    struct fb_device *d = fb_device_active();
    if (!d || !d->blit) { return; }
    d->blit(shadow + py * screen_w + px, screen_w * 4, (int)px, (int)py, (int)w, (int)h);
}

static void put_glyph_fg(uint32_t gx, uint32_t gy, unsigned char ch, uint32_t fg) {
    uint32_t px = gx * GLYPH_W, py = gy * GLYPH_H;
    render_glyph(px, py, ch, fg, BG);
    present_rect(px, py, GLYPH_W, GLYPH_H);
}

static void put_glyph(uint32_t gx, uint32_t gy, unsigned char ch) {
    put_glyph_fg(gx, gy, ch, FG);
}

// Shift the shadow buffer up one text row (plain memory -- no device
// access), then present the whole screen. Scrolling is rare next to
// per-character typing; blitting only the newly-revealed strip is a
// later optimization if `make run` ever shows this one is too slow --
// not needed to make the migration correct.
static void scroll_one(void) {
    uint32_t row_pixels = screen_w * GLYPH_H;
    uint32_t total_pixels = screen_w * screen_h;
    for (uint32_t i = 0; i + row_pixels < total_pixels; i++) {
        shadow[i] = shadow[i + row_pixels];
    }
    for (uint32_t i = total_pixels - row_pixels; i < total_pixels; i++) {
        shadow[i] = 0;
    }
    present_rect(0, 0, screen_w, screen_h);
}

static void clear_locked(void) {
    for (uint32_t i = 0; i < screen_w * screen_h; i++) { shadow[i] = 0; }
    present_rect(0, 0, screen_w, screen_h);
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

void fbcon_clear(void) {
    if (!shadow) { return; }
    uint64_t f = fbcon_acquire();
    clear_locked();
    fbcon_release(f);
}

void fbcon_init(void) {
    struct fb_device *d = fb_device_active();
    if (!d) { return; }
    struct fb_mode m;
    d->current(&m, 0, 0);
    screen_w = m.width;
    screen_h = m.height;
    shadow = kmalloc((size_t)screen_w * (size_t)screen_h * 4);
    if (!shadow) {
        serial_write_string("[fbcon] kmalloc failed -- console disabled\n");
        return;
    }
    spin_init(&fbcon_lock, LOCK_RANK_FBCON, "fbcon");
    cols = screen_w / GLYPH_W;
    rows = screen_h / GLYPH_H;
    fbcon_clear();
    serial_write_string("[fbcon] ");
    serial_write_hex64(cols);
    serial_write_string("x");
    serial_write_hex64(rows);
    serial_write_string(" cells\n");
}

// ---- con_driver ----------------------------------------------------

static int fbcon_probe(void) { return fb_device_active() != 0; }

static void fbcon_init_cd(int *c, int *r) {
    fbcon_init();
    *c = (int)cols;
    *r = (int)rows;
}

static void fbcon_putc_attr(char c, uint8_t fg) {
    if (!shadow) { return; }
    uint32_t rgb = con_rgb[fg & 15];
    uint64_t f = fbcon_acquire();
    putc_locked_fg(c, rgb);
    fbcon_release(f);
}

// --- grid-addressed (M1c-3 VT layer) --------------------------------

static void put_cell(uint32_t gx, uint32_t gy, unsigned char ch,
                     uint32_t fgrgb, uint32_t bgrgb) {
    uint32_t px = gx * GLYPH_W, py = gy * GLYPH_H;
    render_glyph(px, py, ch, fgrgb, bgrgb);
    present_rect(px, py, GLYPH_W, GLYPH_H);
}

static int cur_row = -1, cur_col = -1;   // where the software cursor is drawn

static void fbcon_putc_at(int row, int col, char ch, uint8_t attr) {
    if (!shadow) { return; }
    if (row < 0 || col < 0 || (uint32_t)row >= rows || (uint32_t)col >= cols) { return; }
    uint32_t fg = con_rgb[attr & 15];
    uint32_t bg = con_rgb[(attr >> 4) & 15];
    uint64_t f = fbcon_acquire();
    put_cell((uint32_t)col, (uint32_t)row, (unsigned char)(ch ? ch : ' '), fg, bg);
    if (row == cur_row && col == cur_col) { cur_row = cur_col = -1; }  // painted over
    fbcon_release(f);
}

static void fbcon_cursor(int row, int col, int visible) {
    if (!shadow) { return; }
    uint64_t f = fbcon_acquire();
    // erase the old cursor block by filling it solid black (the cell
    // under it is repainted by vt.c's diff when its content changes).
    if (cur_row >= 0 && (cur_row != row || cur_col != col || !visible)) {
        uint32_t px = (uint32_t)cur_col * GLYPH_W, py = (uint32_t)cur_row * GLYPH_H;
        for (uint32_t y = GLYPH_H - 3; y < GLYPH_H; y++) {
            uint32_t *r = shadow + (py + y) * screen_w + px;
            for (uint32_t x = 0; x < GLYPH_W; x++) { r[x] = BG; }
        }
        present_rect(px, py + GLYPH_H - 3, GLYPH_W, 3);
        cur_row = cur_col = -1;
    }
    if (visible && row >= 0 && col >= 0 &&
        (uint32_t)row < rows && (uint32_t)col < cols) {
        uint32_t px = (uint32_t)col * GLYPH_W, py = (uint32_t)row * GLYPH_H;
        for (uint32_t y = GLYPH_H - 3; y < GLYPH_H; y++) {   // underline-style block
            uint32_t *r = shadow + (py + y) * screen_w + px;
            for (uint32_t x = 0; x < GLYPH_W; x++) { r[x] = FG; }
        }
        present_rect(px, py + GLYPH_H - 3, GLYPH_W, 3);
        cur_row = row; cur_col = col;
    }
    fbcon_release(f);
}

struct con_driver fbcon_drv = {
    .name = "fbcon", .priority = 100,
    .probe = fbcon_probe, .init = fbcon_init_cd,
    .putc_attr = fbcon_putc_attr,
    .putc_at = fbcon_putc_at, .cursor = fbcon_cursor,
    .clear = fbcon_clear,
};

void fbcon_selftest(void) {
    if (!shadow) {
        serial_write_string("[fbcon] selftest skipped (no framebuffer)\n");
        return;
    }
    uint64_t f = fbcon_acquire();
    render_glyph(0, 0, 'A', FG, BG);
    uint32_t got = shadow[9 * screen_w + 1];   // a set bit of the 'A' crossbar
    clear_locked();
    fbcon_release(f);

    if (got != FG) {
        serial_write_string("[fbcon] selftest FAILED: glyph readback ");
        serial_write_hex64(got);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[fbcon] selftest passed\n");
}
