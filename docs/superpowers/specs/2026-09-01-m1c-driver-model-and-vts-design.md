# M1c — driver model, display abstraction, kernel virtual terminals — design

> **Status (2026-09-01): M1c-1, M1c-2 and M1c-3 shipped. M1c-4 was
> dropped from the roadmap.**
> The `VT_SETMODE` / `VT_RELDISP` / `SIGUSR1` handshake, scrollback and
> evdev keys on the VT described below are no longer planned. M1c-3
> ships `VT_AUTO` switching, which is what an interactive shell on
> `/dev/tty1..6` needs; the handshake only matters to a display server
> that wants to veto a switch, and NeoOS has none. `VT_RELDISP` stays
> accepted-but-inert, as M1c-3 documented. Everything else in this spec
> describes shipped code. See
> `docs/superpowers/specs/2026-08-31-post-smp-roadmap.md`.

**Milestone:** M1c, after M1b (framebuffer terminal). Precedes the
concurrency-hardening and BusyBox tracks
(`docs/superpowers/specs/2026-08-31-post-smp-roadmap.md`).

**Goal:** get the kernel out of the business of *assuming* a display.
Today `console.c` hard-codes `fb.present ? fbcon : vga`, and the
framebuffer is whatever the Multiboot2 tag said. M1c turns that into a
**registered driver model with a boot-time probe**, reorganises every
hardware driver into `kernel/drivers/`, and adds a **kernel virtual
terminal layer** (`/dev/tty1..6`, `Alt+F1..F6`) that the M1b userland
terminal becomes a *client* of, through the Linux VT/KD ioctl
protocol.

**Origin:** brainstormed 2026-09-01 (session
`session_01CNR4gEkyMq6qhFWfxt3KXE`), immediately after M1b-3. The user
chose the maximal option on every axis: a full display-device model, a
kernel VT layer with TERM as a client, and moving *all* hardware
drivers into `kernel/drivers/`.

**Note — M1c-4 re-does M1b-3's TERM.** `NEOOS_TIOCSACTIVE` (the
"userland owns the whole framebuffer" ioctl from M1b-3) is replaced by
the standard `VT_SETMODE` / `KDSETMODE` / `VT_ACTIVATE` protocol.
M1b-4's scrollback + evdev-key work is folded into M1c-4 so it is
built once, against the VT-client model.

---

## 1. `kernel/drivers/` reorganisation (M1c-1)

Pure move, **zero behaviour change**. `git mv` + a scripted
`#include` rewrite, one commit, verified by `make test` producing a
byte-identical serial log.

| From `kernel/dev/` | To |
|---|---|
| `vga.{c,h}` | `kernel/drivers/video/vgacon.{c,h}` (renamed — it is a console driver, §3) |
| `fb.{c,h}` | `kernel/drivers/video/vesafb.{c,h}` |
| `fbcon.{c,h}`, `font8x16.c` | `kernel/drivers/video/fbcon.{c,h}`, `font8x16.c` |
| `keyboard.{c,h}`, `keymap_us.h` | `kernel/drivers/input/keyboard.*` |
| `evdev.{c,h}`, `input.{c,h}` | `kernel/drivers/input/` |
| `ata.{c,h}` | `kernel/drivers/block/ata.{c,h}` |
| `serial.{c,h}` | `kernel/drivers/char/serial.{c,h}` |
| `rtc.{c,h}`, `pit.{c,h}`, `timer.{c,h}` | `kernel/drivers/char/` |
| `lapic.{c,h}`, `ioapic.{c,h}`, `pic.{c,h}` | `kernel/drivers/irq/` |
| `acpi.{c,h}` | `kernel/drivers/acpi/` |

Stays out of `drivers/` — these are subsystems, not hardware drivers:

| `kernel/dev/` file | To |
|---|---|
| `tty.{c,h}`, `pty.{c,h}` | `kernel/tty/` |
| `console.{c,h}` | `kernel/tty/` — becomes the con_driver dispatch (§3) |

