# M1c-2 — display driver interfaces — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** replace `console.c`'s hard-coded `fb.present ? fbcon : vga`
with two registered driver interfaces — **`fb_device`** (a linear-
framebuffer provider) and **`con_driver`** (a text painter) — chosen by
a boot-time probe. Fix the Multiboot2 channel-layout bug for real so
`/dev/fb0` reports a trustworthy pixel format.

**Architecture:** `fb_device` is a small vtable + registry;
`vesafb` (the renamed `fb.c`) is the one implementation, wrapping the
existing `struct fb_info fb`. `con_driver` is a streaming-console vtable
+ registry; `fbcon`, `vgacon` (renamed `vga.c`), and a new `dummycon`
register. `kmain` does `fb_device_probe_all()` → `fb_map()` →
`con_driver_select()`. No VT grid yet — that is M1c-3; M1c-2 keeps the
current streaming putc/scroll behaviour exactly, just behind vtables.

**Tech Stack:** C (freestanding kernel), GNU Make, headless QEMU +
serial-log markers, the parallel gauntlet.

**Spec:** `docs/superpowers/specs/2026-09-01-m1c-driver-model-and-vts-design.md` (§2, §3, §9 row M1c-2)

## Global Constraints

- **Behaviour-preserving.** The screen output is byte-for-byte what it
  is today: `fbcon` under `-vga std`, streaming `putc`, scroll at the
  bottom, one grey-on-black colour. The *only* observable change is
  `/dev/fb0`'s `FBIOGET_VSCREENINFO` now reporting sane channel
  offsets, plus two new selftest marker lines.
- **These REQUIRED_MARKERS must still appear verbatim:** `[fb] framebuffer`,
  `[fbcon] selftest passed`. Keep them printed from `vesafb` / `fbcon`.
- **New markers:** `[fbdev] selftest passed`, `[con] selftest passed`
  — add both to `REQUIRED_MARKERS`.
- **`fb_owned` / `console_set_fb_owned` stay** for now — M1c-4 removes
  them with `NEOOS_TIOCSACTIVE`. M1c-2 does not touch the M1b-3 TERM
  code; once `vesafb` reports `{16,8},{8,8},{0,8}`, TERM's existing
  "use the ioctl values if they look sane" guard takes them and its
  XRGB8888 fallback goes dead on its own.
- **The `con_driver` interface is STREAMING** with 16-colour cells
  (`putc_attr(char, uint8_t fg)` / `clear`) — the row/col-addressed
  `putc(row,col,ch,attr)` + `repaint(vc)` ops from spec §3 are added in
  **M1c-3** when `struct vt_console` exists. Do not add a `vt_console`
  dependency here.
- **Task 3 (boot banner) is a user addition on top of the M1c spec.**
  It needs the colour `con_driver` (Task 2) and `con_driver_select()`,
  so it lands here. Record it in the M1c spec's decisions table.
- **File renames** (`git mv`, symbol renames too): `fb.c` → `vesafb.c`,
  `vga.{c,h}` → `vgacon.{c,h}`, `vga_putc`→`vgacon_putc`,
  `vga_clear`→`vgacon_clear`, `vga_print_string` → delete (unused —
  `keyboard.c` and `devfs.c` `#include` `vga.h` but call nothing;
  drop those includes). `fb.h` **keeps its name** — it is the
  `/dev/fb0` (fbdev) public header, conventionally `fb`.
- **Work on `main`, one commit per task**, trailer:
  `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01CNR4gEkyMq6qhFWfxt3KXE`.
- **Regenerate disks** before a bare `make test`. **Never `make`
  during the gauntlet.**

## File Structure

