# DOOM in a window

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M10. Changes `neoos-doom` (a windowed backend + a start-menu manifest);
depends on spec 05 (WM protocol v2: states, fullscreen, undecorated)
and spec 06 (manifests).

## What exists (read from `neoos-doom`, 2026-09-23)

- doomgeneric with a NeoOS shim (`neoos-shim/`): `i_video_neoos.c`
  opens `/dev/fb0`, `mmap`s it, and in `DG_DrawFrame` copies the
  320×200 `DG_ScreenBuffer` onto the framebuffer with **integer
  scaling**, centred. `DG_SetWindowTitle` is a no-op ("no window
  manager"). `i_input_neoos.c` reads `/dev/input/event0` (evdev,
  `O_NONBLOCK`). Sound via `/dev/snd/pcmC0D0p`.
- Built against `neoos-musl` (`MUSL_DIR`), default IWAD
  `/opt/doom/doom1.wad` (shareware, bundled).
- It therefore takes over the whole screen and fights the compositor
  for `/dev/fb0` and the keyboard if both run.

## Goals

- Registered in the start menu: category **Games**, name **DOOM**,
  exec `/usr/local/bin/doom.nex`, args `-iwad /opt/doom/doom1.wad`, an
  icon.
- Runs **in its own window** by default, composited by `neoos-wm` like
  any client, with keyboard/mouse focus routed by the WM.
- **Fullscreen toggle** (F11 and a menu option), via the WM's
  fullscreen state — not by grabbing `/dev/fb0`.
- Keeps working **without** a compositor (text-mode / `busybox-doom`
  images): the existing fb0 backend stays and is chosen automatically.

## Non-goals

