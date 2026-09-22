# Unified desktop shell (`neoos-shell`)

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M4 (shell) and M5 (the start menu inside it — spec 06). Depends on
spec 01 (SQLite), 02 (UI kit) and 05 (WM protocol v2).

## Goals

- **One program, one process, one event loop** owns the whole desktop
  surface: wallpaper, desktop icons, taskbar, start menu and the shell's
  own dialogs. There is no separate executable for any of them.
- Replace today's pair `taskbar.nex` + `startmenu.nex` (neoos-wm), which
  the brief's single-binary rule retires.
- Taskbar shows every running application window (spec 05's shell
  channel) with Windows semantics: click to restore/raise, click the
  focused one to minimize.
- Survive its own crash: restarted automatically, re-adopts every
  window, loses nothing but in-flight UI state.
- Configuration in SQLite (`/var/lib/neoos/desktop.db`, spec 01) with a
  small, documented set of settings.

## Non-goals

- Hosting the compositor. The WM stays its own process (decision log).
- A file manager. Desktop icons are launchers, not a view of a
  directory, in v1.
- Notifications/system tray, multi-monitor, per-user sessions, a lock
  screen, Alt-Tab UI (possible later on the same channel).

## Repository and binary

`NeoOSOrganization/neoos-shell`, C (musl, hosted toolchain), linking
`libnui.a` (spec 02), `liblvgl.a` (neoos-lvgl), `libwmclient` v2 (spec
05), `libsqlite3.a` + `libnsql.a` (spec 01). Output: `build/shell.elf`,
installed as `/usr/local/bin/shell.nex`.

```
neoos-shell/
  README.md, ARCHITECTURE.md
  Makefile                      # UIKIT_DIR LVGL_DIR WM_DIR SQLITE_DIR
  src/main.c                    # startup, event loop, signal handling
  src/wallpaper.c               # background layer
  src/desktop_icons.c           # icon grid on the background layer
  src/taskbar.c                 # panel surface: start button, window buttons, clock
  src/startmenu.c               # popup surface (spec 06)
  src/windows.c                 # model of running windows, fed by the shell channel
  src/launch.c                  # spawn + argument splitting (spec 06 rules)
  src/store.c                   # desktop.db: schema, settings, manifests (spec 06)
  src/power.c                   # shutdown / restart via init (K3)
  assets/                       # default wallpaper, icons (see Assets)
  test/                         # host tests: model, launch parsing, store
```

## Process model

Single-threaded. One `nui_app` (spec 02) whose loop `poll()`s:

1. the WM connection (all surfaces' events and the shell channel), and
   the shell's control socket (below),
2. a `signalfd`-style self-pipe for `SIGCHLD`/`SIGTERM` (reaping launched
   apps without a thread — the old taskbar needed a reaper pthread per
   menu spawn; one `waitpid(-1, WNOHANG)` loop on `SIGCHLD` replaces it),
3. LVGL's next timer deadline (clock minute tick, tooltip timers).

No thread, no fixed-period sleep. The clock redraw is an `lv_timer`
armed for the next minute boundary computed from `CLOCK_REALTIME`, which
the hrtimer milestone made exact.

## Layering and surfaces

The shell owns four kinds of WM surface (spec 05 roles), each with its
own LVGL display via `nui_window_create`:

| layer (bottom→top) | surface | role | content |
|---|---|---|---|
| wallpaper + desktop icons | 1 × screen size | `BACKGROUND` | an `lv_image` wallpaper, icon grid above it on the same display |
| (application windows) | — | `NORMAL` | other processes; chrome drawn by the WM |
| taskbar | 1 × `screen_w × 40` at the bottom | `PANEL`, glass 50% | Start button, window buttons, clock |
| start menu, context menus, dialogs | on demand | `POPUP` | spec 06; right-click menus on desktop/taskbar |

Why wallpaper and icons share one display: they are one layer in the
brief's list and never interleave with windows; two surfaces would only
double the compositing work for a full-screen buffer. The taskbar must
be its own surface because it sits **above** windows while the
background sits **below** them — no single surface can do both. The
brief's "one process, not one surface" is the requirement; this is the
minimum number of surfaces that satisfies z-order.

Window decorations are the compositor's (spec 05), so the shell does
not draw them; it only supplies their tokens (via the UI kit's
generated `wm-theme.conf`) and the theme choice.

## Integration with the WM

On startup:

1. `nui_app_create("shell")` → connects (retrying via `wm_connect`'s
   existing wait-for-compositor loop, which already polls a real
   condition rather than sleeping blindly).
2. `WM_SHELL_BIND`. If `EBUSY`, another shell is running: log and exit 0
   (prevents two shells when init and a human both start one).
3. Create the background surface (`BACKGROUND` role — the WM keeps it at
   the bottom and at the origin by construction), the taskbar panel.
4. The WM sends `WM_WIN_ADDED` for every existing window; `windows.c`
   builds the model, the taskbar renders buttons.
5. `WM_SET_THEME` with the stored theme.

The shell never assumes it was first: everything it knows about
windows comes from the channel, so a restart is just a normal start.

## Control socket

Only one client may hold the WM's shell channel (spec 05), but other
system tools need a view of the windows — Task Manager's Applications
tab (spec 08). The shell therefore listens on an abstract AF_UNIX socket
`neoos-shell` (the same naming scheme as the WM's `neoos-wm`) and serves
a line-based text protocol, serviced by the same `poll` loop:

| request | reply |
|---|---|
| `list-windows` | one line per window: `<surface> <pid> <state> <title>` then `.` |
| `activate <surface>` / `minimize <surface>` / `close <surface>` | `ok` or `err <reason>` |
| `launch <app-key>` | `ok <pid>` or `err <reason>` (start-menu app by manifest key) |

Text, not binary: it is tiny, debuggable with `busybox nc`, and not
performance-sensitive. No authentication, like the WM (single-user OS);
recorded as a divergence.

## Taskbar

- Left: Start button (the existing ring logo asset, moved from
  neoos-wm to neoos-shell; `png2c.py` moves with it).
- Middle: one button per `NORMAL` window (no grouping, spec 05),
  in creation order, showing the window icon (if the client set one;
  otherwise a generic app glyph) and a truncated title with a tooltip
  showing the full title. The focused window's button uses the pressed
  style; minimized windows' buttons are dimmed.
  - Click → spec 05's table (restore / raise / minimize).
  - Right-click → context menu: Restore, Minimize, Maximize, Close,
    **End task** (sends `SIGTERM`, then `SIGKILL` after the user
    confirms — pid from `WM_WIN_ADDED`).
  - Overflow: buttons shrink to a minimum width, then the strip scrolls
    horizontally with arrow buttons.
- Right: clock `HH:MM` (24 h; format setting), tooltip with the date.

## Desktop icons

- A `nui_grid` on the background display, column-major from the top-left,
  96×96 px cells: icon (48×48) + label (two lines max, ellipsised).
- Contents come from `desktop.db` table `desktop_icons (id, app_id →
  apps.id, position)` — an icon is a pin of a start-menu app (spec 06),
  so launching goes through the same code path and the same argument
  rules. "Pin to desktop" is a start-menu context action.
- Double-click launches; single-click selects; drag reorders (positions
  saved). Delete/right-click → "Remove from desktop" (unpins only).
- Keyboard: arrows move selection, Enter launches (the background
  surface takes focus when the user clicks the bare desktop).

## Wallpaper

- Default: a generated gradient in the theme's `bg` colours (no asset
  download; the current flat `0x00202830` fill is its dark variant's
  start colour so today's look is preserved).
- User image: settings key `wallpaper.path` → a PNG/BMP loaded with
  LVGL's built-in decoders (`LV_USE_LODEPNG` / `LV_USE_BMP` — enable in
  `neoos-lvgl`'s `lv_conf.h`), modes `fill | fit | center | tile`.
- Decoded once and scaled once into the surface; never per frame.

## Startup and shutdown lifecycle

- Launched by init as a **`respawn`** entry (init already supports
  `respawn`: "launch, and relaunch whenever it exits"), after `wm.nex`
  in `/etc/inittab` for desktop images:

  ```
  respawn /usr/local/bin/wm.nex
  respawn /usr/local/bin/shell.nex
  ```

  Ordering between them needs no delay: the shell's `wm_connect` waits
  for the compositor's socket (existing behaviour).
- Normal exit paths: `SIGTERM` from init at shutdown (K3) → the shell
  closes its surfaces and exits 0. The power actions it *initiates*
  (spec 06) go the other way: shell → init.
- `make desktop` (NeoOS) switches from `wm + taskbar` to `wm + shell`.

## Crash recovery

- init's `respawn` restarts the shell whenever it exits, including on a
  crash. To avoid a hot crash loop, init gains a **backoff** for respawn
  entries: an entry that exits within 5 s of starting is restarted after
  1 s, then 2 s, 4 s … capped at 60 s, reset after it has run for 60 s
  (an init.c change, part of M4; the delays are between *restarts of a
  failed process*, the one place a timed wait is the actual requirement).
- State that survives a crash: everything in `desktop.db` and everything
  the WM holds (windows, their states, focus). State that is lost: an
  open start menu, a half-typed search — acceptable.
- On restart the channel's `WM_WIN_ADDED` replay rebuilds the taskbar.
  Launched apps are **not** children of the new shell (they were the old
  shell's; init reaps them as orphans) — the shell must not rely on
  being their parent for anything but its own `SIGCHLD` bookkeeping.
- A crash is logged by init (`[init] respawn shell.nex: exited with
  signal 11`) — that line is what tests assert on.

## Configuration

Settings live in `desktop.db` `settings(key, value)` (spec 01 schema),
defaults compiled in, so an empty table means "defaults":

| key | default | meaning |
|---|---|---|
| `theme` | `dark` | `light` / `dark` |
| `wallpaper.path` | empty | image path; empty = generated gradient |
| `wallpaper.mode` | `fill` | `fill`/`fit`/`center`/`tile` |
| `clock.format` | `24h` | `24h`/`12h` |
| `taskbar.glass` | `50` | glass intensity 0–100 |
| `startmenu.show_recent` | `1` | see spec 06 |

Why SQLite rather than a text file for settings: the start menu already
needs the database (brief), the shell is its only writer, and one store
means one backup/restore story (spec 01). A text config would be a
second format for six keys. A future Settings app edits the same table.

## Assets

Default icons (generic app, folder, terminal, notepad, task manager,
games, power, restart) as small PNGs rasterised at build time from SVGs
under `assets/icons/`, converted to C arrays with the `png2c.py` tool
that already exists in neoos-wm — same pipeline as the Start logo, no
runtime SVG. Source SVGs are drawn for NeoOS (simple geometric glyphs)
or taken from a permissively licensed set **already present on the build
host** — nothing is downloaded (restricted bandwidth). Credits recorded
in `assets/CREDITS`.

## Error handling and logging

- Prefix `[shell]`. Every failure path logs and degrades:
  - `desktop.db` unusable → spec 01's recovery (restore backup / fresh
    DB + reseed); if even that fails, the shell runs with compiled-in
    defaults and an empty menu, and says so in a dialog.
  - Launch failure (`spawn` error, missing executable) → a dialog naming
    the path and errno; logged.
  - Lost WM connection → exit 1 (init restarts both in order).
- The shell never exits on a recoverable error; a crash-restart loop is
  treated as a bug and made visible by init's backoff log.

## Testing

- Host: the window model (`windows.c`: sequences of channel events →
  expected taskbar button list and click actions), argument splitting,
  settings defaults.
- Target (`make desktop-shell`, headless, `boot_until.sh` markers):
  1. wm + shell boot; `[shell] ready surfaces=2` marker.
  2. Two `wmdemo` windows appear → `[shell] taskbar buttons=2`.
  3. Synthetic click on a button → the window's state events in the
     log; screenshot shows it restored/minimized.
  4. `kill -9` the shell → init respawns it → `[shell] adopted windows=2`.
  5. Screenshots of the full desktop in light and dark against goldens.

## Assumptions to verify

1. init's `respawn` restarts on *crash* exits (signal-terminated), not
   only on normal exit — read `userland/init.c` in the M4 plan.
2. `SIGCHLD` delivery + `waitpid(-1, WNOHANG)` works in the NeoOS musl
   path (a signalfd is not required; a self-pipe written from the
   handler is enough).
3. LVGL's PNG decoder fits the hosted toolchain build (lodepng is
   bundled in LVGL; no download).

## Risks

| risk | mitigation |
|---|---|
| A background surface the size of the screen costs memory (1920×1080×4 ≈ 8 MB) | acceptable; it is shared with the WM, not copied |
| The shell is a single point of failure for the desktop UI | respawn + channel replay; the WM keeps windows usable without it |
| Two shells racing after a quick restart | `WM_SHELL_BIND` is exclusive; the loser exits |

## Open questions

- Should desktop icons be a view of `/home/<user>/Desktop`-like
  directory (files, not only launchers)? Draft: no for v1.
- Default wallpaper: gradient (draft) or a NeoOS-branded image made from
  the ring logo?

## Decision log

| decision | alternatives | why |
|---|---|---|
| Separate shell process, WM stays separate | shell hosts the WM in-process | a shell crash must not take the compositor (and every window) down; the WM renders via Mesa and must not grow LVGL |
| One process with 3+ surfaces (background, panel, popups) | one full-screen surface | z-order: taskbar above windows, wallpaper below — impossible in one surface |
| Start menu inside the shell | its own executable (today) | brief: no separate processes for desktop pieces; shares the SQLite handle and the model |
| C, not C# | C# NativeAOT like the tools | the shell is on the critical path at boot and after crashes; C starts fastest and has no GC pauses on the taskbar |
| init `respawn` + backoff for crash recovery | a watchdog process; the WM restarting the shell | init already has respawn; no new process |
| Settings in SQLite | INI file | one store, one backup path, one writer |
| Desktop icons are pins of start-menu apps | free-form `.desktop` files on disk | one launch path, one argument rule |