| File | Change |
|---|---|
| `kernel/drivers/video/fb_device.h` (new) | `struct fb_device`, registry API |
| `kernel/drivers/video/fb_device.c` (new) | registry (static array, ≤4) + `fb_device_probe_all` / `fb_device_active` + `[fbdev]` selftest |
| `kernel/drivers/video/fb.c` → `vesafb.c` | now defines `struct fb_device vesafb_drv`; keeps `struct fb_info fb`, `fb_init` (renamed `vesafb_parse`), `fb_map`, `fb_file_ops`; **channel sanitiser** |
| `kernel/drivers/video/fb.h` | `struct fb_info` + `fb`/`fb_init`/`fb_map` move to a private `vesafb_internal.h`; `fb.h` keeps only the `/dev/fb0` surface (`fb_file_ops`, `fb_open`, `FBIO*`) |
| `kernel/tty/con_driver.h` (new) | `struct con_driver`, registry API |
| `kernel/tty/con_driver.c` (new) | registry + `con_driver_select` + `[con]` selftest |
| `kernel/drivers/video/fbcon.c` | add `struct con_driver fbcon_drv`; `probe = fb_device_active() != NULL`; body unchanged |
| `kernel/drivers/video/vga.{c,h}` → `vgacon.{c,h}` | add `struct con_driver vgacon_drv`; `probe` always true, priority 10 |
| `kernel/tty/dummycon.c` (new) | no-op `con_driver`, priority 0 |
| `kernel/tty/console.c` | `console_putc/write/clear` → `con_driver_active()->…` |
| `kernel/kernel.c` | `fb_init`→`fb_device_probe_all`; `fbcon_init`→`con_driver_select`+init; add the two selftests |
| `kernel/fs/devfs.c`, `kernel/drivers/input/keyboard.c` | drop the now-unused `#include "drivers/video/vga.h"` |
| `kernel/version.h` (new) | `NEOOS_VERSION`; `NEOOS_GITREV` fallback |
| `kernel/tty/banner.{c,h}` (new) | the boot banner (Task 3) |
| `kernel/arch/cpu.{c,h}` | `cpu_brand_string`, `cpu_feature_string` (Task 3) |
| `kernel/mm/pmm.{c,h}` | `pmm_total_frame_count` if absent (Task 3) |
| `Makefile` | `REQUIRED_MARKERS += "[fbdev] selftest passed" "[con] selftest passed" "[banner]"`; `-DNEOOS_GITREV` from `git describe` |
| `README.md`, `docs/abi-compatibility.md`, `docs/stdlib.md` | the driver model, the fixed channel reporting, the banner |

---

## Task 1: `struct fb_device` + `vesafb`

**Files:** Create `fb_device.{c,h}`; `git mv fb.c vesafb.c`; split
`fb.h` → `fb.h` + `vesafb_internal.h`; modify `kernel/kernel.c`,
`Makefile`.

**Interfaces:**
- Produces:
  ```c
  // fb_device.h
  struct fb_channel { uint8_t offset, length; };
  struct fb_mode {
      uint32_t width, height, bpp, pitch;
      struct fb_channel r, g, b;
  };
  struct fb_device {
      const char *name;
      int priority;                        // higher wins
      int  (*probe)(void *multiboot_info); // 1 if it can drive this machine
      void (*current)(struct fb_mode *m, uint64_t *phys, uint64_t *size);
      int  (*modes)(struct fb_mode *out, int max);
      int  (*set_mode)(const struct fb_mode *m);
      void (*flush_rect)(int x, int y, int w, int h);   // may be NULL
      int  (*cursor)(int x, int y, int visible);        // may be NULL
  };
  void              fb_device_register(struct fb_device *d);
  struct fb_device *fb_device_active(void);   // NULL on a text-only boot
  void              fb_device_probe_all(void *multiboot_info);
  void              fb_device_selftest(void); // "[fbdev] ..."
  ```

- [ ] **Step 1: `fb_device.{c,h}`**

`fb_device.c`: a `static struct fb_device *registry[4]; static int n;`
plus `static struct fb_device *active;`. `fb_device_register` appends
(assert `n < 4`). `fb_device_probe_all` iterates the registry, calls
each `probe(mbi)`, keeps the highest-`priority` one that returns
nonzero, stores it in `active`, logs `"[fbdev] <name> selected"` or
`"[fbdev] no framebuffer device"`. `fb_device_active` returns `active`.

