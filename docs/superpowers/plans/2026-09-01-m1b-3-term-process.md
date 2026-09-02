# M1b-3 — the TERM process — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `/bin/term.nex` — a userland process that owns a PTY, runs a child
on the slave, `mmap`s `/dev/fb0`, and paints the VT grid with the
Spleen glyph table. The kernel hands cooked keyboard input and the
framebuffer to whichever pty master claims them, stops touching
framebuffer pixels for normal output while claimed, and reclaims both
on a panic or on master close.

**Architecture:**
```
 keyboard IRQ ─► input_key_event ─► active_input_tty  (default tty_console,
                                    │                   set to a pts slave
                                    │                   by NEOOS_TIOCSACTIVE)
 /sbin/init.nex ─spawn─► /bin/term.nex      ▼
   open /dev/ptmx ─► master fd; TIOCGPTN ─► /dev/pts/N
   ioctl(master, NEOOS_TIOCSACTIVE, 1)   → route keys to pts/N, fb_owned=1
   open /dev/fb0; FBIOGET_VSCREENINFO; mmap MAP_SHARED
   fork: child close 0/1/2, open pts/N ×3, setsid, exec /bin/term.nexCHILD
   loop: poll(master) → read → vt_feed → vt_take_dirty → render_span(fb,…)
   child exits → self-check fb pixels → "[term] render ALL PASSED"
   ioctl(master, NEOOS_TIOCSACTIVE, 0); exit
```
Kernel `console_write`/`console_putc`/`console_clear` become serial-only
(a no-op for pixels) while `fb_owned_by_userland`; `exception_dump_and_halt`
clears the flag first so a fault always paints.

**Tech Stack:** C — kernel (`-mcmodel=kernel`) and libneoos userland
(`-mcmodel=large`, `printf` = `%s %d %u %x %c %%`, `memset/memcpy/
memmove`, no `malloc`, no `dup2`). Headless QEMU + serial-log markers.
The parallel gauntlet, ×3 on Task 3 (TERM is boot-critical — it holds
the console and the framebuffer for the whole run).

**Spec:** `docs/superpowers/specs/2026-09-01-m1b-framebuffer-terminal-design.md` (§1, §3, §6, §7, §9 row M1b-3)

## Global Constraints