`kernel/dev/` is deleted. Makefile `KERNEL_DIRS` gains
`kernel/drivers/video kernel/drivers/input kernel/drivers/block
kernel/drivers/char kernel/drivers/irq kernel/drivers/acpi kernel/tty`.

**Method:** a throwaway `tools/reorg-drivers.sh` that does the `git mv`s
and `sed -i` on every `#include "dev/…"` across `kernel/`. Committed
result only; the script is not kept. The commit message lists the
mapping. `make test` and the gauntlet must pass unchanged.

---

## 2. `struct fb_device` — the linear-framebuffer provider (M1c-2)

A DRM-lite abstraction over "a chunk of memory the scanout reads". One
implementation now (`vesafb`); a `bochs-drm` or `virtio-gpu` driver
could implement it later without touching any caller.

```c
// kernel/drivers/video/fb_device.h
struct fb_channel { uint8_t offset, length; };

struct fb_mode {
    uint32_t width, height;
    uint32_t bpp;                 // 32 only, for now
    uint32_t pitch;              // bytes per scanline in this mode
    struct fb_channel r, g, b;
};

struct fb_device {
    const char *name;
    int priority;               // higher wins the probe

    // Return 1 if this driver can drive the display on this machine.
    int (*probe)(void *multiboot_info);

    // Current mode + the physical framebuffer for it.
    void (*current)(struct fb_mode *out, uint64_t *phys, uint64_t *size);

    // Mode list + set. A single-mode device returns its one mode and
    // rejects set_mode of anything not equal to it.
    int (*modes)(struct fb_mode *out, int max);
    int (*set_mode)(const struct fb_mode *m);

    // Push a dirty rectangle to the scanout. No-op for a plain linear
    // framebuffer; real work for virtio-gpu. The con_driver and the
    // userland client both call it after drawing.
    void (*flush_rect)(int x, int y, int w, int h);

    // Optional hardware cursor. NULL -> the VT layer / client draws a
    // software cursor.
    int (*cursor)(int x, int y, int visible);
};

// registration + selection
void             fb_device_register(struct fb_device *d);
struct fb_device *fb_device_active(void);        // NULL on a text-only machine
void             fb_device_probe_all(void *multiboot_info);
```

- **`vesafb`** (`kernel/drivers/video/vesafb.c`): `probe` = the MB2 tag
  carried a type-1 RGB 32bpp framebuffer. `current`/`modes` = the one
  mode GRUB set. `set_mode` accepts only that mode. `flush_rect` =
  no-op. `cursor` = NULL. **This is where the M1b-3 XRGB8888 workaround
  is fixed for real:** if the MB2 tag's channel offsets are
  inconsistent (`r==g` or out of range), `vesafb` falls back to
  `{16,8},{8,8},{0,8}` and logs it — so `/dev/fb0` and the con_driver
  report a trustworthy layout and TERM no longer needs its own guess.
- `/dev/fb0` becomes a thin shim over `fb_device_active()`:
  `FBIOGET_VSCREENINFO` reads `current()`, `mmap` maps `phys`,
  `FBIOPUT_VSCREENINFO` routes to `set_mode` (still `-EINVAL` for
  vesafb, but a real driver could accept it).

---

## 3. `struct con_driver` — painting a text grid (M1c-2)

Separate from `fb_device` because **VGA text mode and a linear
framebuffer are different hardware**: one takes `(char, attr)` pairs at
`0xb8000`, the other takes pixels. Linux splits these the same way
(`con_driver`: `fbcon`, `vgacon`, `dummycon`).

