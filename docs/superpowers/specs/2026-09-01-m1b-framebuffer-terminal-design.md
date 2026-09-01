# M1b — framebuffer terminal — design

**Milestone:** M1b of the console / init sequence (M1a → M2 → M1b).
Follows M2 (init as PID 1). Precedes the dynamic-linking and BusyBox
tracks (`docs/superpowers/specs/2026-09-01-busybox-and-dynamic-linking-roadmap.md`).

**Goal:** a scrollable, xterm-ish terminal that lives in **userland**,
rendering to the linear framebuffer with a Spleen 12×24 bitmap font, so
the kernel is out of the business of painting normal output — it keeps
the framebuffer only for the boot log and the panic screen.

**Origin:** brainstormed 2026-08-31 (the "userspace init + real
scrollable terminal" session) and 2026-09-01 (session
`session_01CNR4gEkyMq6qhFWfxt3KXE`). Decisions settled in those
sessions are in the table at the end; this document is the design, not
a re-litigation of them.

---

## 1. Architecture

```
 kernel keyboard IRQ ──► input_key_event ──► evdev /dev/input/event0
                                    │
                                    └──(cooked bytes)──► the ACTIVE pty master
                                                              │
 /SBIN/INIT ──spawns──► /BIN/TERM ◄──── read / write ─────────┘
                          │  (userland VT engine, plain C)
                          │  opens /dev/ptmx → gets /dev/pts/N
                          │  execs a child on pts/N as fd 0/1/2
                          │  mmaps /dev/fb0, blits the cell grid
                          └─ the child (a test program now; the shell
                             after BB) runs on pts/N, fully unaware it
                             is not a real terminal
```

- **`/BIN/TERM`** is a new userland program (`userland/term/`). On
  start it:
  1. opens `/dev/ptmx`, reads its slave index via `TIOCGPTN`, opens
     `/dev/pts/N`;
  2. claims the **active terminal** (§3) via an ioctl on the master;
  3. `mmap`s `/dev/fb0` `MAP_SHARED` and reads geometry with
     `FBIOGET_VSCREENINFO`;
  4. `fork`s; the child sets pts/N as fd 0/1/2, `setsid`, and
     `exec`s (syscall 13 — no argv needed) the target program (a
     build-time-configured path — `/BIN/TERMCHILD.ELF` for the
     milestone's own test). BB6 repoints this at `/BIN/BUSYBOX` and,
     needing argv/env by then, switches to `execve`;
  5. loops: `poll({master_fd, event0_fd}, …)` →
     - master readable: read up to 4 KiB, feed each byte to the VT
       parser, mark dirty cells, blit them;
     - event0 readable: read `struct input_event`s, handle the keys
       the slave's line discipline will not produce (arrows, Home/End,
       PageUp/PageDown, F-keys) — translate to escape sequences and
       `write()` them to the master, or, for `Shift+PageUp/Dn`, scroll
       the local scrollback view;
  6. when the child exits (master read returns 0 / `EIO`), TERM
     restores the console as the active terminal and exits, and init
     (respawn) starts it again.

- **The VT engine is plain C compiled into TERM** (`userland/term/vt.c`,
  `vt.h`). No kernel code, no shared library. It owns the cell grid,
  the scrollback ring, the parser state machine, and the cursor. It
  exposes:
  ```c
  struct vt;
  void vt_init(struct vt *, int cols, int rows);
  void vt_feed(struct vt *, const uint8_t *p, size_t n);   // parse + mutate grid
  void vt_resize(struct vt *, int cols, int rows);
  // dirty-cell iteration for the renderer
  int  vt_take_dirty(struct vt *, struct vt_span *out, int max);
  void vt_scroll_view(struct vt *, int delta_lines);       // scrollback
  ```

- **The renderer** (`userland/term/render.c`) is the only part that
  knows about the framebuffer: given a dirty span `{row, col0, col1}`
  it draws each cell's glyph from the font table into the mapped
  pixels, honouring the cell's SGR attributes (FG/BG colour, bold →
  brighter, underline, reverse).