Registration happens from a constructor-ish path: since NeoOS has no
`.init_array` for the kernel, add an explicit
`void fb_device_register_builtin(void)` in `fb_device.c` that calls
`fb_device_register(&vesafb_drv)` (extern). `kmain` calls
`fb_device_register_builtin()` then `fb_device_probe_all(mbi)`.

- [ ] **Step 2: split `fb.h`**

Move `struct fb_info`, `extern struct fb_info fb`, `fb_init` (→ rename
`vesafb_parse`), `fb_map` into a new `kernel/drivers/video/vesafb_internal.h`.
`fb.h` keeps `#include "drivers/video/fb.h"` users happy with only:
`fb_file_ops`, `int fb_open(struct file_descriptor *)`, `FBIOGET_VSCREENINFO`
/ `FBIOPUT_VSCREENINFO` / `FBIOGET_FSCREENINFO`. `console.c` currently
`#include`s `fb.h` only to read `fb.present` — after Task 2 it will not
touch `fb` at all, so its `fb.h` include is dropped there. `kernel.c`
switches to calling `fb_device_probe_all` / `fb_map`.

- [ ] **Step 3: `git mv fb.c vesafb.c`; make it an `fb_device`**

```c
// vesafb.c
#include "drivers/video/fb_device.h"
#include "drivers/video/vesafb_internal.h"

struct fb_info fb;   // unchanged internal state

static int vesafb_probe(void *mbi) {
    vesafb_parse(mbi);           // the old fb_init: scan the MB2 tag
    return fb.present;
}
static void vesafb_current(struct fb_mode *m, uint64_t *phys, uint64_t *size) {
    m->width = fb.width; m->height = fb.height; m->bpp = fb.bpp;
    m->pitch = fb.pitch;
    m->r = (struct fb_channel){ fb.r.pos, fb.r.size };
    m->g = (struct fb_channel){ fb.g.pos, fb.g.size };
    m->b = (struct fb_channel){ fb.b.pos, fb.b.size };
    if (phys) *phys = fb.phys;
    if (size) *size = fb.size;
}
static int vesafb_modes(struct fb_mode *out, int max) {
    if (max < 1) return 0;
    vesafb_current(&out[0], 0, 0);
    return 1;
}
static int vesafb_set_mode(const struct fb_mode *m) {
    struct fb_mode cur; vesafb_current(&cur, 0, 0);
    return (m->width == cur.width && m->height == cur.height &&
            m->bpp == cur.bpp) ? 0 : -EINVAL;
}
static void vesafb_flush_rect(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }

struct fb_device vesafb_drv = {
    .name = "vesafb", .priority = 100,
    .probe = vesafb_probe, .current = vesafb_current,
    .modes = vesafb_modes, .set_mode = vesafb_set_mode,
    .flush_rect = vesafb_flush_rect, .cursor = 0,
};
```

- [ ] **Step 4: the channel-layout sanitiser**

At the end of `vesafb_parse` (old `fb_init`), after reading
`fb.r/g/b` from the MB2 tag:

```c
// The MB2 tag's channel positions have been observed inconsistent
// (r==g, or > 24) under GRUB+QEMU stdvga. Every 32bpp mode NeoOS
// supports is XRGB8888; fbcon already assumes it. Fall back to that
// and log when the tag disagrees.
int bad = (fb.r.pos == fb.g.pos) || (fb.g.pos == fb.b.pos) ||
          fb.r.pos > 24 || fb.g.pos > 24 || fb.b.pos > 24;
if (bad) {
    serial_write_string("[fb] MB2 channel positions bogus (r=");
    serial_write_hex64(fb.r.pos); serial_write_string(" g=");
    serial_write_hex64(fb.g.pos); serial_write_string(" b=");
    serial_write_hex64(fb.b.pos);
    serial_write_string("), assuming XRGB8888\n");
    fb.r.pos = 16; fb.g.pos = 8; fb.b.pos = 0;
    fb.r.size = fb.g.size = fb.b.size = 8;
}
```

