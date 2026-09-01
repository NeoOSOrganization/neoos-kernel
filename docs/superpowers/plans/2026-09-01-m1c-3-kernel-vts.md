# M1c-3 — kernel virtual terminals — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** six kernel virtual terminals — `/dev/tty1..6`, each a text
grid with its own line discipline and modest scrollback, one visible at
a time, switched with `Alt+F1..F6`. The kernel log and panic screen
land on VT 1. `/dev/CONSOLE` and `/dev/tty0` follow the active VT. The
Linux `VT_*` / `KD*` ioctl surface (minus the process handshake, which
is M1c-4).

**Architecture:** `struct vt_console` wraps a `struct tty` (line
discipline reused) whose backend feeds a small in-kernel escape parser
(`kvt.c`) that mutates a `vc_cell` grid + a scrollback ring. Only the
*active* VT paints, through two new `con_driver` ops
(`putc_at(row,col,ch,attr)`, `cursor`). Input routes to the active VT
after the layer skips `Alt+Fn` / `Shift+PageUp/Dn`.

**Tech Stack:** C (freestanding kernel), GNU Make, headless QEMU +
serial markers, `neoos_test_inject_key` for the switch tests, the
parallel gauntlet.

**Spec:** `docs/superpowers/specs/2026-09-01-m1c-driver-model-and-vts-design.md` (§3 grid ops, §4, §5 minus VT_PROCESS, §9 row M1c-3)

## Global Constraints

- **`VT_COUNT` = 6.** `/dev/tty1..6`. `/dev/tty0` and `/dev/CONSOLE`
  resolve to `vts[vt_active].tty`.
- **The M1c-2 streaming `con_driver` ops stay** (`putc_attr`, `clear` —
  used by the banner and any direct `con_write`). M1c-3 *adds*
  `putc_at(int row, int col, char ch, uint8_t attr)` and
  `cursor(int row, int col, int visible)`. `attr` byte = `fg | (bg<<4)`,
  both `CON_*` 0..15.
- **`repaint` is not a `con_driver` op.** `kernel/tty/vt.c` owns the
  redraw loop (`vt_render_full` / `vt_render_dirty`), calling `putc_at`
  per cell — so `con_driver.h` never depends on `vt.h`.
- **VT grid geometry comes from the `con_driver`.** `con_driver_select`
  already reports `cols`/`rows`; `vt.c` sizes every VT to that (clamped
  to `VC_MAX_COLS` 200 / `VC_MAX_ROWS` 64). Scrollback: `VC_SCROLLBACK`
  = 256 lines per VT. `sizeof(struct vt_console)` ≈ 3.3 KB (tty) +
  `(256+64) * 200 * 2` ≈ 128 KB grid → ~130 KB × 6 ≈ 785 KB in `.bss`.
  Acceptable.
- **`VT_SETMODE` / `VT_RELDISP` / the `SIGUSR1/2` handshake are M1c-4.**
  M1c-3 switches are all `VT_AUTO`: instant. `KDSETMODE(KD_GRAPHICS)`
  is accepted and stops the kernel painting that VT, but nothing sets
  it yet (TERM does, in M1c-4).
- **The in-kernel parser is deliberately small** (spec §4): `BS CR LF
  HT BEL`, CSI `A B C D` (cursor), `H`/`f` (CUP), `J` (ED 0/1/2), `K`
  (EL 0/1/2), `m` (SGR: 0, 1 bold, 7 reverse, 22/27, 30–37 fg, 39,
  40–47 bg, 49), `\e[?25h/l` (cursor visible). Unknown sequences are
  consumed and dropped. The full xterm engine stays in userland
  `vt.c`.
- **These REQUIRED_MARKERS must survive:** `[tty] console line
  discipline ready`, `[banner]`, everything in the current list. New:
  `[kvt] selftest passed`, `[vt] selftest passed`, `[vtswitchtest] ALL
  PASSED`.
- **Work on `main`, one commit per task**, trailer:
  `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01CNR4gEkyMq6qhFWfxt3KXE`.
- **Regenerate disks** before a bare `make test`. **Never `make`
  during the gauntlet.**

## File Structure