## 2. Font pipeline

- **Source:** **Spleen 12×24** (`github.com/fcambus/spleen`),
  **BSD-2-Clause**. It ships a *native* 12×24 BDF — the exact cell
  size, no scaling, no rasterizer. (NokiaPure was the brainstorm
  choice but is Nokia-proprietary: shipping a rasterized derivative in
  a public repo is redistribution, so it is out. Spleen is a
  console/terminal font by design and its licence permits
  redistribution with attribution.)
- Vendored under `third_party/spleen/` **with the upstream
  `LICENSE`**; only `spleen-12x24.bdf` is needed.
- **Cell:** 12×24 px. At 1280×800 → **106 columns × 33 rows** (the
  bottom 8 px row is slack).
- **Converter:** `tools/bdf2c.py` — Python standard library only, no
  `freetype`, no `fonttools`. Parses the BDF, packs each glyph
  (U+0020–U+00FF, i.e. Latin-1, which Spleen 12×24 covers, plus the
  box-drawing glyphs Spleen ships) into
  `const uint8_t term_glyphs[256][24 * 2]` (24 rows × 2 bytes = 16
  bits, top 12 used), emitting `userland/term/font_term.c` +
  `font_term.h`. Same shape and provenance rationale as
  `kernel/dev/font8x16.c`.
- The generated `font_term.c` is **checked in** (like `font8x16.c`),
  so a normal `make` needs no Python; the Makefile rule to regenerate
  it is there for when the source or the converter changes.

## 3. Input routing and the "active terminal"

Today `kernel/dev/input.c` cooks decoded keystrokes straight into
`tty_console()`. M1b generalises the destination.