- **`NEOOS_TIOCSACTIVE`** — a NeoOS-private ioctl on a **pty master**,
  not a new syscall. `#define NEOOS_TIOCSACTIVE 0x4E454F01` (`"NEO\x01"`,
  deliberately outside Linux's `0x54xx` TIOC range). `arg == 1`
  claims (route cooked keyboard input to this master's slave, hand the
  framebuffer to userland); `arg == 0` releases. Idempotent. Recorded
  in `docs/stdlib.md` as a NeoOS extension.
- **`active_input_tty`** lives in `kernel/dev/tty.c`
  (`static struct tty *active_input_tty = &console_tty;`), reached
  through `tty_set_active(struct tty *)` / `struct tty *tty_active(void)`
  declared in `tty.h`. `tty_set_active(NULL)` resets to the console.
- **`fb_owned_by_userland`** lives in `kernel/dev/console.c`
  (`static int fb_owned;`), set/read through `void console_set_fb_owned(int)`
  / `int console_fb_owned(void)` in `console.h`. `console_putc` skips
  `fbcon_putc`/`vga_putc` while set (serial is written by callers, not
  here — unchanged). `console_clear` likewise.
- **Panic reclaim:** `exception_dump_and_halt` (`kernel/arch/isr.c`)
  calls `console_set_fb_owned(0)` and `tty_set_active(NULL)` **before**
  its `console_write("EXCEPTION - HALTED\n", …)`, so a fault repaints
  over TERM's grid. `lock_panic` is already serial-only — no change.
- **`ptm_close` releases first, frees second:** if the closing master
  is the active one, `tty_set_active(NULL)` + `console_set_fb_owned(0)`
  **before** `pty_unref` can free the `struct pty`, so
  `active_input_tty` never dangles.
- **Cell geometry:** `TERM_GLYPH_W` 12, `TERM_GLYPH_H` 24 (from
  `userland/term/font_term.h`). `cols = min(fb.w / 12, VT_MAX_COLS)`,
  `rows = min(fb.h / 24, VT_MAX_ROWS)`.
- **TERM does not grab evdev.** M1b-3 uses only the cooked path
  (printable keys → `active_input_tty` → pts line discipline → echo →
  master → render). Arrow/PageUp translation and scrollback are M1b-4.
- **`dup2` was pulled forward from BB1.** The plan originally had the
  child do `close(0/1/2)` + `open` ×3, but `fd_table_alloc` **never
  reallocates fds 0/1/2** (`kernel/sched/fd_table.c`) — after `close(0)`
  the next `open` returns fd 5. So `dup`/`dup2`/`dup3` (SYS_DUP 70,
  DUP2 71, DUP3 72; `fd_table_dup2`; shim + `<unistd.h>` wrappers) were
  implemented here instead. The child now does
  `s = open(pts); dup2(s,0); dup2(s,1); dup2(s,2); close(s)`. BB1's dup
  work is done; BB1 keeps only `F_DUPFD`.
- **The parent holds the slave open across the fork** and closes it
  only after — otherwise `slave_refs` momentarily hits 0 between the
  child's `close` and its re-`open`, the pty reports hung-up, and
  TERM's first `poll` returns `POLLHUP` and the render loop bails.
- **`ptm_close` releases `NEOOS_TIOCSACTIVE` only on the *last* master
  fd.** `fork()` duplicates the master fd; the child's `close(m)` must
  not release the parent's claim (it did, which let `fbcon` repaint
  over the grid mid-render).
- **Framebuffer format:** 32-bpp, `fb.r/g/b` channel positions from
  `FBIOGET_VSCREENINFO` (`x.red.offset` etc. — extend the ioctl if it
  does not already report them; check `kernel/dev/fb.c`). A pixel is
  `(r << ro) | (g << go) | (b << bo)`.
- **Work on `main`**, one commit per task, trailer:
  `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01CNR4gEkyMq6qhFWfxt3KXE`.
- **Regenerate disks** before a bare `make test`. **Never `make`
  during the gauntlet.**

## File Structure

| File | Change |
|---|---|
| `kernel/dev/tty.h` / `tty.c` | `tty_set_active` / `tty_active` + the static pointer |
| `kernel/dev/console.h` / `console.c` | `console_set_fb_owned` / `console_fb_owned` + the guards |
| `kernel/dev/input.c` | route to `tty_active()` instead of `tty_console()` |
| `kernel/dev/pty.c` / `pty.h` | `NEOOS_TIOCSACTIVE` in `ptm_ioctl`; release in `ptm_close` |
| `kernel/dev/fb.c` | ensure `FBIOGET_VSCREENINFO` reports the r/g/b bitfields |
| `kernel/arch/isr.c` | panic reclaim in `exception_dump_and_halt` |
| `userland/term/render.h` / `render.c` | glyph blitter: `render_span`, `render_cursor` |
| `userland/term/palette.c` / `palette.h` | 256-entry xterm RGB table + default fg/bg |
| `userland/term/main.c` | the TERM process |
| `userland/termchild.c` | the M1b-3 test child |
| `userland/activettytest.c` | Task 1's routing test |
| `tools/rendertest.c` | host check for `render.c` |
| `Makefile` | `TERM.ELF` / `TERMCHILD.ELF` / `ACTIVETTYTEST.ELF` rules, disk copies, INITTAB, `REQUIRED_MARKERS`, `render-check` |
| `docs/stdlib.md` | `NEOOS_TIOCSACTIVE`, the fb-ownership handoff |

---

## Task 1: kernel plumbing — active tty, fb ownership, the ioctl