| File | Change |
|---|---|
| `kernel/drivers/input/keyboard.h` / `.c` | `struct key_event` gains `uint32_t mods`; `keyboard_decode` fills it from the static `mods` |
| `kernel/tty/kvt.{c,h}` (new) | `struct vc_cell`, `struct kvt` parser state, `kvt_init` / `kvt_feed` / `kvt_cell` / `kvt_cursor` / grid + scrollback ring |
| `kernel/tty/vt.{c,h}` (new) | `struct vt_console`, `vts[6]`, `vt_init` (all 6 + wire tty backends), `vt_switch`, `vt_scroll`, `vt_active_console`, `vt_render_full`, the VT/KD ioctl handler, panic hook |
| `kernel/tty/con_driver.h` / `.c` | add `putc_at` + `cursor` to the vtable |
| `kernel/drivers/video/fbcon.c` | implement `fbcon_putc_at` (glyph at cell) + `fbcon_cursor` (inverse block); no scroll needed — `vt.c` addresses cells |
| `kernel/drivers/video/vgacon.c` | `vgacon_putc_at` (cell write) + `vgacon_cursor` (CRTC cursor regs, or an inverse cell) |
| `kernel/tty/dummycon.c` | `putc_at`/`cursor` no-ops |
| `kernel/tty/tty.c` | `tty_console()` → `vt_active_console()->tty`; drop `console_tty` + `console_backend`; `tty_init` calls `vt_init` |
| `kernel/tty/console.c` | `console_write`/`putc`/`clear` → feed the active VT (`vt_write_active`) |
| `kernel/drivers/input/input.c` | intercept `Alt+F1..F6` → `vt_switch`; `Shift+PageUp/Dn` → `vt_scroll`; else deliver to `vt_active_console()->tty` |
| `kernel/kernel.c` | `tty_init` order; `banner_show` still fine; panic reclaim → `vt_panic_reset()` |
| `kernel/arch/isr.c` | `exception_dump_and_halt` calls `vt_panic_reset()` (force VT 1, KD_TEXT, repaint) before its `console_write` |
| `kernel/fs/devfs.c` | `tty1`..`tty6` + `tty0` devices (before `pts`), `vt_file_ops` |
| `userland/vtswitchtest.c` (new) | writes markers to `/dev/tty2`, `VT_ACTIVATE`s it, `VT_GETSTATE`; key-injection `Alt+F2` |
| `Makefile` | `VTSWITCHTEST.ELF` + disk + inittab + `REQUIRED_MARKERS` |
| `docs/stdlib.md`, `docs/abi-compatibility.md`, `README.md` | Task 4 |

---

## Task 1: `key_event.mods` + the `kvt` parser

**Files:** modify `kernel/drivers/input/keyboard.{c,h}`; create
`kernel/tty/kvt.{c,h}`; modify `kernel/kernel.c` (call the selftest).

**Interfaces:**
- Produces:
  ```c
  // kvt.h
  #define VC_MAX_COLS   200
  #define VC_MAX_ROWS   64
  #define VC_SCROLLBACK 256
  #define VC_RING       (VC_SCROLLBACK + VC_MAX_ROWS)

  // attr bits
  #define VC_BOLD    0x01
  #define VC_REVERSE 0x02
  struct vc_cell { uint8_t ch; uint8_t fg; uint8_t bg; uint8_t attr; };

  struct kvt {
      int cols, rows;
      struct vc_cell ring[VC_RING][VC_MAX_COLS];
      int top, history, view;
      int cx, cy, cursor_visible;
      struct vc_cell pen;
      int pstate; int params[8]; int nparam; int priv;
      int bell;
  };
  void kvt_init(struct kvt *v, int cols, int rows);
  void kvt_feed(struct kvt *v, const char *p, unsigned n);
  const struct vc_cell *kvt_cell(const struct kvt *v, int row, int col);
  void kvt_cursor(const struct kvt *v, int *x, int *y, int *visible);
  void kvt_scroll_view(struct kvt *v, int delta);
  int  kvt_view(const struct kvt *v);
  ```

- [x] **Step 1: `key_event.mods`**

`keyboard.h`: add `uint32_t mods;` to `struct key_event` (after
`ascii`). `keyboard.c` `keyboard_decode`: `out->mods = mods;` right
before it returns 1. The decoder's static `mods` is already maintained
(`MOD_LALT`, `MOD_LSHIFT`, …). No behaviour change — a new field on an
event nobody reads yet.