Keep the existing `[fb] framebuffer …` line (a REQUIRED_MARKER).

- [ ] **Step 5: `[fbdev]` selftest + `/dev/fb0` unchanged in behaviour**

`fb_device_selftest`: assert `fb_device_active()` is non-NULL under
`-vga std`; call `current()` and check `width/height` match the MB2
tag's; call `flush_rect(0,0,1,1)` (must not fault); print
`[fbdev] selftest passed` (or `[fbdev] selftest skipped (no fb)`).

`fb_file_ops` `FBIOGET_VSCREENINFO`: change nothing except that the
`x.red/green/blue` it fills now come from the sanitised `fb.r/g/b`
(which they already read — the sanitiser fixed the source). `fbtest`
must still pass.

- [ ] **Step 6: `kmain` wiring for Task 1 only**

Replace `fb_init(multiboot_info);` with
```c
fb_device_register_builtin();
fb_device_probe_all(multiboot_info);
```
`fb_map();` stays (still needed — fbcon draws through `fb.virt`).
After `fb_map()`, add `fb_device_selftest();`. `fbcon_init()` /
`fbcon_selftest()` stay untouched until Task 2.

- [ ] **Step 7: build, test, gauntlet, commit**

`REQUIRED_MARKERS += "[fbdev] selftest passed"`. `make test` →
`[fb] framebuffer …`, `[fbdev] selftest passed`, `[fbcon] selftest
passed`, `[fbtest] ALL PASSED` all present; serial output on screen
unchanged. Gauntlet 15/15. Commit
`"M1c-2: struct fb_device + vesafb; fix MB2 channel-layout"`.

---

## Task 2: `struct con_driver` + fbcon/vgacon/dummycon + `console.c`

**Files:** Create `con_driver.{c,h}`, `dummycon.c`; `git mv vga.{c,h}
vgacon.{c,h}`; modify `fbcon.c`, `console.c`, `kernel.c`, `devfs.c`,
`keyboard.c`, `Makefile`.

**Interfaces:**
- Produces:
  ```c
  // con_driver.h
  // 16-colour indices (VGA order). fbcon maps to RGB; vgacon uses the
  // index directly as the attribute fg nibble.
  #define CON_BLACK 0  #define CON_RED 4   #define CON_MAGENTA 5
  #define CON_GREY 7    #define CON_LRED 12 #define CON_LMAGENTA 13
  #define CON_WHITE 15  /* ...the full 0..15 set in the header */

  struct con_driver {
      const char *name;
      int priority;
      int  (*probe)(void);
      void (*init)(int *cols, int *rows);       // set up, report geometry
      void (*putc_attr)(char c, uint8_t fg);    // fg = CON_* index
      void (*clear)(void);
  };
  void               con_driver_register(struct con_driver *d);
  struct con_driver *con_driver_active(void);
  void               con_driver_select(void);   // probe + init the winner
  void               con_driver_selftest(void); // "[con] ..."

  // convenience: plain grey putc + write, on top of putc_attr
  static inline void con_putc(char c)  { con_driver_active()->putc_attr(c, CON_GREY); }
  void con_write(const char *s, uint64_t n);
  void con_write_attr(const char *s, uint64_t n, uint8_t fg);
  ```
  `putc_attr` (not `putc`) is the one driver primitive — a 16-colour
  cell is what M1c-3's VT grid and Task 3's banner both need, and
  carrying it now avoids a second interface change. `con_write` /
  `con_putc` are free functions in `con_driver.c`.

- [ ] **Step 1: `con_driver.{c,h}`**