**Files:** `kernel/dev/tty.{c,h}`, `kernel/dev/console.{c,h}`,
`kernel/dev/input.c`, `kernel/dev/pty.{c,h}`, `kernel/arch/isr.c`,
`kernel/dev/fb.c`; Create `userland/activettytest.c`; modify `Makefile`.

**Interfaces:**
- Produces:
  ```c
  /* tty.h */   void tty_set_active(struct tty *t);   /* NULL -> console */
                struct tty *tty_active(void);
  /* console.h */ void console_set_fb_owned(int on);
                  int  console_fb_owned(void);
  /* pty.h */   #define NEOOS_TIOCSACTIVE 0x4E454F01
  ```

- [ ] **Step 1: failing test — `userland/activettytest.c`**

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>          /* if absent, declare ioctl extern */
#include <neoos_test.h>

#define NEOOS_TIOCSACTIVE 0x4E454F01
#define TIOCGPTN          0x80045430
#define KEY_A 30               /* Linux KEY_A; decoder maps to 'a' */

int main(void) {
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0) { printf("[activetty] FAILED: ptmx %d\n", m); return 1; }
    int n = -1; ioctl(m, TIOCGPTN, &n);
    char path[32]; /* build "/dev/pts/N" */
    /* ... snprintf-free: printf-style not available; format by hand ... */
    int p = 0; const char *pre = "/dev/pts/";
    for (; pre[p]; p++) path[p] = pre[p];
    if (n < 10) { path[p++] = '0' + n; } else { path[p++] = '0'+n/10; path[p++]='0'+n%10; }
    path[p] = 0;
    int s = open(path, O_RDWR);
    if (s < 0) { printf("[activetty] FAILED: pts open %d\n", s); return 1; }

    /* not claimed yet: an injected key must NOT reach this master */
    fcntl(m, 4 /*F_SETFL*/, O_NONBLOCK);
    neoos_test_inject_key(KEY_A, 1); neoos_test_inject_key(KEY_A, 0);
    char b[8]; int r = read(m, b, sizeof b);
    if (r > 0) { printf("[activetty] FAILED: key leaked before claim\n"); return 1; }

    /* claim -> the key is cooked into the slave and echoed to the master */
    if (ioctl(m, NEOOS_TIOCSACTIVE, 1) != 0) { printf("[activetty] FAILED: claim\n"); return 1; }
    neoos_test_inject_key(KEY_A, 1); neoos_test_inject_key(KEY_A, 0);
    /* give the echo a moment: block briefly */
    fcntl(m, 4, 0);                       /* back to blocking */
    r = read(m, b, sizeof b);
    if (r < 1 || b[0] != 'a') { printf("[activetty] FAILED: no echo after claim r=%d\n", r); return 1; }

    /* release -> back to normal */
    if (ioctl(m, NEOOS_TIOCSACTIVE, 0) != 0) { printf("[activetty] FAILED: release\n"); return 1; }
    fcntl(m, 4, O_NONBLOCK);
    neoos_test_inject_key(KEY_A, 1); neoos_test_inject_key(KEY_A, 0);
    r = read(m, b, sizeof b);
    if (r > 0) { printf("[activetty] FAILED: key leaked after release\n"); return 1; }

    printf("[activetty] ALL PASSED\n");
    return 0;
}
```

Wire a `ACTIVETTYTEST.ELF` rule + disk copy + `spawn /BIN/ACTIVETTYTEST.ELF`
+ `"[activetty] ALL PASSED"` in `REQUIRED_MARKERS`. `make test` →
`[activetty] FAILED` (`NEOOS_TIOCSACTIVE` returns `-EINVAL` today).

- [ ] **Step 2: `tty_set_active` / `tty_active`**

`tty.c`: `static struct tty *active_input_tty = &console_tty;`
```c
void tty_set_active(struct tty *t) { active_input_tty = t ? t : &console_tty; }
struct tty *tty_active(void) { return active_input_tty; }
```
Declare both in `tty.h`. `console_tty` is already file-static in
`tty.c`; `tty_console()` returns `&console_tty`.

- [ ] **Step 3: `console_set_fb_owned` + the guards**

`console.c`:
```c
static int fb_owned;
void console_set_fb_owned(int on) { fb_owned = on ? 1 : 0; }
int  console_fb_owned(void) { return fb_owned; }