- [x] **Step 2: failing selftest — `kvt_selftest()` in `kvt.c`**

```c
void kvt_selftest(void) {
    static struct kvt v;
    int fail = 0;
    kvt_init(&v, 10, 4);
    kvt_feed(&v, "hi", 2);
    if (kvt_cell(&v, 0, 0)->ch != 'h') { fail = 1; }
    kvt_feed(&v, "\r\nX", 3);
    if (kvt_cell(&v, 1, 0)->ch != 'X') { fail = 1; }        // LF/CR + print
    kvt_feed(&v, "\x1b[1;1HZ", 7);
    if (kvt_cell(&v, 0, 0)->ch != 'Z') { fail = 1; }        // CUP
    kvt_feed(&v, "\x1b[31mR", 6);
    if (kvt_cell(&v, 0, 1)->fg != CON_RED) { fail = 1; }    // SGR fg
    kvt_feed(&v, "\x1b[2J", 4);
    if (kvt_cell(&v, 0, 0)->ch != 0) { fail = 1; }          // ED 2
    // scroll into history
    kvt_init(&v, 4, 2);
    kvt_feed(&v, "a\r\nb\r\nc", 7);
    if (kvt_cell(&v, 0, 0)->ch != 'b') { fail = 1; }        // 'a' scrolled off
    kvt_scroll_view(&v, -1);
    if (kvt_cell(&v, 0, 0)->ch != 'a') { fail = 1; }        // history visible

    serial_write_string(fail ? "[kvt] selftest FAILED\n"
                             : "[kvt] selftest passed\n");
}
```
Call it from `kmain` right after `con_driver_select()`. `make test` →
`[kvt] selftest FAILED` (or a link error — `kvt.c` is a stub).

- [x] **Step 3: implement `kvt.c`**

A compact port of the userland `userland/term/vt.c` structure —
**same ring/scroll model** (screen row `r` = `ring[(top + r - view) %
VC_RING]`, scroll = advance `top` + bump `history`), but only the
control set in the Global Constraints. ~180 lines. Key points:
- `pen` starts `{ch:0, fg:CON_GREY, bg:CON_BLACK, attr:0}`; erases fill
  with that default.
- SGR `m`: `0` → reset pen; `1` → `VC_BOLD`; `7` → `VC_REVERSE`;
  `22`/`27` clear those; `30..37` → `pen.fg = n-30`; `39` →
  `pen.fg = CON_GREY`; `40..47` → `pen.bg = n-40`; `49` →
  `pen.bg = CON_BLACK`. `1` (bold) also bumps `fg` to the bright
  variant (`fg | 8`) at render time — do it in `kvt` so `fg` is the
  final index: on `VC_BOLD` set and `fg < 8`, `fg += 8`.
- `\e[?25h` / `?25l` → `cursor_visible`.
- Any `\e` sequence not recognised: consume to the final byte, drop.

- [x] **Step 4: selftest passes; commit**

`make test` → `[kvt] selftest passed`. Add to `REQUIRED_MARKERS`.
Gauntlet 15/15. Commit
`"M1c-3: key_event.mods + the in-kernel VT escape parser (kvt)"`.

---

## Task 2: `con_driver` grid ops + the VT layer

**Files:** modify `con_driver.{c,h}`, `fbcon.c`, `vgacon.c`,
`dummycon.c`, `tty.c`, `console.c`, `isr.c`, `kernel.c`; create
`kernel/tty/vt.{c,h}`.

**Interfaces:**
- Produces:
  ```c
  // con_driver.h -- additions
  void (*putc_at)(int row, int col, char ch, uint8_t attr);  // attr = fg|(bg<<4)
  void (*cursor)(int row, int col, int visible);

  // vt.h
  struct vt_console;                        // opaque; full def in vt.c
  void  vt_init(void);                      // build all VT_COUNT, wire ttys
  struct vt_console *vt_active_console(void);
  struct tty        *vt_active_tty(void);
  int   vt_active_index(void);              // 0-based
  void  vt_switch(int n);                   // 0-based; VT_AUTO, instant
  void  vt_scroll(int delta_lines);         // active VT scrollback
  void  vt_write_active(const char *s, unsigned n);   // kernel console_write
  void  vt_render_full(struct vt_console *vc);
  void  vt_panic_reset(void);               // force VT0, KD_TEXT, repaint
  int64_t vt_ioctl(int vt_index, uint64_t req, void *arg);  // -1 == active
  void  vt_selftest(void);
  ```