- Resizable window with arbitrary scaling (integer scales only, v1).
- Mouse look (doomgeneric's mouse support is minimal; keyboard as today).
- Changes to DOOM's engine or WAD handling.

## Decision: DOOM becomes an ordinary WM client (brief's "Option A")

The brief offered: (A) DOOM renders into a shared buffer the WM
composites, or (B) a framebuffer-backed window API. On NeoOS these are
the same thing, because the WM's only window API **is** a shared
memfd buffer per surface (spec 05). Choosing (A) means: no new WM
feature is invented for games. DOOM writes pixels into its surface
buffer, damages, commits — exactly what `wmdemo` and every LVGL app do.
A "framebuffer passthrough" mode (letting one client own `/dev/fb0`
while the WM steps aside) was rejected: it would need the WM to
suspend compositing, release input devices and restore them on crash —
a whole mode-switching protocol for one app, and a crash would leave
the desktop dark.

## Windowed backend

`neoos-shim/i_video_wm.c` (new) next to `i_video_neoos.c`; the backend
is picked at startup:

```
DG_Init:  if wm_connect() succeeds and "-fb" was not given -> WM backend
          else                                            -> fb0 backend (today's code)
```

- Links `libwmclient` built against musl — `neoos-lvgl/wmclient/`
  already carries a musl build of it; with protocol v2 (spec 05) that
  copy is replaced by linking `neoos-wm`'s own `build/libwmclient-musl.a`
  (one source of truth). Build dependency: `WM_DIR` (os-builder passes
  it, roadmap).
- Window: `NORMAL` role, content size **640×400** (2× scale), title
  "DOOM". The WM draws the chrome (spec 05).
- `DG_DrawFrame`: the same integer-scaling copy as the fb0 backend, into
  `wm_surface_pixels()` instead of `fb_mem`; then `wm_surface_damage`
  (whole content) + `wm_surface_commit`. Format XRGB8888 — DOOM's
  palette output is already 0x00RRGGBB.
- `DG_SetWindowTitle` → `wm_surface_set_title` (DOOM sets "DOOM
  Shareware" etc.).
- Scale follows the surface size on `WM_CONFIGURE`: the largest integer
  scale that fits, centred, black borders — so maximize and fullscreen
  work with no extra code.

### Input

- `i_input_wm.c`: keys from `WM_KEY` events instead of
  `/dev/input/event0`. WM keys are **raw evdev codes** (spec 05 notes
  this is the WM's current behaviour), so the existing evdev → DOOM key
  table in `i_input_neoos.c` is reused unchanged — factored into a shared
  function both input backends call.
- Focus: the WM delivers keys only to the focused window; on
  `WM_FOCUS{0}` DOOM releases all held keys (sends key-up for every key
  it believes is down), otherwise a key held while clicking away would
  stay "pressed" forever.
- Mouse: ignored in v1 (as today).

### Fullscreen

- F11 (and Options → "Fullscreen" if cheap to add to the menu) calls
  `wm_surface_request_state(FULLSCREEN)` / back to `NORMAL`. The WM
  applies `WM_STATE_FULLSCREEN` (undecorated, full screen, above the
  taskbar while focused — spec 05) and sends `WM_CONFIGURE` with the
  screen size; DOOM re-attaches a screen-sized buffer and picks the
  largest integer scale.
- `-fullscreen` command-line flag starts in fullscreen.

### Frame pacing

- DOOM runs at 35 tics/s. Today `i_timer_neoos.c` sleeps between tics;
  since the hrtimer milestone `nanosleep`/`clock_nanosleep` are exact to
  microseconds, so pacing uses `clock_nanosleep(CLOCK_MONOTONIC,
  TIMER_ABSTIME, next_tic)` on an absolute schedule (no drift
  accumulation) — the only kind of timed wait here, and it *is* the
  requirement (a game clock), not a synchronisation shortcut.
- There is no vsync signal from the WM. A frame is committed once per
  rendered frame; the compositor coalesces damage. If the compositor
  falls behind (softpipe on TCG), DOOM keeps simulating at 35 Hz and
  frames are simply presented late — no back-pressure in v1. A
  `WM_FRAME_DONE` event (Wayland's frame callback) is noted as future
  work in spec 05's open questions if tearing/latency shows up.
- Budget: a 640×400 XRGB copy per frame is 1 MB/frame, 35 MB/s — cheap.

## Start-menu registration

`neoos-doom` ships `assets/doom.app` (spec 06 manifest format):

```
key=org.neoos.doom
name=DOOM
category=Games
category_key=org.neoos.category.games
exec=/usr/local/bin/doom.nex
args=-iwad /opt/doom/doom1.wad
icon=/usr/share/neoos/icons/doom.png
sort_order=0
```

and `assets/doom-icon.png` (48×48, drawn for NeoOS or derived from the
shareware title screen — the icon must not be id Software's logo
artwork unless licensing is checked; open question). os-builder's port
install copies both into place (`/usr/share/neoos/apps/`,
`/usr/share/neoos/icons/`), which is ordinary file installation it
already does for ports (`doom.test.json` category `bin`; add the asset
paths).

## Sound

Unchanged (`/dev/snd/pcmC0D0p`). Two DOOMs would contend for the PCM
device; out of scope.

## Error handling

- No compositor → fb0 backend (today's behaviour), logged
  `[doom] no compositor, using /dev/fb0`.
- Surface creation or buffer attach fails → log and fall back to fb0
  **only if** no compositor owns the screen (checked by `wm_alive`);
  otherwise exit with an error, never scribble on a composited screen.
- `WM_CLOSE` (× button) → DOOM's normal quit path (`I_Quit`), so config
  is saved.

## Testing

- Target (`make doom-window`): boot wm + shell with the DOOM manifest;
  launch via the start menu (synthetic click) → `[shell] launched
  org.neoos.doom pid=N`, the WM logs the surface, a screenshot after
  the title screen appears shows it inside decorated chrome (compared to
  a golden with tolerance — the demo loop animates, so capture on a
  marker DOOM prints when the title screen is up).
- F11 via synthetic key → `WM_STATE_CHANGED fullscreen` in the log,
  screenshot full-screen; F11 again → back.
- `busybox-doom.yaml` image (no compositor) still boots DOOM on fb0 —
  the existing smoke test must keep passing.

## Decision log

| decision | alternatives | why |
|---|---|---|
| DOOM is a normal WM client (shared buffer composited by the WM) | fb passthrough mode in the WM | no WM mode-switching protocol; a crash cannot leave the screen dark |
| Backend chosen at runtime, fb0 kept | windowed only | non-desktop images still run DOOM |
| Reuse the evdev key table | new key mapping | WM keys are evdev codes already |
| Integer scaling, largest fit | arbitrary scaling | crisp pixels, trivial copy loop |
| Absolute-deadline tic pacing | relative sleeps | exact since the hrtimer milestone; no drift |