Same registry shape as `fb_device.c`. `con_driver_select`: pick the
highest-priority `probe()==1`, call its `init(&cols,&rows)`, store
`active`, log `"[con] <name> selected, <cols>x<rows>"`.
`con_driver_register_builtin()` registers `fbcon_drv`, `vgacon_drv`,
`dummycon_drv`.

- [ ] **Step 2: `fbcon.c` gains a `con_driver`**

Add, without changing any existing function body except that
`fbcon_putc`/`fbcon_write` gain an `fg` colour and a 16-entry
`CON_* → 0x00RRGGBB` palette table (VGA colour values). `fbcon_clear`
unchanged.
```c
static int  fbcon_probe(void) { return fb_device_active() != 0; }
static void fbcon_init_cd(int *cols, int *rows) {
    fbcon_init();                 // existing: computes cols/rows, clears
    *cols = (int)fbcon_cols();    // add tiny getters, or expose the statics
    *rows = (int)fbcon_rows();
}
static void fbcon_putc_attr(char c, uint8_t fg) { /* was fbcon_putc, + colour */ }
struct con_driver fbcon_drv = {
    .name = "fbcon", .priority = 100,
    .probe = fbcon_probe, .init = fbcon_init_cd,
    .putc_attr = fbcon_putc_attr, .clear = fbcon_clear,
};
```
`fbcon.c` now `#include`s `fb_device.h` (for `fbcon_probe`) and
`con_driver.h`. `fbcon_selftest` stays as-is, still called from kmain.

- [ ] **Step 3: `git mv vga.{c,h} vgacon.{c,h}`; make it a `con_driver`**

Rename symbols `vga_clear`→`vgacon_clear`, `vga_putc`→`vgacon_putc`;
delete `vga_print_string`. `vgacon_putc` gains an `fg` and writes
`(ch | (fg << 8))` into the cell (VGA hardware attribute byte — the
`CON_*` indices ARE the VGA fg nibble). Add `vgacon_init(int*,int*)`
(`*cols=80; *rows=25;` + clear).
```c
static int vgacon_probe(void) { return 1; }   // always available
static void vgacon_putc_attr(char c, uint8_t fg);
struct con_driver vgacon_drv = {
    .name = "vgacon", .priority = 10,
    .probe = vgacon_probe, .init = vgacon_init,
    .putc_attr = vgacon_putc_attr, .clear = vgacon_clear,
};
```
Update the two stale includes: `devfs.c` and `keyboard.c` drop
`#include "drivers/video/vga.h"` entirely (grep confirms neither calls
a `vga_*` symbol).

- [ ] **Step 4: `dummycon.c`**

```c
#include "tty/con_driver.h"
static int  dummy_probe(void) { return 1; }
static void dummy_init(int *c, int *r) { *c = 80; *r = 25; }
static void dummy_putc_attr(char c, uint8_t fg) { (void)c; (void)fg; }
static void dummy_clear(void) {}
struct con_driver dummycon_drv = {
    .name = "dummycon", .priority = 0,
    .probe = dummy_probe, .init = dummy_init,
    .putc_attr = dummy_putc_attr, .clear = dummy_clear,
};
```

- [ ] **Step 5: `console.c` through the vtable**

```c
#include "tty/console.h"
#include "tty/con_driver.h"

static int fb_owned;                       // still here; M1c-4 removes it
void console_set_fb_owned(int on) { fb_owned = on ? 1 : 0; }
int  console_fb_owned(void) { return fb_owned; }

void console_putc(char c) {
    if (fb_owned || !con_driver_active()) return;
    con_driver_active()->putc_attr(c, CON_GREY);
}
void console_write(const char *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { console_putc(s[i]); }
}
void console_clear(void) {
    if (fb_owned || !con_driver_active()) return;
    con_driver_active()->clear();
}
```
Drops the `fb.h` / `fbcon.h` / `vga.h` includes. `con_driver_active()`
is guaranteed non-NULL after `con_driver_select()` (dummycon always
probes true) — but guard with `if (!con_driver_active()) return;` for
the window before selection during very early boot.