void console_putc(char c) {
    if (fb_owned) { return; }                 /* userland owns the pixels */
    if (fb.present) { fbcon_putc(c); } else { vga_putc(c); }
}
```
Same early-return in `console_clear`. `console_write` already loops
`console_putc`. (Callers that want serial write it themselves — the
`console_output` tty backend and the exception path both do.)

- [ ] **Step 4: route input to the active tty**

`input.c`, the delivery line: `tty_input_char(tty_active(), (char)e->ascii);`
Include `dev/tty.h` if not already. No other change — grab handling is
unchanged.

- [ ] **Step 5: `NEOOS_TIOCSACTIVE` in `ptm_ioctl`; release in `ptm_close`**

`pty.h`: `#define NEOOS_TIOCSACTIVE 0x4E454F01`.
`pty.c` `ptm_ioctl` switch:
```c
case NEOOS_TIOCSACTIVE:
    if ((int64_t)(intptr_t)arg) {
        tty_set_active(&pt->slave);
        console_set_fb_owned(1);
        pt->owns_active = 1;
    } else if (pt->owns_active) {
        tty_set_active(NULL);
        console_set_fb_owned(0);
        pt->owns_active = 0;
    }
    return 0;
```
Add `int owns_active;` to `struct pty`. In `ptm_close`, **before** the
`pty_unref` that can free the struct:
```c
if (pt->owns_active) {
    tty_set_active(NULL);
    console_set_fb_owned(0);
    pt->owns_active = 0;
}
```
Include `dev/tty.h` and `dev/console.h` in `pty.c`.

- [ ] **Step 6: panic reclaim**

`isr.c` `exception_dump_and_halt`, first two statements:
```c
console_set_fb_owned(0);
tty_set_active(NULL);
```
(Include `dev/console.h`, `dev/tty.h`.) Now `console_write("EXCEPTION
- HALTED\n", 18)` repaints via `fbcon` over whatever TERM left.

- [ ] **Step 7: `FBIOGET_VSCREENINFO` reports r/g/b bitfields**

Check `kernel/dev/fb.c`'s `fb_ioctl` `FBIOGET_VSCREENINFO` handler — it
must fill `x.red/green/blue` (`struct fb_bitfield { u32 offset, length,
msb_right; }`) from `fb.r/g/b`. If it currently zeroes them, fill them.
Confirm `struct fb_var_screeninfo` in `fb.c` has the `red/green/blue/
transp` fields at the Linux offsets (it is a large struct; the M1a
version may be truncated — extend if so, it is kernel-internal).

- [ ] **Step 8: `make test` green; gauntlet; commit**

`[activetty] ALL PASSED`, every other marker unchanged. Gauntlet
15/15. Commit `"M1b-3: kernel active-tty + fb-ownership handoff, NEOOS_TIOCSACTIVE"`.

---

## Task 2: `render.c` — the glyph blitter

**Files:** Create `userland/term/render.{c,h}`, `userland/term/palette.{c,h}`,
`tools/rendertest.c`; modify `Makefile` (`render-check`).

**Interfaces:**
- Consumes: `font_term.h` (`term_glyphs`, `TERM_GLYPH_W/H`), `vt.h`
  (`vt_cell_at`, `vt_cursor`, `struct vt_cell`, `VT_*`).
- Produces:
  ```c
  /* render.h */
  struct term_fb {
      volatile uint8_t *pix;     /* framebuffer base */
      int pitch;                 /* bytes per scanline */
      int w, h;                  /* pixels */
      int ro, go, bo;            /* channel shift positions */
  };
  void render_span(const struct term_fb *fb, const struct vt *v,
                   int row, int col0, int col1);
  void render_cursor(const struct term_fb *fb, const struct vt *v);
  void render_clear(const struct term_fb *fb, uint32_t rgb);
  /* palette.h */
  extern const uint32_t vt_palette[256];   /* 0xRRGGBB */
  #define VT_DEFAULT_FG 0x00c8c8c8
  #define VT_DEFAULT_BG 0x00000000
  ```