- [x] **Step 1: `con_driver` grid ops**

`fbcon_putc_at(row, col, ch, attr)`: draw `font8x16[ch]` into the
8×16 cell at `(col*8, row*16)`, `fg = con_rgb[attr & 15]`,
`bg = con_rgb[(attr >> 4) & 15]`, honour `VC_REVERSE` by swapping (the
caller already folds bold into `fg`). No cursor advance, no scroll —
`vt.c` addresses cells directly. `fbcon_cursor(row, col, visible)`:
invert the cell block (remember the previous cursor cell to restore).
`vgacon_putc_at`: `VGA_BUFFER[row*80+col] = ch | (attr << 8)`.
`vgacon_cursor`: program the CRTC cursor-location registers (0x0E/0x0F
via ports 0x3D4/0x3D5), or invert a cell. `dummycon`: no-ops. Add all
three to the driver structs.

- [x] **Step 2: `vt.c` — the layer**

```c
struct vt_console {
    struct tty tty;
    struct kvt scr;
    int kd_mode;                 // 0 = KD_TEXT, 1 = KD_GRAPHICS
    struct waitq wait_active;    // VT_WAITACTIVE sleepers
    struct vc_cell shown[VC_MAX_ROWS][VC_MAX_COLS];  // last painted (dirty diff)
};
static struct vt_console vts[VT_COUNT];
static int vt_active;
```

- `vt_init()`: for each VT, `kvt_init(&vc->scr, cols, rows)` (cols/rows
  from the con_driver), `tty_obj_init(&vc->tty, &vt_backend, vc)`,
  `waitq_init(&vc->wait_active)`. Set `vt_active = 0`.
- `vt_backend.output(t, s, n)`: `struct vt_console *vc = t->backend_priv;`
  `kvt_feed(&vc->scr, s, n); if (vc == &vts[vt_active] && vc->kd_mode == 0)
  vt_render_diff(vc);`  — so a write to a *background* VT updates its
  grid and paints nothing.
- `vt_render_diff(vc)`: for every cell, compare `kvt_cell(&vc->scr,r,c)`
  to `vc->shown[r][c]`; on a difference call
  `con_driver_active()->putc_at(r, c, cell->ch ? cell->ch : ' ', pack(cell))`
  and update `shown`. Then `con_driver_active()->cursor(...)` from
  `kvt_cursor`. `vt_render_full` is `vt_render_diff` after zeroing
  `shown` (forces every cell).
- `vt_switch(n)`: bounds-check; if `n == vt_active` return. `vt_active
  = n`; `con_driver_active()->clear()`; `vt_render_full(&vts[n])`;
  `waitq_wake_all(&vts[n].wait_active)`. (VT_PROCESS handshake: M1c-4.)
- `vt_scroll(delta)`: `kvt_scroll_view(&vts[vt_active].scr, delta);
  vt_render_full(&vts[vt_active])`.
- `vt_write_active(s, n)`: `tty_out_...`? No — just
  `tty_obj_write(&vts[vt_active].tty, s, n)` so it goes through the
  line discipline's ONLCR and then `vt_backend.output`. Guard: if
  `vt_active` VT is `KD_GRAPHICS`, still feed the grid (so switching
  back shows it) but skip the paint (the backend already does that).
- `vt_panic_reset()`: `vt_active = 0; vts[0].kd_mode = 0;
  con_driver_active()->clear(); vt_render_full(&vts[0]);`