- [ ] **Step 6: `kmain` wiring**

```c
// after fb_map():
con_driver_register_builtin();
con_driver_select();          // replaces fbcon_init()
fbcon_selftest();             // keep -- still the [fbcon] REQUIRED_MARKER
con_driver_selftest();        // new [con] marker
console_clear();
console_write("NeoOS booted\n", 13);
```

- [ ] **Step 7: `[con]` selftest**

`con_driver_selftest`: assert `con_driver_active()` is `fbcon` under
`-vga std` (name compare), `putc('X')` + `clear()` do not fault, print
`[con] selftest passed`.

- [ ] **Step 8: build, test, gauntlet, commit**

`REQUIRED_MARKERS += "[con] selftest passed"`. `make test`: every
prior marker present, `[con] selftest passed` present, on-screen
output unchanged (verify by eye with `make run` once, or trust the
byte-identical selftest lines). Gauntlet 15/15. Update
`docs/abi-compatibility.md` / `docs/stdlib.md`. Commit
`"M1c-2: struct con_driver (fbcon/vgacon/dummycon); console.c through the vtable"`.

---

## Task 3: boot banner

**Files:** Create `kernel/tty/banner.{c,h}`, `kernel/version.h`;
modify `kernel/arch/cpu.{c,h}`, `kernel/mm/pmm.{c,h}` (a total-frame
getter if absent), `kernel/kernel.c`, `Makefile`.

**Interfaces:**
- Produces:
  ```c
  // kernel/version.h
  #define NEOOS_VERSION "0.1"
  // NEOOS_GITREV is -D'd by the Makefile: git describe --always --dirty
  #ifndef NEOOS_GITREV
  #define NEOOS_GITREV "unknown"
  #endif

  // kernel/arch/cpu.h
  void cpu_brand_string(char out[49]);      // CPUID 0x80000002..4, NUL-terminated
  int  cpu_feature_string(char *out, int max);  // "sse2 sse4.2 avx2 aes ..."

  // kernel/mm/pmm.h
  uint64_t pmm_total_frame_count(void);     // add if not already present

  // kernel/tty/banner.h
  void banner_show(void);                   // clear + logo + info panel
  ```

- [ ] **Step 1: `cpu_brand_string` + `cpu_feature_string`**

`cpu.c` already has `cpuid(leaf, &a,&b,&c,&d)` and the SSE/AVX feature
`#define`s. Add:

```c
void cpu_brand_string(char out[49]) {
    uint32_t r[12], unused;
    cpuid(0x80000000, &r[0], &unused, &unused, &unused);
    if (r[0] < 0x80000004) { out[0] = 0; return; }
    for (int i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, &r[i*4+0], &r[i*4+1], &r[i*4+2], &r[i*4+3]);
    }
    __builtin_memcpy(out, r, 48);
    out[48] = 0;
    // trim leading spaces some CPUs pad with
}

int cpu_feature_string(char *out, int max) {
    // leaf 1 -> edx (sse, sse2), ecx (sse3, ssse3, sse4.1, sse4.2,
    //   avx, aes, fma, rdrand, xsave, popcnt); leaf 7 ebx (avx2,
    //   bmi1, bmi2, sha); leaf 0x80000001 ecx (lahf), edx (nx, 1gb,
    //   rdtscp, lm). Append each present name + ' '.
}
```

Emit `[cpu] brand: <brand>` and `[cpu] features: <list>` to serial from
`cpu_init` (or from `banner_show`) — greppable proof the CPUID reads
work.

- [ ] **Step 2: `pmm_total_frame_count`**

If `pmm.c` tracks only `total_free_frames`, add a `static uint64_t
total_frames;` set once in `pmm_init` (sum of every usable region), and
`pmm_total_frame_count()`. Memory in MiB = `count * 4096 / (1<<20)`.

- [ ] **Step 3: `kernel/tty/banner.c`**