- [ ] **Step 1: failing host test — `tools/rendertest.c`**

```c
/* host cc, not the cross toolchain. Proves render.c blits the right
   pixels; the on-hardware proof is Task 3's [term] render self-check. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../userland/term/vt.h"
#include "../userland/term/render.h"
#include "../userland/term/vt.c"
#include "../userland/term/render.c"
#include "../userland/term/palette.c"
#include "../userland/term/font_term.c"

static uint8_t buf[64 * 1024 * 4];
static struct term_fb FB = { buf, 800 * 4, 800, 48, 16, 8, 0 };  /* xRGB8888 */

static uint32_t px(int x, int y) {
    uint32_t v; memcpy(&v, (void *)(buf + y * FB.pitch + x * 4), 4);
    return v & 0x00ffffff;
}

int main(void) {
    struct vt v;
    vt_init(&v, 10, 2);
    vt_feed(&v, (const uint8_t *)"\x1b[31mA", 6);   /* red 'A' at cell (0,0) */
    struct vt_span sp[8];
    int n = vt_take_dirty(&v, sp, 8);
    render_clear(&FB, VT_DEFAULT_BG);
    for (int i = 0; i < n; i++) render_span(&FB, &v, sp[i].row, sp[i].col0, sp[i].col1);

    /* the 'A' glyph has ink somewhere in cell (0,0); it must be red */
    int red_seen = 0, bg_ok = 1;
    for (int y = 0; y < TERM_GLYPH_H; y++)
        for (int x = 0; x < TERM_GLYPH_W; x++) {
            uint32_t p = px(x, y);
            if (p == (vt_palette[1] & 0xffffff)) red_seen = 1;
            else if (p != (VT_DEFAULT_BG & 0xffffff)) bg_ok = 0;
        }
    /* cell (0,1) is untouched -> all background */
    for (int y = 0; y < TERM_GLYPH_H; y++)
        for (int x = TERM_GLYPH_W; x < 2 * TERM_GLYPH_W; x++)
            if (px(x, y) != (VT_DEFAULT_BG & 0xffffff)) bg_ok = 0;

    if (!red_seen) { printf("[rendertest] FAILED: no red ink\n"); return 1; }
    if (!bg_ok)    { printf("[rendertest] FAILED: stray pixels\n"); return 1; }
    printf("[rendertest] ALL PASSED\n");
    return 0;
}
```
`cc -std=c11 -Wall -Wextra -o /tmp/rendertest tools/rendertest.c` →
compile error (`render.c` absent).

- [ ] **Step 2: `palette.c` + `palette.h`**

`vt_palette[0..15]` = the standard ANSI + bright set (well-known RGB
values). `[16..231]` = the 6×6×6 cube: index `16 + 36r + 6g + b` →
`(r ? 55 + 40*r : 0)` per channel. `[232..255]` = grayscale `8 + 10*i`.
This is the canonical xterm-256 table — transcribe or compute it in a
static initializer / a one-time `palette_init()` (prefer a `const`
computed at build time via a tiny generator, or just a plain `const`
array written out — a `tools/gen-palette.py` that emits `palette.c` is
fine and mirrors `bdf2c.py`; the array is then checked in).

- [ ] **Step 3: `render.c`**