```c
// kernel/tty/con_driver.h
struct con_driver {
    const char *name;
    int priority;

    int  (*probe)(void);                     // can this drive the console?
    void (*init)(int *cols, int *rows);      // set up, report the text geometry
    void (*putc)(int row, int col, char ch, uint8_t attr);
    void (*clear)(void);
    void (*scroll)(int lines);               // scroll the whole screen up
    void (*repaint)(const struct vt_console *vc);   // full redraw (VT switch)
    void (*cursor)(int row, int col, int visible);
};

void            con_driver_register(struct con_driver *d);
struct con_driver *con_driver_active(void);
void            con_driver_select(void);      // probe, pick highest priority
```

Implementations:

| driver | `probe` | notes |
|---|---|---|
| `fbcon` | `fb_device_active() != NULL` | renders glyphs (font8x16) into the active `fb_device`; calls `flush_rect` after each op. priority 100. |
| `vgacon` | always (checks `0xb8000` is usable) | 80×25, colour attrs. priority 10. |
| `dummycon` | always | discards everything; for a truly headless boot. priority 0. |

`console.c` (now `kernel/tty/console.c`) loses its `fb.present`
branch — `console_putc` etc. dispatch through `con_driver_active()`.
`console_set_fb_owned` is replaced by the per-VT graphics mode (§5).

Boot order in `kmain`: `fb_device_probe_all()` → `con_driver_select()`
→ `vt_init()`. Everything before that (very early serial) is unchanged.

---

## 4. Kernel virtual terminals (M1c-3)

`kernel/tty/vt.c`. `VT_COUNT` = 6.

```c
struct vt_console {
    struct tty tty;                 // full line discipline (reuse struct tty)
    struct vc_cell *cells;          // cols*rows, + SCROLLBACK lines of history
    int cols, rows;
    int cx, cy;
    int scrollback;                 // lines of history retained
    int view;                       // lines scrolled up (0 = live)
    int kd_mode;                    // KD_TEXT (kernel paints) or KD_GRAPHICS (client)
    struct vt_mode vtmode;          // VT_AUTO or VT_PROCESS + relsig/acqsig
    int owner_pid;                  // process that did VT_SETMODE(VT_PROCESS), or 0
    uint8_t dirty;
};

struct vc_cell { uint8_t ch; uint8_t attr; };   // attr: fg|bg|bold, VGA-ish

static struct vt_console vts[VT_COUNT];
static int vt_active;                // 0..VT_COUNT-1
```

- Each VT is `/dev/tty1..6` in devfs (static entries, like `CONSOLE`).
  `/dev/tty0` and `/dev/console` resolve to `&vts[vt_active].tty`.
- A write to `/dev/ttyN` feeds that VT's grid through a **small
  in-kernel escape parser** — a cut-down version of the userland
  `vt.c`: `BS CR LF TAB`, `\e[H`, `\e[J`, `\e[K`, `\e[m` (SGR: the 8
  colours + bold), `\e[<n>;<m>H`. Not the full xterm set — the
  userland TERM is where that lives. If the kernel parser sees a
  sequence it does not know, it drops it.
- **Rendering:** only the *active* VT is painted, and only when its
  `kd_mode == KD_TEXT`. A write to a non-active VT updates its grid but
  paints nothing. `con_driver_active()->repaint(&vts[vt_active])` on a
  switch.
- **Keyboard:** `input_key_event` delivers cooked bytes to
  `vts[vt_active].tty` (replacing `tty_active()`), *unless* the active
  VT is `KD_GRAPHICS` and has a raw-input client (evdev grab), in which
  case the current evdev path already handles it.
- Kernel boot log + `kmain` selftests write to `vts[0]` (VT 1).
  `panic()` forces `vt_active = 0`, `vts[0].kd_mode = KD_TEXT`, and
  repaints before its dump — the M1b-3 `exception_dump_and_halt`
  reclaim generalises to this.
- Scrollback: `Shift+PageUp/Down` on the active VT (intercepted in the
  input layer like the switch keys) adjusts `view` and repaints.

---

## 5. The VT-switch and graphics-mode protocol (M1c-3 kernel side, M1c-4 client side)