```c
#include "tty/banner.h"
#include "tty/con_driver.h"
#include "version.h"
#include "arch/cpu.h"
#include "mm/pmm.h"
#include "smp/smp.h"
#include "drivers/char/serial.h"

// The butterfly-N. Two arrays of the same shape: `art[]` is the glyph
// rows, `col[]` is the per-cell colour (P = purple = CON_LMAGENTA for
// the N strokes + body, R = red = CON_LRED for the wings/spots). Both
// checked in; iterate the glyphs freely, keep the two arrays aligned.
static const char *art[] = { /* ~9 rows, box/block glyphs */ };
static const char *col[] = { /* same shape: 'P' / 'R' / ' ' */ };

static void put_line_coloured(const char *s, const char *c) {
    for (int i = 0; s[i]; i++) {
        uint8_t fg = (c && c[i] == 'P') ? CON_LMAGENTA
                   : (c && c[i] == 'R') ? CON_LRED : CON_LRED;
        con_driver_active()->putc_attr(s[i], fg);
    }
    con_driver_active()->putc_attr('\n', CON_LRED);
}

void banner_show(void) {
    struct con_driver *cd = con_driver_active();
    if (!cd) { return; }
    cd->clear();

    for (int i = 0; art[i]; i++) { put_line_coloured(art[i], col[i]); }

    char brand[49]; cpu_brand_string(brand);
    char feats[160]; cpu_feature_string(feats, sizeof feats);
    uint64_t mib_total = pmm_total_frame_count() * 4096ull >> 20;
    uint64_t mib_free  = pmm_free_frame_count()  * 4096ull >> 20;

    // "NeoOS" wordmark in red with a purple 'N', then the panel lines,
    // all CON_LRED. Use a small printf-to-console helper or format by
    // hand (kernel has no console printf -- add a tiny one in banner.c
    // that writes through con_driver_active()->putc_attr).
    banner_printf(CON_LRED, "\n  NeoOS %s (%s)\n", NEOOS_VERSION, NEOOS_GITREV);
    banner_printf(CON_LRED, "  %s\n", brand[0] ? brand : "unknown CPU");
    banner_printf(CON_LRED, "  %d cores  -  %lu MiB free / %lu MiB\n",
                  smp_online_count(), mib_free, mib_total);
    banner_printf(CON_LRED, "  %s\n\n", feats);

    // Also to serial, one line, as a REQUIRED_MARKER:
    serial_write_string("[banner] NeoOS "); serial_write_string(NEOOS_VERSION);
    serial_write_string(" | "); serial_write_string(brand);
    serial_write_string(" | "); /* cores, mib, feats */
    serial_write_string(" shown\n");
}
```