```c
static uint32_t pack(const struct term_fb *fb, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (r << fb->ro) | (g << fb->go) | (b << fb->bo);
}

void render_clear(const struct term_fb *fb, uint32_t rgb) {
    uint32_t v = pack(fb, rgb);
    for (int y = 0; y < fb->h; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb->pix + y * fb->pitch);
        for (int x = 0; x < fb->w; x++) row[x] = v;
    }
}

void render_span(const struct term_fb *fb, const struct vt *v,
                 int row, int col0, int col1) {
    for (int c = col0; c < col1; c++) {
        const struct vt_cell *cell = vt_cell_at(v, row, c);
        if (!cell) continue;
        uint32_t fg = (cell->attr & VT_DEFFG) ? VT_DEFAULT_FG : vt_palette[cell->fg];
        uint32_t bg = (cell->attr & VT_DEFBG) ? VT_DEFAULT_BG : vt_palette[cell->bg];
        if (cell->attr & VT_REVERSE) { uint32_t t = fg; fg = bg; bg = t; }
        if (cell->attr & VT_BOLD)    { fg = brighten(fg); }
        if (cell->attr & VT_HIDDEN)  { fg = bg; }
        uint32_t pfg = pack(fb, fg), pbg = pack(fb, bg);
        const uint8_t *g = term_glyphs[cell->ch];
        int x0 = c * TERM_GLYPH_W, y0 = row * TERM_GLYPH_H;
        for (int gy = 0; gy < TERM_GLYPH_H; gy++) {
            volatile uint32_t *dst =
                (volatile uint32_t *)(fb->pix + (y0 + gy) * fb->pitch) + x0;
            int under = (cell->attr & VT_UNDERLINE) && gy == TERM_GLYPH_H - 2;
            for (int gx = 0; gx < TERM_GLYPH_W; gx++) {
                int ink = (g[gy * 2 + (gx >> 3)] >> (7 - (gx & 7))) & 1;
                dst[gx] = (ink || under) ? pfg : pbg;
            }
        }
    }
}
```
`brighten(rgb)`: clamp each channel to `min(255, ch * 3 / 2)` (or map
`vt_palette[0..7]` → `[8..15]` when the index is known — simplest to
do it on the RGB). `render_cursor`: invert the 12×24 block at the
cursor cell when `vt_cursor`'s `visible` is set and the view is at the
bottom (`vt_view_offset(v) == 0`).

- [ ] **Step 4: host test passes; wire `make render-check`**

```
cc -std=c11 -Wall -Wextra -o /tmp/rendertest tools/rendertest.c && /tmp/rendertest
```
→ `[rendertest] ALL PASSED`. Add to the Makefile:
```make
.PHONY: render-check
render-check:
	cc -std=c11 -Wall -Wextra -o /tmp/neoos-rendertest tools/rendertest.c
	/tmp/neoos-rendertest
```
(and mention it beside `font-check` in the comment block).

- [ ] **Step 5: commit**

`git commit -m "M1b-3: render.c glyph blitter + palette + host rendertest"`.

---

## Task 3: the TERM process + TERMCHILD + integration

**Files:** Create `userland/term/main.c`, `userland/termchild.c`;
modify `Makefile`, `docs/stdlib.md`.

**Interfaces:**
- Consumes: everything above; `mmap_fd_raw`, `poll`, `fork`, `exec`,
  `open/read/write/close/ioctl`, `setsid` from libneoos.

- [ ] **Step 1: `userland/termchild.c`**

```c
#include <unistd.h>
#include <stdio.h>

/* Deterministic pattern for TERM's pixel self-check:
   clear, home, red 'R' at (0,0), then a line, then exit. */
int main(void) {
    printf("\x1b[2J\x1b[H");
    printf("\x1b[31mR\x1b[0mENDER-OK\r\n");
    printf("line two\r\n");
    /* let TERM drain + render before the slave closes */
    for (volatile long i = 0; i < 20000000L; i++) { }
    return 0;
}
```

- [ ] **Step 2: `userland/term/main.c` — TERM**

Sequence (all error paths: print `[term] FAILED: <what>` and exit 1 —
except a missing `/dev/fb0`, which prints `[term] SKIPPED: no fb` and
exits 0 so a text-mode boot still passes):

1. `open("/dev/ptmx", O_RDWR)` → `m`. `ioctl(m, TIOCGPTN, &n)`. Build
   `"/dev/pts/N"` by hand (no snprintf).