- `vt_ioctl(idx, req, arg)`: `idx < 0` → `vt_active`. Handles
  `VT_ACTIVATE` (`vt_switch(arg-1)`), `VT_WAITACTIVE`
  (`while (vt_active != arg-1) waitq_sleep(&vts[arg-1].wait_active,0)`),
  `VT_GETSTATE` (`struct vt_stat { u16 v_active, v_signal, v_state; }`
  — `v_active = vt_active + 1`, `v_state` = bitmask of allocated VTs
  = `0x7E` for 6), `VT_OPENQRY` (always returns a valid number 1..6 —
  M1c has no "free" concept; return `*(int*)arg = vt_active + 1` or
  the lowest, spec says "1..6 or -EINVAL"), `KDSETMODE`
  (`vts[idx].kd_mode = arg; if switching to TEXT and active, repaint`),
  `KDGETMODE`. `VT_SETMODE`/`VT_RELDISP` → `return 0` **stub** (accept,
  do nothing — M1c-4 implements). Constants: `VT_ACTIVATE 0x5606`,
  `VT_WAITACTIVE 0x5607`, `VT_GETSTATE 0x5603`, `VT_OPENQRY 0x5600`,
  `VT_SETMODE 0x5602`, `VT_RELDISP 0x5605`, `KDSETMODE 0x4B3A`,
  `KDGETMODE 0x4B3B`, `KD_TEXT 0`, `KD_GRAPHICS 1`.

- [x] **Step 3: rewire `tty.c` / `console.c` / `isr.c`**

- `tty.c`: delete `console_tty`, `console_output`, `console_backend`.
  `struct tty *tty_console(void) { return vt_active_tty(); }`.
  `tty_init()` → `vt_init(); serial_write_string("[tty] console line
  discipline ready\n");`. `active_input_tty` initialiser →
  `0` meaning "the active VT" — add a helper: `tty_active()` returns
  `active_input_tty ? active_input_tty : vt_active_tty()`.
  `tty_set_active(NULL)` → `active_input_tty = 0`.
- `console.c`: `console_putc(c)` → `if (!fb_owned) vt_write_active(&c, 1);`
  `console_write(s,n)` → `vt_write_active(s,n)` (guarded). `console_clear`
  → `con_driver_active()->clear()` + `vt_render_full` of the active VT.
  Keep `fb_owned`.
- `isr.c` `exception_dump_and_halt`: replace the M1b-3
  `console_set_fb_owned(0); tty_set_active(0);` with
  `console_set_fb_owned(0); vt_panic_reset();` then the existing
  `console_write("EXCEPTION - HALTED\n", …)` lands on VT 1.