NeoOS adopts the **Linux VT/KD ioctl surface** — the roadmap's north
star is running real Linux programs, and a ported `fbterm`, `kmscon`,
or an X server expects exactly this. Numbers and semantics are Linux's;
they act on `/dev/ttyN` or `/dev/tty0`.

| ioctl | value | effect |
|---|---|---|
| `VT_ACTIVATE` | `0x5606` | switch to VT `arg` (1-based). Honours `VT_PROCESS` (below). |
| `VT_WAITACTIVE` | `0x5607` | block until VT `arg` is active. |
| `VT_GETSTATE` | `0x5603` | `struct vt_stat { v_active, v_signal, v_state }` |
| `VT_OPENQRY` | `0x5600` | lowest free VT number (M1c: always 1..6, or `-EINVAL` if all claimed) |
| `VT_SETMODE` | `0x5602` | `struct vt_mode { char mode; char waitv; short relsig, acqsig, frsig; }`. `mode = VT_AUTO` (0) or `VT_PROCESS` (1). |
| `VT_RELDISP` | `0x5605` | `arg`: 0 = refuse the switch, 1 = ack a release, 2 = ack an acquire. |
| `KDSETMODE` | `0x4B3A` | `arg`: `KD_TEXT` (0) or `KD_GRAPHICS` (1). In `KD_GRAPHICS` the kernel stops painting that VT. |
| `KDGETMODE` | `0x4B3B` | current KD mode |

**Switch flow** (`vt_switch(target)`):
1. If the current VT is `VT_AUTO`: repaint away immediately (step 4).
2. If `VT_PROCESS` and an `owner_pid`: send `relsig` (default
   `SIGUSR1`) to the owner and mark the switch *pending*. The owner
   stops drawing and calls `VT_RELDISP(1)` (or `VT_RELDISP(0)` to
   veto). On the ack, proceed.
3. `vt_active = target`.
4. `con_driver_active()->repaint(&vts[target])` if `KD_TEXT`; if the
   new VT is a `VT_PROCESS` owner in `KD_GRAPHICS`, send its `acqsig`
   (default `SIGUSR2`) instead — the owner redraws and calls
   `VT_RELDISP(2)`.

**Timeout:** an owner that does not ack a release within ~1.5 s is
force-switched (Linux behaviour). A dead owner (`proc_find` fails) is
force-switched immediately.

`struct vt_mode`, `struct vt_stat`, the `SIGUSR1/2` defaults, and the
constant values all match Linux x86-64. Recorded in
`docs/stdlib.md` with the divergences (no `KD_MEDIUMRAW`/`K_RAW`
keyboard modes, `frsig` accepted-ignored, 6 VTs not 63).

---

## 6. `/bin/term.nex` reworked as a VT client (M1c-4)

M1b-3's TERM is rewritten to be a well-behaved VT client instead of
grabbing the whole framebuffer:

1. `open("/dev/tty0")` (the VT multiplexer). `ioctl(VT_OPENQRY)` → a
   free VT number `N`; `ioctl(VT_ACTIVATE, N)` + `VT_WAITACTIVE`.
2. `ioctl(N_fd, VT_SETMODE, {VT_PROCESS, 0, SIGUSR1, SIGUSR2, 0})` and
   install handlers for both.
3. `ioctl(N_fd, KDSETMODE, KD_GRAPHICS)`.
4. `open("/dev/fb0")`, `FBIOGET_VSCREENINFO` (now trustworthy — §2),
   `mmap`.
5. Allocate a PTY, `fork` + `dup2` the slave onto the child's 0/1/2
   (the M1b-3 mechanics, unchanged), `exec` the child.
6. `poll({master, /dev/input/event0})` render loop (M1b-2 `vt.c` +
   M1b-3 `render.c`), **plus** the M1b-4 work folded in here:
   scrollback view on `Shift+PageUp/Down`, evdev raw keys (arrows,
   Home/End, PageUp/Down, F-keys) translated to escape sequences and
   written to the master.