2. `open("/dev/fb0", O_RDWR)` → `fbfd`; `ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)`;
   `ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)` for the pitch. `len = pitch * h`,
   rounded up to a page. `mmap_fd_raw(0, len, RW, MAP_SHARED, fbfd, 0)`.
3. `struct term_fb FB = { map, pitch, w, h, vinfo.red.offset,
   vinfo.green.offset, vinfo.blue.offset }`.
4. `cols = min(w/12, VT_MAX_COLS); rows = min(h/24, VT_MAX_ROWS);`
   `vt_init(&V, cols, rows);` `render_clear(&FB, VT_DEFAULT_BG);`
5. `ioctl(m, NEOOS_TIOCSACTIVE, 1)`.
6. `fork()`:
   - child: `close(0); close(1); close(2);`
     `open("/dev/pts/N", O_RDWR)` ×3 (→ 0,1,2); `setsid();`
     `exec("/usr/tests/termchild.nex");` `_exit(127)` if it returns.
   - parent: continue.
7. Loop:
   ```c
   struct pollfd pf = { .fd = m, .events = POLLIN };
   for (;;) {
       int pr = poll(&pf, 1, 1000);
       if (pr > 0 && (pf.revents & POLLIN)) {
           uint8_t b[1024];
           int r = read(m, b, sizeof b);
           if (r <= 0) break;               /* slave closed */
           vt_feed(&V, b, r);
           struct vt_span sp[VT_MAX_ROWS];
           int nd = vt_take_dirty(&V, sp, VT_MAX_ROWS);
           for (int i = 0; i < nd; i++)
               render_span(&FB, &V, sp[i].row, sp[i].col0, sp[i].col1);
           render_cursor(&FB, &V);
       }
       if (pf.revents & (POLLHUP | POLLERR)) break;
   }
   ```
8. Reap the child (`wait4(child, &st, 0, 0)`).
9. **Self-check:** the centre pixel of cell (0,0) — where TERMCHILD's
   red `R` has ink — must equal `pack(&FB, vt_palette[1])`; a pixel in
   cell (0,20) (past `RENDER-OK`, blank) must equal
   `pack(&FB, VT_DEFAULT_BG)`. Print `[term] render ALL PASSED` or
   `[term] render FAILED: <detail>`.
   (Pick the sample pixel from a glyph row/col that `font_term`'s 'R'
   actually sets — verify once with `tools/font_check` output.)
10. `ioctl(m, NEOOS_TIOCSACTIVE, 0);` `return 0;`

- [ ] **Step 3: Makefile + INITTAB**

- `TERM.ELF` rule: links `userland/term/main.c vt.c render.c palette.c
  font_term.c` + crt0 + `-lneoos`, `-I userland/term`.
- `TERMCHILD.ELF` rule: `userland/termchild.c` (plain, like `looper`).
- Both added to `$(DISK_IMG)` prereqs; `mcopy … ::BIN/TERM.ELF` and
  `::BIN/TERMCHILD.ELF`.
- INITTAB: add `spawn /bin/term.nex` (near the end, after the noisy
  suites, so its render is the last thing on screen).
- `REQUIRED_MARKERS += "[term] render ALL PASSED"`.

- [ ] **Step 4: `make test`**

`rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'term|FAIL'`

Expected: `[term] render ALL PASSED`; every prior marker still present;
no `EXCEPTION`; powers off (TERM exits after TERMCHILD, releases the
active tty, init reaps everything). If the boot hangs, TERM is stuck in
`poll` — check the child actually `exec`'d TERMCHILD (fd setup) and the
slave close is seen as `read == 0`.

- [ ] **Step 5: verify the panic path by hand**

Temporarily add a `*(volatile int*)0 = 1;` to `termchild.c`, `make
test`, confirm the serial log shows `EXCEPTION` **and** that the boot
still completes/powers off (init's PID-1 panic is not triggered — the
faulting process is TERMCHILD, not init) and that no later test is
wedged by a still-owned framebuffer. Remove the line, rebuild.

