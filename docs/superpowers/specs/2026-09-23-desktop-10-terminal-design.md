# Terminal emulator

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M9. C# NativeAOT in `neoos-systools/apps/Terminal` (spec 07), rendering
through LVGL, reusing NeoOS's existing C terminal parser.

## What exists (read from the NeoOS tree, 2026-09-23)

- **PTYs**: `kernel/tty/pty.c` — `/dev/ptmx` master, `/dev/pts/N`
  slaves with a full line discipline; `TIOCGPTN`, `TIOCSCTTY` work
  (`userland/term/main.c` uses them). BusyBox `ash` runs interactively
  on them (project memory: BusyBox 1.37, job control, pipes).
- **A pure VT parser**: `userland/term/vt.{c,h}` (560 lines) — "a
  self-contained xterm-ish terminal emulator: cell grid, escape/CSI
  parser, SGR attributes, scroll regions, alt screen, scrollback ring.
  PURE logic — no framebuffer, no syscalls, no I/O." Handles cursor
  movement (CUU/CUD/CUF/CUB/CNL/CPL/CHA/VPA/CUP), ED/EL, IL/DL, SU/SD,
  DECSTBM, save/restore cursor, SGR, DSR, RIS, IND/RI/NEL, OSC.
  Limits: **Latin-1 cells** (`uint8_t ch`), 200×64 max, 1000 lines of
  scrollback, fixed at compile time.
- `/bin/term.nex`: the full-screen framebuffer terminal built on it.

The brief says "reuse existing code where possible": the parser is
exactly that, and it is already the one BusyBox has been tested against.

## Goals

- A windowed terminal: a PTY per session running `/bin/sh` (BusyBox
  `ash`), rendered with LVGL in a UI-kit window.
- The existing `vt` parser, extracted into a library (`libvt`) and
  **extended** rather than rewritten: UTF-8 cells, runtime sizes,
  configurable scrollback.
- **Tabs** (several sessions in one window), **copy/paste** through the
  WM clipboard (spec 05), **scrollback** with mouse wheel and
  Shift+PgUp/PgDn, window resize → `TIOCSWINSZ` + `SIGWINCH`.

## Non-goals (v1)

- Split panes, true colour (24-bit SGR is parsed and mapped to the
  nearest xterm-256 colour), ligatures, image protocols (sixel, kitty),
  mouse reporting to applications (xterm 1000/1006 modes — parsed and
  ignored in v1; the scope table lists it for v2), bracketed paste
  (easy; v1.1).

## `libvt`: extraction and extension (NeoOS repo)

- Move `userland/term/vt.{c,h}` to `userland/libvt/` built as
  `libvt.a` (both for `term.nex` and, via the hosted toolchain, for the
  terminal app). `term.nex` keeps working unchanged — it is the
  regression test for the extraction.