7. **`SIGUSR1` handler** (release): set a flag; the render loop stops
   drawing and calls `VT_RELDISP(1)`. **`SIGUSR2` handler** (acquire):
   set a flag; the loop does a full repaint from the grid and calls
   `VT_RELDISP(2)`.
8. On child exit: `KDSETMODE(KD_TEXT)`, `VT_SETMODE(VT_AUTO)`,
   `VT_ACTIVATE(1)`, exit. init respawns it.

The `neoos_test_inject_key` scripted-session test from M1b-3 grows:
inject `Alt+F2` mid-session, assert the shell keeps running but stops
receiving input; inject `Alt+F1` (or back to TERM's VT), assert it
resumes.

Deleted: `NEOOS_TIOCSACTIVE`, `console_set_fb_owned` / `fb_owned`,
`tty_set_active` / `active_input_tty` (subsumed by `vt_active` and the
per-VT `kd_mode`). `activettytest` is replaced by a `vttest`-style
kernel-VT test.

---

## 7. What existing code changes

| M1b-3 artefact | M1c fate |
|---|---|
| `NEOOS_TIOCSACTIVE` on the pty master | deleted; replaced by `VT_*`/`KD*` on `/dev/ttyN` |
| `active_input_tty` + `tty_set_active`/`tty_active` | deleted; `input.c` delivers to `vts[vt_active].tty` |
| `fb_owned` + `console_set_fb_owned` | deleted; per-VT `kd_mode` |
| `exception_dump_and_halt` reclaim | generalised: force VT 1, `KD_TEXT`, repaint |
| `userland/term/main.c` (TERM) | rewritten per §6 |
| `userland/activettytest.c` | deleted; kernel-VT test added |
| `/dev/fb0` XRGB8888 guess in TERM | deleted; `vesafb` reports the right layout |
| `dup`/`dup2`/`dup3` (pulled forward in M1b-3) | kept as-is |
| `console.c` `fb.present ? fbcon : vga` | `con_driver_active()->…` |
| `fb.c` (`struct fb_info fb`) | becomes `vesafb`, an `fb_device` impl |
| `vga.c` | becomes `vgacon`, a `con_driver` impl |
| `fbcon.c` | becomes a `con_driver` impl over `fb_device` |

---

## 8. Testing (headless)

- **M1c-1:** `make test` serial log identical to the pre-reorg
  baseline **after normalising the known non-determinism** (pid
  numbers, `[timer] tick=` counters, RTC timestamps) — every
  `selftest passed` / `ALL PASSED` / `[…]` marker line present in the
  same order. Capture the baseline to a scratch file before the reorg,
  `diff` the normalised logs. Gauntlet 15/15.
- **M1c-2:** a kernel selftest `[fbdev] selftest passed` — asserts
  exactly one `fb_device` probed true, its `current()` matches the MB2
  tag, `flush_rect` is callable. `[con] selftest passed` — the
  selected `con_driver` matches expectation (`fbcon` under `-vga std`),
  `putc`/`clear`/`repaint` don't fault. `/dev/fb0` still passes
  `fbtest`.
- **M1c-3:** `[vt] selftest passed` — 6 VTs registered, `/dev/tty3`
  opens, a write to an inactive VT updates its grid and paints
  nothing, `vt_switch` repaints, `panic()` path forces VT 1. A
  userland `vtswitchtest` — writes a marker to `/dev/tty2`, `VT_ACTIVATE`s
  it, reads it back; `VT_GETSTATE` reflects the switch. The keyboard
  `Alt+F2` intercept is driven by `neoos_test_inject_key`.
- **M1c-4:** the M1b-3 `[term] render ALL PASSED` test, retargeted at
  the VT-client TERM, plus the `Alt+Fn` release/acquire assertions
  (§6). Gauntlet ×3 (TERM + VT layer are boot-critical).
- Every sub-milestone refreshes `docs/abi-compatibility.md` (the
  `VT_*`/`KD*` ioctls) and `docs/stdlib.md`.

---

## 9. Sub-milestone decomposition

| # | Deliverable | Ends when |
|---|---|---|
| **M1c-1** | `kernel/drivers/` + `kernel/tty/` reorg. `git mv` + include rewrite, Makefile `KERNEL_DIRS`. No behaviour change. | serial log diff-identical, gauntlet 15/15. |
| **M1c-2** | `struct fb_device` + `vesafb`; `struct con_driver` + `fbcon`/`vgacon`/`dummycon`; boot probe; `console.c` and `/dev/fb0` rewired; vesafb fixes the channel-layout bug. | `[fbdev]` + `[con]` selftests, `fbtest` green, gauntlet 15/15. |
| **M1c-3** | Kernel VT layer: `vts[6]`, `/dev/tty1..6`, in-kernel escape parser, `Alt+Fn` + `Shift+PageUp/Dn` intercepts, `vt_switch`, the `VT_*`/`KD*` ioctls, panic path. Boot log moves to VT 1. | `[vt]` selftest + `vtswitchtest`, gauntlet 15/15. |
| **M1c-4** | TERM rewritten as a `VT_PROCESS` client (§6); M1b-4 scrollback + evdev-key translation folded in; `NEOOS_TIOCSACTIVE` and friends deleted; docs. | `[term] render ALL PASSED` + `Alt+Fn` handoff test, gauntlet ×3, docs refreshed. |

M1c-1 is independent and low-risk (but wide). M1c-2 needs M1c-1's
layout. M1c-3 needs M1c-2's `con_driver`. M1c-4 needs M1c-3.

---

## 10. Decisions already settled (do not relitigate in sub-specs)

| Question | Decision |
|---|---|
| VT multiplexing | **Kernel VT layer**; `/bin/term.nex` is a `VT_PROCESS` client, not a wholesale fb grab. |
| Driver reorg scope | **Everything hardware-facing** → `kernel/drivers/{video,input,block,char,irq,acpi}/`; `tty`/`pty`/`console` → `kernel/tty/`; `kernel/dev/` deleted. |
| Display abstraction | **Two interfaces**: `fb_device` (linear-fb provider, DRM-lite) and `con_driver` (text-grid painter). Matches Linux's fbdev-vs-con_driver split. |
| Switch protocol | **Linux `VT_SETMODE`/`VT_RELDISP`/`KDSETMODE` + `SIGUSR1`/`SIGUSR2`** — ABI-compatible with real Linux console clients. |
| VT count | **6** (`/dev/tty1..6`). |
| In-kernel escape parser | **Cut-down** (cursor moves, ED/EL, SGR-8-colour) — the full xterm engine stays in userland `vt.c`. |
| Sequencing | M1b-3 (done) → **M1b-4 folded into M1c-4** → M1c-1..4 → DL → BB. |
| M1b-3's `NEOOS_TIOCSACTIVE` / `fb_owned` / `active_input_tty` | Deleted in M1c-4, replaced by `vt_active` + per-VT `kd_mode`. |
| M1c-1 method | Scripted `git mv` + `sed` include rewrite; script not kept; serial log must be diff-identical. |
| virtio-gpu / real GPU drivers | **Not in M1c.** The point is that `fb_device` leaves room for one; writing one is a separate future milestone. |
| Boot banner | Added to M1c-2 (Task 3, user request): `console_clear()` + a red/purple butterfly-N logo + info panel (`NEOOS_VERSION` + `git describe`, CPU brand string via CPUID `0x80000002-4`, online cores, free/total MiB, CPU feature list), drawn right after `con_driver_select()`. The `con_driver` primitive is `putc_attr(char, uint8_t fg)` (16 colours) so the banner and M1c-3's SGR share one interface. |