- `kernel.c`: `tty_init()` must run **after** `con_driver_select()`
  (VT sizing needs the driver's cols/rows). Check the current order —
  `tty_init` is around line 131; `con_driver_select` is ~110. Fine.
  `banner_show()` unchanged (streaming `putc_attr` — it paints, then
  VT 1 is blank underneath; the boot-log lines that follow are
  serial-only so the banner survives).

- [x] **Step 4: `[vt] selftest`**

`vt_selftest()`: `VT_COUNT` consoles exist; write `"probe\n"` to
`vts[2].tty` (VT 3) — assert `kvt_cell(&vts[2].scr, 0, 0)->ch == 'p'`
and that nothing painted (VT 3 not active — compare a `con_driver`
paint counter, or just that `vts[2].shown` stayed zero). `vt_switch(2)`
then `vt_switch(0)` do not fault. `vt_panic_reset()` forces index 0.
Print `[vt] selftest passed`. Call from `kmain` after `tty_init()`.

- [x] **Step 5: build, test, gauntlet, commit**

`REQUIRED_MARKERS += "[vt] selftest passed"`. `make test` green,
serial output unchanged (kernel log still serial; the VT grid is only
the framebuffer). Gauntlet 15/15. Commit
`"M1c-3: con_driver grid ops + the kernel VT layer (vts[6])"`.

---

## Task 3: `/dev/tty1..6`, the VT/KD ioctls, and the key intercepts

**Files:** modify `kernel/fs/devfs.c`, `kernel/tty/tty.c` (or a new
`vt_file_ops`), `kernel/drivers/input/input.c`; create
`userland/vtswitchtest.c`; modify `Makefile`.

- [x] **Step 1: `/dev/tty1..6` + `/dev/tty0` devices**

`devfs.c`: add seven entries **before `pts`** (which must stay last —
`DEVFS_PTS_INODE == DEVFS_COUNT`):
```c
    { "tty0", VNODE_DEVICE, &vt_file_ops, vt_dev_open },
    { "tty1", VNODE_DEVICE, &vt_file_ops, vt_dev_open },
    ... "tty6"
```
`vt_file_ops`: `read`/`write`/`poll` route to the target VT's
`struct tty` via `tty_obj_*`; `ioctl` routes to `vt_ioctl(index, …)`
first (the `VT_*`/`KD*` numbers) and falls through to
`tty_obj_ioctl(&vc->tty, …)` for `TCGETS` &c. The device stashes its
VT index in `f->priv` (0 for tty0 = "active"; 1..6 → index-1). A helper
maps the devfs name suffix digit to the index in `vt_dev_open`.
`/dev/CONSOLE` (`tty_file_ops`) is unchanged in code but now follows
the active VT because `tty_console()` does.

- [x] **Step 2: input intercepts**

`input.c`, in `input_key_event`, **before** the cooked-delivery block,
using the new `e->mods`:
```c
if (e->pressed && (e->mods & (MOD_LALT | MOD_RALT))) {
    if (e->keycode >= KEY_F1 && e->keycode <= KEY_F6) {   // 59..64
        vt_switch(e->keycode - KEY_F1);
        goto done;                                        // consume
    }
}
if (e->pressed && (e->mods & (MOD_LSHIFT | MOD_RSHIFT))) {
    if (e->keycode == KEY_PAGEUP)   { vt_scroll(-(rows/2)); goto done; }
    if (e->keycode == KEY_PAGEDOWN) { vt_scroll(+(rows/2)); goto done; }
}
```
(`goto done` skips both the evdev fan-out *and* the tty delivery — a
VT hotkey is not an input event. Or fan it out to evdev but skip the
tty; match Linux, which consumes it entirely. Consume entirely.)
`rows` = `vt_active_console()`'s row count — expose a getter.

- [x] **Step 3: `userland/vtswitchtest.c`**

```c
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <neoos_test.h>

#define VT_ACTIVATE 0x5606
#define VT_GETSTATE 0x5603
#define KEY_F2 60

struct vt_stat { unsigned short v_active, v_signal, v_state; };

int main(void) {
    int t2 = open("/dev/tty2", O_RDWR);
    if (t2 < 0) { printf("[vtswitchtest] FAILED: open tty2 %d\n", t2); return 1; }
    write(t2, "on VT2\n", 7);                 // updates VT2 grid, paints nothing

    int t0 = open("/dev/tty0", O_RDWR);
    if (ioctl(t0, VT_ACTIVATE, (void *)2) != 0) { printf("[vtswitchtest] FAILED: activate\n"); return 1; }

    struct vt_stat st;
    if (ioctl(t0, VT_GETSTATE, &st) != 0 || st.v_active != 2) {
        printf("[vtswitchtest] FAILED: v_active=%d\n", st.v_active); return 1;
    }

    // Alt+F1 via injected keys -> back to VT1 (test-hook builds only)
    if (neoos_test_inject_key(56 /*KEY_LEFTALT*/, 1) == 0) {
        neoos_test_inject_key(59 /*KEY_F1*/, 1);
        neoos_test_inject_key(59, 0);
        neoos_test_inject_key(56, 0);
        ioctl(t0, VT_GETSTATE, &st);
        if (st.v_active != 1) { printf("[vtswitchtest] FAILED: alt+f1 -> %d\n", st.v_active); return 1; }
    } else {
        printf("[vtswitchtest] switch-key check skipped (production kernel)\n");
    }

    // restore
    ioctl(t0, VT_ACTIVATE, (void *)1);
    printf("[vtswitchtest] ALL PASSED\n");
    return 0;
}
```
Makefile: `VTSWITCHTEST.ELF` rule, disk copy, `spawn /BIN/VTSWITCHTEST.ELF`
in the INITTAB, `"[vtswitchtest] ALL PASSED"` in `REQUIRED_MARKERS`.

- [x] **Step 4: `make test`, panic hand-check, gauntlet ×3**

`make test` → `[vtswitchtest] ALL PASSED`, every prior marker present,
powers off. Hand-check the panic path: temporarily `*(volatile int*)0
= 1;` in a kernel selftest, confirm `EXCEPTION - HALTED` appears (on
serial; on screen it repaints VT 1). Remove it.
Gauntlet **×3** — VT layer + input routing are boot-critical. 45/45.
Commit `"M1c-3: /dev/tty1..6, VT_*/KD* ioctls, Alt+Fn / Shift+PageUp intercepts"`.

---

## Task 4: docs

- [x] **Step 1: `docs/stdlib.md`** — a "Virtual terminals" section:
  `/dev/tty1..6`, `/dev/tty0` = active, `Alt+F1..F6`,
  `Shift+PageUp/Down`. The `VT_*` / `KD*` ioctls with their Linux
  numbers and the divergences: **6 VTs not 63**, `VT_SETMODE` /
  `VT_RELDISP` accepted but inert until M1c-4, `VT_OPENQRY` never fails
  (no "free VT" pool), no `KD_MEDIUMRAW` / raw-keyboard modes,
  `v_signal` always 0.

- [x] **Step 2: `docs/abi-compatibility.md`** — VT/KD ioctls in the
  ioctl table; "what a ported console app hits": the `VT_PROCESS`
  handshake is M1c-4, so a display server can't yet cooperate with
  console switching.

- [x] **Step 3: `README.md`** — "six virtual terminals, `Alt+F1..F6`".

- [x] **Step 4: commit** `"M1c-3: docs -- virtual terminals, VT/KD ioctls"`.

---

## Self-Review

**Spec coverage (§3 grid ops, §4, §5):**
- `struct fb_device` unaffected; `con_driver` gains `putc_at` +
  `cursor` (§3 grid-addressed ops) → Task 2 Step 1. `repaint(vc)` is
  **not** added — `vt.c` owns the loop (Global Constraints), so
  `con_driver.h` stays free of `vt.h`. Noted deviation, same effect.
- 6 VTs, `/dev/tty1..6`, `/dev/tty0`/`/dev/CONSOLE` = active (§4) →
  Task 2 (`tty_console`), Task 3 Step 1.
- In-kernel cut-down parser (§4) → Task 1 (`kvt.c`), control set in
  Global Constraints.
- Only the active VT paints; a write to a background VT updates its
  grid silently (§4) → Task 2 Step 2 `vt_backend.output`.
- Keyboard → active VT; `Alt+Fn` / `Shift+PageUp` intercepted (§4) →
  Task 3 Step 2 (needs `key_event.mods` from Task 1 Step 1).
- Boot log + panic on a VT (§4) → Task 2 Step 3 (`isr.c`,
  `vt_panic_reset`).
- `VT_*` / `KD*` ioctls (§5) → Task 2 Step 2 `vt_ioctl`, Task 3 Step 1.
  **`VT_SETMODE` / `VT_RELDISP` / `SIGUSR1/2` handshake deferred to
  M1c-4** — Global Constraints; M1c-3 switches are `VT_AUTO`.

**Placeholder scan:** `kvt.c`'s body is "a compact port of userland
`vt.c`'s ring/scroll model with only <listed control set>" — the model
is a known quantity (M1b-2, in-tree) and the control set is enumerated;
not "implement a parser". The `vt_ioctl` cases are each spelled out
with their constant and effect. `vgacon_cursor` names the CRTC
registers (0x0E/0x0F via 0x3D4/0x3D5) or the fallback. The selftests
are given in full. No "add error handling".

**Type consistency:** `struct vc_cell {ch,fg,bg,attr}` and `VC_*`
(`VC_BOLD` 0x01, `VC_REVERSE` 0x02) identical in `kvt.h`, the selftests,
and `fbcon_putc_at`. `con_driver` new ops
`putc_at(int,int,char,uint8_t)` / `cursor(int,int,int)` match between
`con_driver.h`, `fbcon`/`vgacon`/`dummycon`, and `vt_render_diff`.
`attr = fg | (bg<<4)` convention stated the same in Global Constraints,
Task 2 Step 1, and `vt.c`'s `pack(cell)`. VT/KD ioctl numbers match
Task 2 Step 2, Task 3's `vtswitchtest`, and Linux. `key_event.mods` /
`MOD_LALT` / `KEY_F1`(59) / `KEY_PAGEUP`(104) match Task 1 Step 1 and
Task 3 Step 2.

**Ordering:** Task 1 (parser + mods) is independent. Task 2 needs
Task 1's `kvt`. Task 3 needs Task 2's `vt_switch`/`vt_ioctl` and Task
1's `mods`. Task 4 last. Each of 1–3 ends `make test`- and
gauntlet-green (Task 3 ×3).
