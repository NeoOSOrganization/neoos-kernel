# Framebuffer Abstraction: A Canonical Blit Primitive — Design

**Date:** 2026-09-05
**Status:** approved, ready for implementation
**Trigger:** wanting to port Doom (as a separate `neoos-doom` port repo,
following the same submodule+manifest pattern as `neoos-busybox` and
`neoos-3d-ascii-viewer`) surfaced that `fbcon`, the kernel's own text
console renderer, bypasses the existing `fb_device` abstraction
entirely and writes straight into `vesafb`'s internal framebuffer
pointer. Before building anything that draws to the screen, the
abstraction needs to actually hold for its one existing consumer.

## 1. Problem

`kernel/drivers/video/fb_device.h` already defines a real abstraction:
a `struct fb_device` ops table (`probe`, `current`, `modes`,
`set_mode`, `flush_rect`, `cursor`) with priority-based auto-probing,
explicitly designed so "a bochs-drm or virtio-gpu driver could add
itself later without touching any caller." `/dev/fb0` (the userland
surface, `fb.h`) goes through it correctly.

`fbcon.c` does not. Its own header comment admits it:
*"fbcon is the one exception — it draws straight into fb.virt."* It
`#include`s `vesafb_internal.h` — a header explicitly documented as
private to `vesafb.c` — and reads/writes the raw `struct fb_info fb`
global (`fb.virt`, `fb.pitch`, `fb.width`, `fb.height`, `fb.present`)
for every glyph draw, scroll, and cursor update.

The existing `flush_rect(int x, int y, int w, int h)` op has exactly
one caller in the entire kernel: `fb_device_selftest()`, which invokes
it as `active->flush_rect(0, 0, 1, 1)` — a smoke check that the
function pointer doesn't crash, not a real rendering path. It carries
no buffer argument, so it was never capable of being fbcon's real
write path in the first place.

Net effect: the abstraction has one real implementation (`vesafb`) and
one real consumer (`fbcon`), and the consumer doesn't use it. Adding a
second heavy consumer (a Doom port, redrawing a full frame constantly)
on top of that would either repeat the same bypass or be the first
thing to discover the abstraction doesn't actually work.

## 2. Scope

**In scope:**
- Replace `flush_rect` with a canonical-format bulk-blit primitive.
- Migrate `fbcon.c` onto it, removing its dependency on
  `vesafb_internal.h` and the `fb` global entirely.
- Make `vesafb`'s raw framebuffer state (`struct fb_info fb`) private
  to `vesafb.c`.

**Out of scope (deliberately, YAGNI):**
- Writing a second `fb_device` implementation (virtio-gpu, bochs-drm).
  Not needed to validate the interface — the interface is sufficient
  by construction once fbcon, its one real consumer, is proven to work
  through it with no bypass.
- The Doom port itself. Separate repo (`neoos-doom`), separate spec,
  built on top of this once it lands.
- Any change to `/dev/fb0` (`fb.h`/`fb_device.c` userland surface) —
  it already goes through the abstraction correctly and needs nothing
  from this work.
- Any change to `struct fb_mode`'s channel fields (`r`/`g`/`b`
  position/size) — they stay for now since other code may read them
  for display purposes; the blit path simply doesn't need them because
  it's always canonical format.

## 3. Decisions

### 3.1 The primitive

```c
// kernel/drivers/video/fb_device.h

// Blits a rectangle from `src` (always canonical 32bpp XRGB8888 --
// byte order 0x00RRGGBB per pixel, regardless of the driver's actual
// hardware format) into the framebuffer at (dst_x, dst_y), width w,
// height h. src_pitch is the SOURCE buffer's stride in bytes (may
// differ from w*4 if the caller is blitting a sub-rect of a larger
// off-screen buffer). May assume the destination rect is fully within
// the current mode's bounds -- callers clip, not drivers.
void (*blit)(const void *src, uint32_t src_pitch,
             int dst_x, int dst_y, int w, int h);
```

Replaces `flush_rect` in `struct fb_device` directly (same slot, no
new field) — `flush_rect` has no real caller to preserve compatibility
for.

**Why canonical format, not native-format-with-query:** a caller that
had to query `current()` for bpp/channel layout before every blit would
carry format-handling logic in every consumer (fbcon today, Doom
tomorrow, anything after). Canonical format pushes that cost to
exactly one place — inside each driver's own `blit` implementation —
and every driver written from now on pays it once, not every caller
paying it forever. `vesafb` needs zero conversion today (it's already
32bpp XRGB8888 in practice; `vesafb_parse`'s channel sanitization
already assumes this), so this costs nothing now and only matters the
day a driver with a genuinely different native format exists.

**Why one primitive for both scales of update:** fbcon's real
workload is many small rects (a single glyph is 8×16 pixels, a
scrolled line is `width`×16); Doom's is one whole-screen rect per
frame. Both are "copy a rectangle of pixels to a destination
position" — the same operation at different sizes. A separate
"whole-frame" op would just be this same primitive with `dst_x=dst_y=0`
and `w`/`h` equal to the current mode's dimensions; not worth a second
function pointer.

**Why `dst_x`/`dst_y`/`w`/`h` instead of a full-screen-only blit:**
without them, fbcon would have to redraw the *entire* screen on every
keystroke echo (a `width`×`height`×4 byte copy, potentially several MB,
many times a second) instead of just the changed glyph or line. The
rect parameters are what make this primitive usable for both consumers
at all.

### 3.2 `vesafb`'s implementation

