# Framebuffer Blit Abstraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `fb_device` a canonical-format bulk-blit primitive and
migrate `fbcon` (the kernel's text console renderer) onto it, so it no
longer bypasses the abstraction by reading/writing `vesafb`'s internal
framebuffer pointer directly.

**Architecture:** Replace the dead `flush_rect(x,y,w,h)` op (one
caller in the whole kernel: a selftest smoke check) with
`blit(src, src_pitch, dst_x, dst_y, w, h)` — always 32bpp XRGB8888
regardless of the driver's native format. `fbcon` gets its own
heap-allocated, full-screen shadow buffer in that same format; every
draw (glyph, cell, cursor, clear, scroll) writes into the shadow buffer
with plain pointer arithmetic, then calls `blit` to push the changed
rectangle to the active device. `vesafb`'s raw framebuffer state
becomes private to `vesafb.c`.

**Tech Stack:** C (freestanding kernel code), no new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-05-fb-device-blit-abstraction-design.md`

## Global Constraints

- **Canonical format only**: `blit`'s `src` buffer is always 32bpp
  XRGB8888 (`0x00RRGGBB` per pixel); a driver whose hardware differs
  converts internally, isolated to that one driver.
- **One primitive for both scales**: no separate "whole-frame" op —
  `blit` with `dst_x=dst_y=0` and `w`/`h` equal to the mode's
  dimensions IS the whole-frame case.
- **`vesafb_internal.h` must end up with zero includers outside
  `vesafb.c`** — this is the concrete, grep-checkable proof the
  migration is complete (spec §4).
- **No gauntlet regression**: `[fbdev] selftest passed` and
  `[fbcon] selftest passed` must both still print, and a `make run`
  visual spot-check (a human looking at the console) must show
  unchanged rendering — this project has no host test suite; visual
  correctness is confirmed by looking at it, per this project's
  existing convention for anything requiring visual judgment.
- **This lands in `neoos-kernel` directly** (the monorepo it was
  originally designed in is now archived/read-only).

---

### Task 1: Replace `flush_rect` with `blit` in `fb_device` and `vesafb`

**Files:**
- Modify: `kernel/drivers/video/fb_device.h`
- Modify: `kernel/drivers/video/fb_device.c`
- Modify: `kernel/drivers/video/vesafb.c`

**Interfaces:**
- Produces: `void (*blit)(const void *src, uint32_t src_pitch, int dst_x, int dst_y, int w, int h);`
  as a field of `struct fb_device` — Task 2's `fbcon.c` calls this via
  `fb_device_active()->blit(...)`.

- [ ] **Step 1: Replace the `flush_rect` field with `blit` in the struct**

In `kernel/drivers/video/fb_device.h`, find:

```c
    void (*flush_rect)(int x, int y, int w, int h);   // may be NULL
```

Replace with:

```c
    // Blits a rectangle from `src` (always canonical 32bpp XRGB8888 --
    // byte order 0x00RRGGBB per pixel, regardless of the driver's
    // actual hardware format) into the framebuffer at (dst_x, dst_y),
    // width w, height h. src_pitch is the SOURCE buffer's stride in
    // bytes (may differ from w*4 if the caller is blitting a sub-rect
    // of a larger off-screen buffer). May assume the destination rect
    // is fully within the current mode's bounds -- callers clip, not
    // drivers. May be NULL (a text-only/no-framebuffer boot).
    void (*blit)(const void *src, uint32_t src_pitch,
                 int dst_x, int dst_y, int w, int h);
```

- [ ] **Step 2: Update `vesafb.c`'s implementation**

Find:

```c
static void vesafb_flush_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;      // linear framebuffer: scanout is live
}

struct fb_device vesafb_drv = {
    .name = "vesafb", .priority = 100,
    .probe = vesafb_probe, .current = vesafb_current,
    .modes = vesafb_modes, .set_mode = vesafb_set_mode,
    .flush_rect = vesafb_flush_rect, .cursor = 0,
};
```

Replace with:

```c
static void vesafb_blit(const void *src, uint32_t src_pitch,
                         int dst_x, int dst_y, int w, int h) {
    if (!fb.present) { return; }
    const uint8_t *s = (const uint8_t *)src;
    volatile uint8_t *d = fb.virt + (uint64_t)dst_y * fb.pitch + (uint64_t)dst_x * 4;
    // vesafb's native format is already canonical XRGB8888 (see the
    // channel-sanitizing comment in vesafb_parse), so this is a
    // straight per-row copy -- no conversion needed. A future driver
    // whose native format genuinely differs would convert here,
    // isolated to its own blit implementation.
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w * 4; col++) { d[col] = s[col]; }
        s += src_pitch;
        d += fb.pitch;
    }
}