- Changes, each kept small and host-tested:
  1. `struct vt_cell.ch` becomes `uint32_t` (a Unicode scalar), fed by a
     UTF-8 decoder in `vt_feed` (incomplete sequences held across calls;
     invalid bytes → U+FFFD). `term.nex` renders code points > 0xFF as
     `?` (its font is Latin-1) — unchanged behaviour for ASCII.
  2. `vt_create(cols, rows, scrollback_lines)` allocating the grid and
     ring at runtime instead of `VT_MAX_*` compile-time arrays (the old
     constants remain as `term.nex`'s arguments).
  3. `vt_resize(v, cols, rows)` with reflow-free resize (truncate/extend
     lines, keep the cursor inside) — what xterm does without reflow.
  4. Wide characters (East Asian Wide) occupy two cells — only if the
     font gains wide glyphs; stubbed to one cell in v1.
  5. **Persian**: cells stay in *logical* order (as every terminal
     emulator and the programs writing to it assume); the renderer shapes
     each run of Arabic-script cells with LVGL's presentation-form shaper
     (joining across adjacent cells) and draws that run right-to-left
     within its cells — the "implicit BiDi per line" approach of mlterm /
     Konsole's BiDi mode, which makes Persian file names and `echo`
     output readable. Full terminal BiDi (ECMA-48 BiDi modes, cursor in
     visual order) is not attempted — no terminal does it consistently.
     Typing uses the UI kit's `fa` layout (Alt+Shift).
- A C API header `vt.h` is what the C# `NeoOS.Vt` binding wraps (spec 07).

## ANSI/VT scope for v1

| class | supported | notes |
|---|---|---|
| C0 controls | BEL (visual flash), BS, HT (8-column stops), LF, CR, ESC | existing |
| CSI cursor / erase / insert-delete | everything `vt.c` has today + ICH/DCH/ECH (`@`, `P`, `X`) | the three additions are what `vi`, `less` and BusyBox's line editor need; confirm against real output in M9 |
| SGR | bold, dim, underline, reverse, hidden, 8/16/256 colours, 24-bit → nearest 256 | existing + truecolour mapping |
| DEC private modes | `?25` cursor visible, `?1049`/`?47` alt screen, `?7` autowrap, `?1` application cursor keys | `?1` added (arrow key encoding) |
| OSC | 0/2 window title → `nui_window_set_title` (tab title) | existing parser, new consumer |
| DSR/DA | DSR 6 (cursor position), DA1 reply `ESC[?1;2c` | CPR exists |
| mouse modes | parsed, ignored | v2 |

`TERM=xterm-256color` is exported to the child only if the image ships a
matching terminfo entry for programs that read one; otherwise
`TERM=vt100`-compatible `linux`. **Assumption to verify**: what BusyBox
applets on NeoOS read (BusyBox mostly hard-codes ANSI).

## Architecture (C#)

```
Terminal window (nui_window + nui_tabs)
  └─ Session (one per tab)
       ├─ Pty: master fd from /dev/ptmx, child = spawn("/bin/sh") on the slave
       ├─ Vt: libvt instance (P/Invoke), fed from the master fd
       └─ TerminalView: an LVGL canvas-like custom draw object
```

- **PTY + child** (`NeoOS.Native`): open `/dev/ptmx`, `TIOCGPTN` →
  `/dev/pts/N`, spawn the shell with the slave as stdin/out/err, new
  session and controlling tty (`setsid` + `TIOCSCTTY`, exactly what
  `term.nex` does — its `main.c` is the reference implementation).
  **Assumption to verify**: that NeoOS `spawn` can set the child's
  fds/session (else a tiny C helper `pty_spawn()` in `libvt`'s repo
  does the fork/exec part, reused by both terminals).
- **Event loop**: the master fd is registered with
  `nui_app_watch_fd` (spec 02), so output is read on the UI thread when
  it arrives — no reader thread, no polling timer. Reads up to 64 KiB
  per wake, `vt_feed`, then invalidate only the dirty spans
  (`vt_take_dirty`).
- **Rendering**: a custom LVGL object with a draw callback that paints
  dirty rows only: background runs, then glyphs from the UI kit's
  monospace font (Spleen 12×24 as an LVGL font), cursor as a block/
  underline, selection highlight. Palette = xterm-256, with the 16 base
  colours taken from UI-kit tokens (light/dark).
- **Input**: WM key events (evdev codes + modifiers tracked by the UI
  kit) → byte sequences written to the master: printable characters as
  UTF-8 (US layout in v1 — keyboard layouts are an open question
  system-wide), Enter `\r`, Backspace `\x7f`, Tab, arrows (`ESC[A` or
  `ESC OA` in application-cursor mode), Home/End/PgUp/PgDn/Delete/
  Insert/F1–F12 (xterm encodings), Ctrl+letter → control codes, Alt
  → ESC prefix.
- **Scrollback**: 10 000 lines (a setting), viewed with the mouse wheel
  and Shift+PgUp/PgDn; new output snaps back to the bottom.
- **Selection / copy / paste**: drag selects cells (word on double-click,
  line on triple-click); **Ctrl+Shift+C** copies (as text, trailing
  blanks trimmed) via `WM_CLIP_SET`; **Ctrl+Shift+V** pastes via
  `WM_CLIP_GET` (spec 05). Plain Ctrl+C stays SIGINT for the shell.
- **Resize**: `WM_STATE_CHANGED`/configure → new cols/rows from the
  font metrics → `vt_resize` + `TIOCSWINSZ` on the master (the line
  discipline delivers `SIGWINCH` to the foreground group — **assumption
  to verify** that NeoOS's pty implements `TIOCSWINSZ` + `SIGWINCH`).
- **Tabs**: Ctrl+Shift+T new, Ctrl+Shift+W close (confirm if a
  foreground job other than the shell is running — detected via
  `TIOCGPGRP` ≠ the shell's pgid), Ctrl+PgUp/PgDn switch; a tab closes
  when its shell exits; the window closes with its last tab.

## Shell to launch

`/bin/sh` (BusyBox `ash`, already on desktop images). Configurable via
a `settings` row in the terminal's own `/var/lib/neoos/terminal.db`
(`shell`, `scrollback`, `font_size`) — tiny, but it keeps the "one DB per
app" rule of spec 01 and avoids inventing a config file format. Login
shells: started with `argv[0] = "-sh"` so `ash` reads `/etc/profile`.

## Error handling

- `/dev/ptmx` open failure or spawn failure: the tab shows the error in
  red text and a "Retry" button; logged.
- The child exits: the tab prints `[process exited with status N]` and
  closes after a keypress (so a crashing command's last output stays
  readable).
- Master read `EIO` (slave closed): treated as child exit.

## Testing

- Host (the biggest win of reusing a pure parser): `libvt` tests —
  golden-output tests that feed recorded byte streams (BusyBox `ls
  --color`, `vi` start-up, `top` frame) and compare the cell grid; UTF-8
  decoder edge cases; resize.
- `term.nex` keeps passing its existing tests after the extraction.
- Target (`make terminal`): boot wm + shell + Terminal; synthetic keys
  type `echo hi; exit 3`; serial marker from the app
  `[terminal] session exited status=3`; screenshot shows `hi`; a second
  scenario runs `vi` and quits, checking the alt screen is restored.

## Decision log

| decision | alternatives | why |
|---|---|---|
| Reuse `userland/term/vt.c` as `libvt` | write a parser in C#; port a third-party one (libvterm) | already debugged against BusyBox; pure; host-testable; no download |
| UTF-8 cells added to the C parser | keep Latin-1 | a desktop terminal must show UTF-8 file names |
| PTY fd on the UI loop (`nui_app_watch_fd`) | reader thread | no cross-thread LVGL calls, no locks; the loop already polls |
| Ctrl+Shift+C/V | Ctrl+C/V | Ctrl+C must stay SIGINT |
| Per-app `terminal.db` for 3 settings | config file; `desktop.db` | spec 01's one-writer rule, one format everywhere |