```c
// kernel/drivers/video/vesafb.c
static void vesafb_blit(const void *src, uint32_t src_pitch,
                         int dst_x, int dst_y, int w, int h) {
    if (!fb.present) { return; }
    const uint8_t *s = (const uint8_t *)src;
    volatile uint8_t *d = fb.virt + (uint64_t)dst_y * fb.pitch + (uint64_t)dst_x * 4;
    for (int row = 0; row < h; row++) {
        // A straight memcpy per row: vesafb's native format already IS
        // canonical XRGB8888, so there is no conversion to do here --
        // this loop body is the one place a future driver with a
        // different native format would add one.
        for (int col = 0; col < w * 4; col++) { d[col] = s[col]; }
        s += src_pitch;
        d += fb.pitch;
    }
}
```

(Byte-at-a-time within a row rather than a real `memcpy`, matching
this file's existing style elsewhere in the driver — freestanding
kernel code without a guaranteed libc `memcpy` available at this call
site; confirm against what's already linked into `vesafb.c` before
implementing and use a real `memcpy` if one is already available
there, since a wider copy is strictly better once it's safe to call.)

Replaces the `.flush_rect = vesafb_flush_rect` table entry with
`.blit = vesafb_blit`; `vesafb_flush_rect` itself is deleted (dead
code, no other caller per §1).

### 3.3 `fb_device_selftest()`

```c
// kernel/drivers/video/fb_device.c
void fb_device_selftest(void) {
    if (!active) { ...; return; }
    struct fb_mode m;
    uint64_t phys = 0, size = 0;
    active->current(&m, &phys, &size);
    if (m.width == 0 || m.height == 0 || m.pitch < m.width || phys == 0) { ...; return; }
    // A 1x1 black-pixel blit at the origin -- proves the driver's
    // blit path itself works, not just that the function pointer is
    // non-NULL (flush_rect's old check).
    uint32_t px = 0;
    if (active->blit) { active->blit(&px, 4, 0, 0, 1, 1); }
    serial_write_string("[fbdev] selftest passed\n");
}
```

### 3.4 `fbcon.c` migration

Every place `fbcon.c` currently reads `fb.width`, `fb.height`,
`fb.present`, or writes through `fb.virt`/`fb.pitch` is replaced:

- `fb.present` → `fb_device_active() != 0` (already how `fbcon_probe()`
  checks this — the rest of the file needs to adopt the same check
  instead of reading the `fb` global's `present` field directly).
- `fb.width` / `fb.height` → cached from `fb_device_active()->current(&m, ...)`,
  read once at `fbcon` init (or mode-change) rather than re-queried
  per draw call, matching the file's current performance-conscious
  style.
- Every direct pixel/glyph/line write into `fb.virt` → render into a
  small local buffer (a glyph is 8×16×4 = 512 bytes — trivial on the
  stack) and call `fb_device_active()->blit(...)` with the appropriate
  rect. Scrolling (`fbcon.c`'s row-shift logic, currently a raw
  `uint64_t`-width memmove over `fb.virt`) becomes a sequence of
  row-by-row `blit` calls reading from a temporary line buffer, OR (if
  performance matters — measure before deciding) a driver-side
  `scroll` op could be added later; **not** part of this spec (YAGNI —
  add only if the naive migration measurably regresses console
  responsiveness, which needs is own follow-up if it happens).
- `#include "drivers/video/vesafb_internal.h"` is removed from
  `fbcon.c` entirely. After this migration, that header (and the `fb`
  global it declares) should have zero includers outside `vesafb.c` —
  grep for it as the concrete, checkable proof this migration is
  complete.

### 3.5 `vesafb_internal.h` / `struct fb_info fb`

`struct fb_info fb` stops being `extern` at file scope in a shared
header; it moves to a `static` file-scope global inside `vesafb.c`
itself. `vesafb_parse()` and `fb_map()` — the two functions the boot
sequence in `kernel/kernel.c` calls directly — keep their prototypes
in a small header `vesafb.h` (or stay in `fb_device.h` if that's a
smaller diff) that exposes *only* those two functions, not the `fb`
struct. `vesafb_internal.h` can then either be deleted (its remaining
content, if any, folds into `vesafb.c` directly) or kept but genuinely
private (not included by anything outside `vesafb.c`).

## 4. Testing

- `[fbdev] selftest passed` — the existing gauntlet marker, now
  actually exercising the `blit` path (§3.3) instead of a no-op-shaped
  flush.
- `[fbcon] selftest passed` — the existing marker; must still pass
  after migration with visually-equivalent output (spot check via
  `make run` — a human confirms the console still renders correctly,
  per this project's existing convention for anything requiring visual
  judgment).
- `[term] render ALL PASSED` — the M1b framebuffer terminal's own
  self-check (a *different* consumer, `userland/term/`, which already
  goes through `/dev/fb0` — should be unaffected by this change, but
  is exactly the kind of thing a `blit` signature mistake would
  silently break, so treat a regression here as equally serious as a
  gauntlet failure even though it's a userland-side marker).
- Grep-based structural check (not a runtime test, but a real one):
  `grep -rn vesafb_internal kernel/` should return exactly one match
  (`vesafb.c`'s own `#include`) after this migration lands.

## 5. Migration ordering

Single-commit-sized work, no phasing needed:
1. Add `blit` to `struct fb_device`, remove `flush_rect`.
2. Implement `vesafb_blit`, remove `vesafb_flush_rect`.
3. Update `fb_device_selftest()`.
4. Migrate `fbcon.c`.
5. Make `fb` private to `vesafb.c`; slim `vesafb_internal.h` or delete
   it.
6. Gauntlet green, `make run` visual spot-check, commit.

This lands in `neoos-kernel` directly (not the archived monorepo).