- [ ] **Step 6: docs**

`docs/stdlib.md`: a short section — `NEOOS_TIOCSACTIVE` (ioctl on a pty
master, arg 1/0, claims cooked keyboard input + the framebuffer for a
userland terminal; released on master close; a panic forcibly
reclaims). Note it is a NeoOS extension with no Linux equivalent.

- [ ] **Step 7: gauntlet ×3, commit**

`for i in 1 2 3; do bash .superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 2 || break; done`
→ 45/45. Commit `"M1b-3: /bin/term.nex renders a PTY child to the framebuffer"`.

---

## Self-Review

**Spec coverage:**
- §1 architecture (TERM owns PTY, forks child, mmaps fb0, poll→feed→
  render loop) → Task 3 Step 2. VT engine and font are consumed, not
  rebuilt.
- §3 the "active terminal": `active_input_tty` + `tty_set_active` →
  Task 1 Step 2; `input.c` routing → Step 4; the pty-master ioctl
  (`NEOOS_TIOCSACTIVE`) → Step 5; `fb_owned_by_userland` +
  `console_write` serial-only → Step 3; release on master close →
  Step 5; fallback to `tty_console()` when unclaimed → the static
  initialiser + `tty_set_active(NULL)`.
- §3 "line-discipline division of labour" — printable keys cooked by
  the pts slave, TERM only handles specials — the cooked half is
  Task 1 (routing) + Task 3 (echo renders); the evdev specials half is
  **M1b-4**, correctly out of scope, and the Global Constraints say so.
- §6 panic reclaim → Task 1 Step 6; boot log via fbcon until TERM →
  unchanged (`fb_owned` starts 0); Task 3 Step 5 hand-verifies.
- §7 file layout (`main.c`, `vt.c`, `render.c`, `palette.c`,
  `font_term.c`; `termchild.c`; INITTAB entry; no new `lib/` wrapper)
  → Tasks 2–3 + the File Structure table.
- §8 testing: `[term] render ALL PASSED` via TERM's own mapping
  read-back (no new kernel hook) → Task 3 Step 2.9; `render.c` host
  test → Task 2; `activettytest` for the routing → Task 1.
- §9 M1b-3 acceptance ("kernel chatter serial-only after TERM starts,
  panic still paints, gauntlet ×3") → Task 1 Step 8, Task 3 Steps 5
  and 7.

**Placeholder scan:** `activettytest.c`'s "format by hand" path is
written out (the `pre[]` copy + one/two digit append). `render_span`
is given in full; `brighten` and `render_cursor` have their one-line
behaviour specified. `palette.c` names the exact cube/gray formulas and
offers the `gen-palette.py`-emits-checked-in-`.c` option (mirrors
`bdf2c.py`) rather than "fill in the table". The self-check sample
pixel is qualified with "verify once with `tools/font_check`". No "add
error handling" — every TERM error path's behaviour (FAIL+exit 1, or
SKIPPED+exit 0 for no-fb) is stated.

**Type consistency:** `NEOOS_TIOCSACTIVE = 0x4E454F01` in the Global
Constraints, `pty.h` (Task 1 Step 5), `activettytest.c` (Step 1), and
`main.c` (Task 3). `tty_set_active`/`tty_active`,
`console_set_fb_owned`/`console_fb_owned` spelled identically in the
interface block and every task. `struct term_fb` fields
(`pix,pitch,w,h,ro,go,bo`) match between `render.h` (Task 2), the host
test, and `main.c`. `TERM_GLYPH_W/H` = 12/24 from M1b-1. `VT_DEFFG/
VT_DEFBG/VT_REVERSE/...` from M1b-2's `vt.h`.

**Ordering:** Task 1 (kernel) and Task 2 (render, host-only) are
independent. Task 3 needs both. Each of Tasks 1 and 3 ends
gauntlet-green (Task 3 ×3); Task 2 ends host-check-green. A reviewer
can gate any task.