`banner_printf` is a ~30-line varargs formatter supporting `%s %d %lu
%c %%` (mirror `lib/`'s minimal printf) writing through
`con_driver_active()->putc_attr(ch, fg)`.

- [ ] **Step 4: `Makefile` — git rev**

```make
NEOOS_GITREV := $(shell git describe --always --dirty --tags 2>/dev/null || echo unknown)
CFLAGS += -DNEOOS_GITREV='"$(NEOOS_GITREV)"'
```
Put it near `CFLAGS :=`. `NEOOS_GITREV` changes per commit, so
`kernel/kernel.o` (and anything including `version.h`) rebuilds when it
does — acceptable; only `banner.c` includes `version.h`.

- [ ] **Step 5: `kmain` — show it**

Right after Task 2's `con_driver_select()` + the selftests, replacing
the bare `console_write("NeoOS booted\n")`:

```c
banner_show();
```
The kernel selftests that follow are serial-only, so the banner stays
on screen until init / TERM writes.

- [ ] **Step 6: test**

`REQUIRED_MARKERS += "[banner]"` (prefix). `make test`: `[banner] …
shown`, `[cpu] brand:`, `[cpu] features:` lines present and non-empty
(under QEMU the brand is `"QEMU Virtual CPU version 2.5+"` or similar;
features include `sse2 sse4.2`). Gauntlet 15/15. **Visual check:**
`make run`, confirm the red/purple butterfly-N + panel render on the
framebuffer and clear the screen first.

- [ ] **Step 7: docs + commit**

`README.md` — a line about the boot banner. `docs/stdlib.md` /
`abi-compatibility.md` unaffected (kernel-internal). Commit
`"M1c-2: boot banner -- butterfly-N logo, CPU brand/features, mem, cores"`.

---

## Self-Review

**Spec coverage (§2, §3, §9 M1c-2):**
- `struct fb_device` + `vesafb` + boot probe → Task 1. `flush_rect`
  no-op, `cursor` NULL → Step 3. Channel-layout fix → Step 4 (spec §2
  "this is where the M1b-3 XRGB8888 workaround is fixed for real").
- `/dev/fb0` a thin shim over the active fb_device → Step 5 (the ioctl
  reads the now-sane `fb.r/g/b`; a fuller `current()`-based rewrite is
  deferred — the observable result is identical and correct).
- `struct con_driver` + `fbcon`/`vgacon`/`dummycon` + priority probe →
  Task 2. `console.c` loses the `fb.present` branch → Step 5.
- Selftests `[fbdev]` + `[con]` → Task 1 Step 5, Task 2 Step 7; added
  to `REQUIRED_MARKERS`.
- **Deviation from spec §3:** the M1c-2 `con_driver` primitive is
  `putc_attr(char, uint8_t fg)` (16-colour, streaming), not the
  `putc(row,col,ch,attr)` + `repaint(vc)` of the spec. The
  grid-addressed / repaint ops need `struct vt_console`, which is
  M1c-3. Colour is carried now (not `putc(char)` alone) because Task 3
  and M1c-3's SGR both need it — one interface change instead of two.
- **New in this plan (user request, not in the M1c spec):** Task 3,
  the boot banner — `banner_show()` after `con_driver_select()`:
  clears the screen, draws a red/purple butterfly-N logo and an info
  panel (`NEOOS_VERSION` + git rev, CPU brand string via CPUID
  `0x80000002-4`, online cores, free/total MiB, feature list). Update
  the M1c spec's decisions table to record it.

**Placeholder scan:** every new struct and function is given in full.
`fbcon_cols()`/`fbcon_rows()` getters are named with their one-line
bodies implied ("expose the statics"). `cpu_feature_string`'s body is
a described bit-walk (leaf/register/mask spelled out), not "add the
features" — the executor transcribes the standard CPUID feature bits.
The `art[]`/`col[]` logo arrays are "checked in, iterate the glyphs,
keep the two aligned" — deliberately left to implementation per the
user's "iterate glyphs in impl" choice. No "add error handling" — the
early-boot `con_driver_active() == NULL` window is called out in
Task 2 Step 5 and Task 3 Step 3.

**Type consistency:** `struct fb_device` / `struct fb_mode` /
`struct fb_channel` fields are identical in the interface block and
Task 1 Step 3's `vesafb_current`. `struct con_driver` fields
(`name, priority, probe, init, putc_attr, clear`) match between the
interface block, `fbcon_drv`, `vgacon_drv`, `dummycon_drv`, and
`console.c`. `CON_*` colour indices are used consistently
(`CON_GREY` for plain text, `CON_LRED`/`CON_LMAGENTA` for the banner).
`NEOOS_VERSION` / `NEOOS_GITREV` spelled the same in `version.h`, the
Makefile, and `banner.c`. `cpu_brand_string(char[49])` /
`cpu_feature_string(char*, int)` signatures match between `cpu.h` and
`banner.c`.

**Scope:** no VT layer, no `NEOOS_TIOCSACTIVE` change, no TERM change,
no new syscalls. Three commits, each `make test`- and gauntlet-green.
Tasks 1–2 keep on-screen behaviour identical; Task 3 adds the banner.
The VT grid is M1c-3.