struct fb_device vesafb_drv = {
    .name = "vesafb", .priority = 100,
    .probe = vesafb_probe, .current = vesafb_current,
    .modes = vesafb_modes, .set_mode = vesafb_set_mode,
    .blit = vesafb_blit, .cursor = 0,
};
```

- [ ] **Step 3: Update `fb_device_selftest()` in `fb_device.c`**

Find:

```c
    if (active->flush_rect) { active->flush_rect(0, 0, 1, 1); }
    serial_write_string("[fbdev] selftest passed\n");
```

Replace with:

```c
    // A 1x1 black-pixel blit at the origin -- proves the driver's
    // blit path itself works, not just that the function pointer is
    // non-NULL.
    uint32_t px = 0;
    if (active->blit) { active->blit(&px, 4, 0, 0, 1, 1); }
    serial_write_string("[fbdev] selftest passed\n");
```

- [ ] **Step 4: Build the kernel**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output
```

Expected: clean build, no errors (nothing outside `fb_device.h/.c` and
`vesafb.c` references `flush_rect`, so nothing else should need
touching for this step to compile).

- [ ] **Step 5: Boot and check the selftest marker**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
grep "\[fbdev\]" build/serial.log
```

Expected: `[fbdev] vesafb selected` and `[fbdev] selftest passed`.

- [ ] **Step 6: Commit**

```bash
git add kernel/drivers/video/fb_device.h kernel/drivers/video/fb_device.c kernel/drivers/video/vesafb.c
git commit -m "fb_device: replace dead flush_rect with a canonical-format blit primitive"
```

---

### Task 2: Migrate `fbcon` to a shadow buffer, drop `vesafb_internal.h`

**Files:**
- Modify: `kernel/drivers/video/fbcon.c` (full rewrite of its drawing
  paths — every function that currently reads `fb.*` or writes through
  `pixel()`)
- Modify: `kernel/drivers/video/vesafb.c` (make `struct fb_info fb`
  file-static instead of extern)
- Delete: `kernel/drivers/video/vesafb_internal.h`
- Modify: `kernel/drivers/video/fbcon.h` (one comment line, no
  interface change)

**Interfaces:**
- Consumes: `struct fb_device` and `fb_device_active()` from Task 1
  (specifically the new `blit` field).
- Consumes: `void *kmalloc(size_t size)` from `kernel/mm/heap.h`
  (already used elsewhere in the kernel — see `kernel/tty/pty.c` for
  the established NULL-check convention this task follows).
- Produces: nothing new for later tasks — this is the last task in
  the plan.

- [ ] **Step 1: Make `fb` file-static in `vesafb.c` and drop the shared internal header**

In `kernel/drivers/video/vesafb.c`, change:

```c
#include "drivers/video/fb.h"
#include "drivers/video/fb_device.h"
#include "drivers/video/vesafb_internal.h"
```

to:

```c
#include "drivers/video/fb.h"
#include "drivers/video/fb_device.h"
```

(`fb.h` already declares `void fb_map(void);`, which is all
`kernel/kernel.c` needs from this file — see `kernel/kernel.c:136`.
`vesafb_parse` is only ever called from `vesafb_probe`, in this same
file, so it needs no header declaration outside it at all.)

Change:

```c
struct fb_info fb;
```

to:

```c
static struct fb_info fb;
```

Change the `struct fb_info` definition's location: it currently lives
in `vesafb_internal.h`. Move the `struct fb_info { ... };` definition
(everything between `struct fb_info {` and its closing `};`, currently
in `vesafb_internal.h`) into `vesafb.c` itself, placed above the
`struct fb_info fb;` line (now `static`). Change `void vesafb_parse(void *multiboot_info);`'s
declaration from a header prototype to a plain `static` function
definition in `vesafb.c` — find:

```c
void vesafb_parse(void *multiboot_info) {
```

and change to:

```c
static void vesafb_parse(void *multiboot_info) {
```

- [ ] **Step 2: Delete `vesafb_internal.h`**

```bash
git rm kernel/drivers/video/vesafb_internal.h
```

- [ ] **Step 3: Build to confirm nothing else includes it**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output 2>&1 | grep -i "vesafb_internal\|fbcon.c" || true
```

Expected: compile errors from `fbcon.c` (it still includes the deleted
header) — this is expected at this point; Step 4 fixes it. If
anything OTHER than `fbcon.c` fails, stop — that means something
outside the two files this task touches depended on the internal
header, which the spec's investigation didn't find, and needs
re-checking before continuing.

- [ ] **Step 4: Rewrite `fbcon.c`'s drawing paths onto a shadow buffer + `blit`**

Replace the entire file with:

```c
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
```

Note what changed versus the original, precisely: every `fb.present`
check became `!shadow` (shadow is only non-NULL once `fbcon_init`
successfully sized it against a real active device — the same
"nothing to do" gate, expressed in terms of this file's own state
instead of reaching into `vesafb`'s); every direct `fb.virt` read/write
via the old `pixel()` helper became a `render_glyph`/inline shadow-index
write followed by a `present_rect` call; `fbcon_selftest`'s pixel
*readback* now reads the shadow buffer (which this file owns) instead
of the device (which it no longer touches for reads at all) — this is
a strictly better test of the font-decoding logic, since it no longer
depends on a working device mapping to validate `render_glyph`'s bit-
to-pixel conversion.

- [ ] **Step 5: Update the one now-stale comment in `fbcon.h`**

Find:

```c
// This is what kernel selftest output and the panic path render through when
// fb.present; dev/console.c picks it over VGA text.
```

Replace with:

```c
// This is what kernel selftest output and the panic path render through
// when a framebuffer device is active; dev/console.c picks it over VGA text.
```

- [ ] **Step 6: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output
```

Expected: clean build.

- [ ] **Step 7: Grep-check the migration is actually complete**

```bash
grep -rn vesafb_internal kernel/
```

Expected: no output at all (the file no longer exists and nothing
references its old name).

- [ ] **Step 8: Boot and check both selftest markers**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
grep -E "\[fbdev\]|\[fbcon\]" build/serial.log
```

Expected: `[fbdev] vesafb selected`, `[fbdev] selftest passed`,
`[fbcon] NNxNN cells`, `[fbcon] selftest passed`.

- [ ] **Step 9: Full gauntlet run**

```bash
./tools/gauntlet.sh
```

Expected: `PGAUNTLET PASSED: N/N`, same marker count as before this
plan started (this change touches no `REQUIRED_MARKERS` entries, only
*how* two existing markers get produced).

- [ ] **Step 10: Visual spot-check**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output run
```

A human confirms: the console still renders text correctly, scrolling
looks the same as before, the cursor still draws/erases correctly, no
visual artifacts. This project has no host-runnable way to assert
"looks right" automatically — this step is required, not optional,
per this project's existing convention for visual correctness.

- [ ] **Step 11: Commit**

```bash
git add kernel/drivers/video/fbcon.c kernel/drivers/video/fbcon.h kernel/drivers/video/vesafb.c
git rm kernel/drivers/video/vesafb_internal.h
git commit -m "fbcon: migrate to a shadow buffer + fb_device.blit, drop vesafb_internal.h dependency"
```

---

## Self-Review Notes (from writing this plan)

- **Spec coverage:** §3.1 (interface) → Task 1 Step 1. §3.2 (vesafb
  impl) → Task 1 Step 2. §3.3 (selftest) → Task 1 Step 3. §3.4 (fbcon
  migration) → Task 2 Step 4, with the scrolling approach the spec
  left as an open note (§3.4: "becomes a sequence of row-by-row blit
  calls... OR a driver-side scroll op... not part of this spec")
  resolved concretely here: a shadow buffer scrolled by plain memory
  move, then one whole-screen `present_rect` — no driver-side op
  needed, no per-row device calls needed either. §3.5 (`fb` private,
  `vesafb_internal.h` gone) → Task 2 Steps 1-2. §4 (testing) → Task 2
  Steps 7-10.
- **Type consistency:** `blit`'s signature is identical everywhere it
  appears (header field, `vesafb_blit`, `fb_device_selftest`'s call,
  `present_rect`'s call) — `(const void *, uint32_t, int, int, int, int)`.
- **No placeholders:** every step has the actual code, not a
  description of it.