- New kernel concept: **the active terminal** — a single
  `struct tty *active_input_tty`, default `tty_console()`. Cooked
  keyboard bytes (`input_key_event`'s TTY path) go to
  `active_input_tty->…` instead of unconditionally to the console.
- A pty **master** gains an ioctl, `TIOCSCTTY`-adjacent but
  NeoOS-numbered under the existing pty ioctl block
  (`NEOOS_TIOCSACTIVE` — pick a free `_IO` number), meaning "route
  cooked keyboard input to my slave, and hand the framebuffer to
  userland". Its inverse restores `tty_console()` and re-enables
  kernel framebuffer output. TERM calls the former on startup and the
  latter on exit (and the kernel calls the latter itself if the
  master is closed without it — `ptm_close`).
- **`fb_owned_by_userland`** flag (in `dev/fb.c` or `dev/console.c`):
  set by the same ioctl. While set, `console_write` / `console_putc`
  write **serial only** — the kernel does not touch framebuffer
  pixels. Cleared on TERM exit and forcibly by `panic()`.
- **Line-discipline division of labour:** ordinary printable input and
  the basic editing keys (backspace, `^U`, `^W`, `^C`, `^Z`) are
  produced by the keyboard decoder and cooked by the **pts slave's**
  line discipline exactly as the console tty does today — TERM does
  not see them, the shell reads cooked lines. TERM only injects
  sequences for keys the decoder emits as `KEY_*` evdev events but the
  line discipline has no byte for: arrows, `Home`/`End`,
  `PageUp`/`PageDown`, `Insert`/`Delete`, `F1`–`F12`. TERM reads those
  from `/dev/input/event0`.
  - `Shift+PageUp` / `Shift+PageDown` are consumed by TERM for
    scrollback and **not** forwarded.

## 4. VT parser scope (xterm-ish)

Implemented:

- **C0:** `BS CR LF HT BEL` (BEL → brief inverse-video flash), `\e c`
  (RIS full reset).
- **CSI:** `CUU CUD CUF CUB` (`A B C D`), `CUP`/`HVP` (`H`/`f`),
  `ED` (`J`, modes 0/1/2), `EL` (`K`, modes 0/1/2), `IL`/`DL`
  (`L`/`M`), `SU`/`SD` (`S`/`T`), `DECSTBM` (`r`, scroll region),
  `DECTCEM` (`?25h`/`?25l`, cursor visibility), `SM`/`RM` and
  `DECSET`/`DECRST` for the modes below, `SGR` (`m`).
- **SGR:** reset, bold, dim, underline, blink→bright, reverse, hidden;
  FG/BG 30–37 / 40–47, 90–97 / 100–107 (bright), `38;5;n` / `48;5;n`
  (xterm-256, mapped to the nearest of a fixed 256-entry RGB table),
  `39`/`49` default.
- **Private modes:** `?1049` (alt screen — save/restore the primary
  grid and cursor; scrollback frozen while active), `?2004`
  (bracketed paste — accepted, no-op, nothing pastes yet), `?1000`
  &c. (mouse — accepted, no-op).
- Colours resolve through a fixed RGBA palette; the terminal has no
  configurable theme in M1b (one built-in scheme).

Explicitly **not** in M1b: sixel/ReGIS, real mouse reporting, tabs
stops beyond every-8, double-width / CJK, UTF-8 (input and grid are
**Latin-1** — matches the font's coverage), tmux/screen control,
title-setting OSC (`\e]0;…` accepted and discarded).

## 5. Scrollback

- Ring buffer of **1000 lines** × 106 cells, each cell
  `{ uint8_t ch; uint16_t attr; }` — ~1.2 MB in TERM's heap.
- New output always appends at the bottom; when the view is scrolled
  up, new output does **not** move it, but any **input keystroke that
  reaches the child** snaps the view back to the bottom first.
- `Shift+PageUp`/`Shift+PageDown` move the view by `rows/2`.
- A 1-px scroll-position indicator down the right edge while scrolled
  away from the bottom.
- Alt screen: entering `?1049` freezes and hides scrollback; leaving
  it restores the primary grid and the prior scroll position.

## 6. Panic and boot

- `fbcon` stays compiled and is unchanged in role: it renders the
  **boot log** (before TERM exists) and the **panic screen**.
- `panic()` (`kernel/…/panic.c`): first statement clears
  `fb_owned_by_userland` and re-points `active_input_tty` at the
  console, then proceeds to its existing `fbcon`/serial dump. A panic
  always paints, over TERM's grid if necessary — TERM's address space
  is being torn down regardless.
- Normal boot: kernel selftests render through `fbcon` as today until
  init starts TERM; from that point kernel chatter is serial-only.
  The `make test` marker scrape is unaffected (it reads the serial
  log).

## 7. Build and process wiring

- `userland/term/` : `main.c`, `vt.c` / `vt.h`, `render.c` / `render.h`,
  `input.c` / `input.h` (evdev key → escape translation),
  `font_term.c` / `.h` (generated from Spleen, checked in),
  `palette.c` (256-colour table).
- `userland/termchild.c` : the milestone's test child — prints
  coloured text, moves the cursor to known cells, then loops reading
  stdin and echoing (so the interactive key-injection test has
  something to talk to).
- `Makefile`: `TERM.ELF` and `TERMCHILD.ELF` build rules (libneoos +
  crt0, large model), disk copies to `::BIN/TERM.ELF` /
  `::BIN/TERMCHILD.ELF`, the `font_term.c` regeneration rule
  (`tools/bdf2c.py` on `third_party/spleen/spleen-12x24.bdf`),
  `REQUIRED_MARKERS` additions.
- `/ETC/INITTAB`: a `respawn /BIN/TERM.ELF` entry. During the
  milestone TERM's child is `TERMCHILD`; a later BB milestone
  repoints it at the shell. The existing `spawn /BIN/*` test entries
  stay — they still run and exit; TERM runs alongside them.
- `lib/`: M1b needs no new `lib/` wrapper — TERM uses `fork` +
  `exec` (13), both already present. `execve`/`spawnve` arrive with
  BB1/BB6.

## 8. Testing (headless)

- **`vttest`** — `userland/term/vt.c` compiled a second time into a
  standalone `VTTEST.ELF` with `-DVT_SELFTEST` providing `main`: feeds
  the parser fixed byte strings and asserts grid contents, cursor
  position, SGR state, scroll-region behaviour, alt-screen
  save/restore. `[vttest] ALL PASSED`.
- **`[term] render ALL PASSED`** — init spawns `TERM` with
  `TERMCHILD`; `TERMCHILD` prints a known pattern; TERM, after
  rendering, reads back a defined rectangle of its own framebuffer
  mapping and checksums it against a compiled-in expected value, then
  prints the marker. (No new kernel hook needed — TERM owns the
  mapping.)
- **scrollback** — `neoos_test_inject_key` feeds `PageUp` events with
  Shift held; `TERMCHILD` has pre-filled the scrollback; TERM asserts
  the view offset changed and the top visible line is the expected
  one, prints `[term] scrollback ALL PASSED`.
- **input translation** — inject `KEY_UP` / `KEY_LEFT`; `TERMCHILD`
  (in raw mode) asserts it received `\e[A` / `\e[D` on stdin.
- Gauntlet stays green; `pgauntlet.sh` picks the new markers out of
  `REQUIRED_MARKERS` automatically.

## 9. Sub-milestone decomposition

| # | Deliverable | Ends when |
|---|---|---|
| **M1b-1** | Font pipeline: Spleen 12×24 vendored + `LICENSE`, `tools/bdf2c.py` (stdlib only), generated `userland/term/font_term.c/.h` checked in, Makefile regeneration rule. | `tools/bdf2c.py` reproduces the checked-in `font_term.c` byte-for-byte; a tiny host harness renders "Ag" from the table and it matches the BDF. |
| **M1b-2** | VT engine (`vt.c/.h`) + `VTTEST.ELF`. Pure grid/parser logic, zero framebuffer. | `[vttest] ALL PASSED` in `make test`, gauntlet green. |
| **M1b-3** | `TERM` process: PTY alloc + child exec + `/dev/fb0` render loop + `NEOOS_TIOCSACTIVE` ioctl + `active_input_tty` + `fb_owned_by_userland` + `console_write` serial-only handoff + `panic()` override + inittab entry. | `[term] render ALL PASSED`, kernel chatter goes serial-only after TERM starts, panic still paints, gauntlet green ×3 (boot-critical). |
| **M1b-4** | Scrollback ring + `Shift+PageUp/Dn` + evdev raw-key → escape translation + right-edge indicator + `docs/stdlib.md` / `docs/abi-compatibility.md` / `README` refresh. | `[term] scrollback ALL PASSED`, input-translation test green, docs updated. |

M1b-1 and M1b-2 are independent. M1b-3 needs both. M1b-4 needs M1b-3.

## 10. Decisions already settled

| Question | Decision |
|---|---|
| Terminal in kernel or userland | **Userland** process (`/BIN/TERM`). |
| VT scope | **xterm-ish / broad** (§4). |
| Font | **Spleen 12×24** (BSD-2-Clause) — native bitmap, converted to a checked-in `.c` table by `tools/bdf2c.py`. NokiaPure (the brainstorm pick) dropped: proprietary, not redistributable as a rasterized derivative. |
| Cell size | **12×24** → 106×33 at 1280×800. |
| Framebuffer mode | **1280×800×32** (boot.asm hint; fb.c takes what GRUB gives). |
| Scrollback | **Yes**, 1000 lines, `Shift+PageUp/Dn`. |
| Input routing | Kernel **active-terminal** pointer; a pty-master ioctl claims cooked keyboard input + the framebuffer. |
| VT engine form | Plain C compiled into TERM — not a lib, not kernel. |
| Kernel framebuffer after handoff | Serial-only for normal output; `fbcon` retained for boot log + panic; `panic()` forcibly reclaims. |
| Charset | **Latin-1** for M1b (matches the font); UTF-8 is later. |
| Shell's home (cross-milestone) | BB6 points TERM's child at `/BIN/BUSYBOX`. |
