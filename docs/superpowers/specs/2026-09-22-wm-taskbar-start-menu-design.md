# Taskbar and start menu in neoos-wm

## Context

This is sub-project 2 of the "Windows-inspired desktop environment"
roadmap (sub-project 1, the Xcursor-themed pointer, shipped -- see
`docs/superpowers/specs/2026-09-22-wm-cursor-theme-design.md`). It also
extends the already-shipped Liquid-Glass compositing
(`docs/superpowers/specs/2026-09-20-wm-glass-lensing-design.md`) with
one general capability -- per-surface glass intensity -- that this
work needs and any future glass surface can use.

Scope, refined from the original 4-stage roadmap sketch down to what
was actually asked for this round: a taskbar (Start button with the
supplied ring logo, a clock, liquid glass) and a start menu that
really opens and closes (glass, empty content this round). No
wallpaper image, no window chrome (draggable title bars/close buttons
-- still sub-project 3), no taskbar entries for running windows --
still sub-project 4.

## Goals

- A taskbar client, docked at the bottom edge of the screen, full
  width, rendered as Liquid Glass, showing:
  - A Start button on the left: the supplied ring logo
    (`/home/neo/Desktop/ring/neoos-start-neon-ring.svg`), rasterized at
    build time.
  - A live clock, right-aligned, 24-hour `HH:MM`, updating once a
    minute is sufficient but implemented as a once-a-second redraw
    (simplest correct thing; see Non-goals).
- Clicking Start opens an empty glass panel (the start menu), docked
  above the taskbar's left edge.
- The start menu closes when Start is clicked again, **or** when the
  user clicks anywhere else (another surface or the bare desktop) --
  both are the same underlying "this surface lost focus" event from
  the menu's own point of view.
- Both the taskbar and the start menu request a **50% glass
  intensity**: mostly see-through with a light frost, not the strong
  refraction/tint the existing default (100%) produces -- so content
  behind them stays legible, per your framing that most real UI wants
  a light frost, not heavy distortion.
- Glass intensity becomes a real, general, per-surface parameter any
  client can set -- not hardcoded to these two surfaces.

## Non-goals

- Start menu **content**. It opens as an empty glass panel this round,
  by explicit request ("it is empty for now"). Populating it is a
  later milestone.
- Work-area reservation. A normal window may still be positioned so it
  visually overlaps the taskbar's screen region -- nothing reserves
  that strip as off-limits to `place_window()`'s cascade. Documented
  limitation, not solved here (matches this project's convention of
  naming known gaps rather than silently scoping around them -- see
  the glass-lensing spec's z-order limitation for precedent).
- A wallpaper image. The desktop background stays the existing flat
  fill color (`0x00202830`).
- Taskbar entries for running windows, a system tray, notifications,
  keyboard shortcuts (Win key), multi-monitor. All later (sub-project
  4 or beyond).
- Second-precision clock accuracy guarantees. `clock_gettime`'s
  documented resolution is 10ms via the 100Hz LAPIC tick
  (`docs/stdlib.md`'s DIVERGENCE note) -- a once-a-second redraw is
  correct at the "minute" granularity actually displayed; no attempt
  to be more precise than that.
- Any change to the SPIR-V shader or `neoos-tinygl`'s VM. Intensity is
  implemented entirely as a compositor-side post-blend (see Design
  section 3) -- deliberately, to avoid re-opening the shader/uniform
  question sub-project 2 explicitly deferred.

## Design

### 1. Protocol: fixed-position surfaces and glass intensity

`wmproto.h`'s `wm_create_surface` gains two new fields and one new
flag bit:

```c
#define WM_SURFACE_NORMAL     0
#define WM_SURFACE_SHELL      1
#define WM_SURFACE_GLASS      2
#define WM_SURFACE_FIXED_POS  4   // honor x,y below instead of cascading

struct wm_create_surface {
    uint32_t width, height, flags;
    int32_t  x, y;              // only consulted when WM_SURFACE_FIXED_POS is set
    uint8_t  glass_intensity;   // 0-100; only consulted when WM_SURFACE_GLASS is set. Default 100 (today's existing full effect) when a caller does not set it explicitly.
};
```

21 bytes plus ABI padding to 24 -- well under `WM_MAX_BODY` (64), no
wire-format constant changes needed. Both sides of the wire are
compiled by the same toolchain family (x86_64, SysV ABI) exactly as
every other struct in this protocol already relies on, so no explicit
packing is needed; this is not a new assumption.

`wm.c`'s `WM_CREATE_SURFACE` handler: when `WM_SURFACE_FIXED_POS` is
set, `c->x = cs->x; c->y = cs->y;` instead of calling `place_window()`
(shell placement, `c->x=c->y=0`, is unaffected -- FIXED_POS and SHELL
are orthogonal bits, though this milestone's two surfaces use FIXED_POS
without SHELL). `c->glass_intensity` (new field on `struct client`,
default 100) is set from `cs->glass_intensity` when `is_glass`.

`wmclient.h` gains one more general entry point alongside the existing
three convenience wrappers (which keep their current signatures and
now simply pass `x=0,y=0,flags` without `FIXED_POS`, `glass_intensity=100`):

```c
// Full control: explicit position (only honored with flags &
// WM_SURFACE_FIXED_POS) and glass intensity (only meaningful with
// flags & WM_SURFACE_GLASS). Existing wm_create_window/
// wm_create_glass_window/wm_create_shell become thin wrappers over
// this with x=y=0, glass_intensity=100.
int32_t wm_create_surface_ex(struct wm_conn *c, int32_t x, int32_t y,
                              uint32_t w, uint32_t h, uint32_t flags,
                              uint8_t glass_intensity, const char *title);
```

### 2. Real `WM_FOCUS` delivery (general compositor fix, not menu-specific)

`WM_FOCUS` (message type 132) is already defined in `wmproto.h` and
`wmclient.c` already decodes it in `wm_poll_event` -- but `wm.c` never
sends it. This is what makes "click Start again, or click anywhere
else" work for free from the menu's side, and is a real, previously
latent gap independent of this feature.

Factor the existing ad hoc focus-change code (today: only in
`pump_mouse`'s button-down handler, and only fires when `hit >= 0`) into
one helper:

```c
// Transitions focus_slot to new_slot (-1 = nothing focused, e.g. a
// click on bare desktop). Notifies both the surface losing focus and
// the one gaining it via WM_FOCUS -- the only place either message is
// ever sent. No-op if new_slot == focus_slot already.
static void set_focus(int new_slot) {
    if (new_slot == focus_slot) { return; }
    if (focus_slot >= 0 && clients[focus_slot].fd >= 0) {
        damage_window(&clients[focus_slot]);
        struct wm_focus f = { 0 };
        send_to(&clients[focus_slot], WM_FOCUS, &f, sizeof f);
    }
    focus_slot = new_slot;
    if (focus_slot >= 0) {
        damage_window(&clients[focus_slot]);
        struct wm_focus f = { 1 };
        send_to(&clients[focus_slot], WM_FOCUS, &f, sizeof f);
    }
    screen_dirty = 1;
}
```

Two call sites:
1. `pump_mouse`'s button-down handler: `set_focus(hit)` unconditionally
   on a left-button press (`hit` may be `-1` for bare desktop -- today's
   code skips notification entirely in that case; this is the fix that
   makes clicking empty desktop close an open start menu).
2. `WM_CREATE_SURFACE`, for non-shell surfaces only: `set_focus(slot)`
   once the surface is fully set up -- a newly created surface takes
   focus immediately, matching ordinary desktop expectations for a
   popup. (Shell surfaces, if any exist in the future, never take
   focus via this path -- they are background, not interactive chrome.
   No shell surface exists in this milestone's design at all.)

### 3. Glass intensity: a compositor-side post-blend, no shader changes

`draw_glass` already computes two things at the same screen rect: the
pre-shade `snap` (the raw captured backdrop, RGBA8) and, after TinyGL
draws the shaded quad, the post-shade result sitting in `back`. Add one
more pass immediately after the existing `tglUseSpvProgram(NULL, NULL)`
line, before `free(snap)`:

```c
// Blend the just-drawn shaded output back toward the original
// unshaded backdrop by (100 - intensity)%. intensity=100 (the
// existing default) is a full no-op pass-through: shaded stays
// shaded. intensity=0 would fully undo the shader's effect. No
// shader/VM changes -- both operands already exist in this function.
if (c->glass_intensity < 100) {
    for (int row = 0; row < h; row++) {
        uint32_t *dst = back + (uint64_t)(y + row) * fb_w + x;
        const unsigned char *orig = snap + (uint64_t)row * w * 4;
        for (int col = 0; col < w; col++) {
            uint32_t shaded = dst[col];
            uint32_t sr = (shaded >> 16) & 0xff, sg = (shaded >> 8) & 0xff, sb = shaded & 0xff;
            uint32_t or_ = orig[col * 4 + 0], og = orig[col * 4 + 1], ob = orig[col * 4 + 2];
            uint32_t r = (sr * c->glass_intensity + or_ * (100 - c->glass_intensity)) / 100;
            uint32_t g = (sg * c->glass_intensity + og * (100 - c->glass_intensity)) / 100;
            uint32_t b = (sb * c->glass_intensity + ob * (100 - c->glass_intensity)) / 100;
            dst[col] = (r << 16) | (g << 8) | b;
        }
    }
}
```

(`snap`'s RGBA byte order is R,G,B,A per the existing capture loop a
few lines above -- `dst[col*4+0]=R` etc. -- confirmed by reading that
loop, not assumed.)

### 3b. Per-pixel content alpha for glass surfaces (discovered during planning -- a real gap, not a nuance)

**This corrects an implicit assumption in section 3 and in the
original taskbar/start-menu goals.** `draw_window`'s content blit
(the step that draws a surface's own pixels on top of whatever
`draw_glass` just shaded) is today an unconditional opaque copy:
`dst[col] = src[col]` for every pixel of the surface's rect, no
exceptions. For a **chromeless** surface (`fixed_pos`, section 1 --
the taskbar and start menu both are), the content rect is now the
*exact same rect* `draw_glass` just shaded (section 3's whole premise
depends on this exactness). The unavoidable consequence: the client's
own content blit completely overwrites 100% of the shaded backdrop,
every time, regardless of `glass_intensity` -- intensity controls how
strongly the shaded area looks *before* it gets fully hidden again.
"Mostly see-through, opaque only where there is an actual icon or
line of text" -- what you asked for -- is not achievable with
`glass_intensity` alone. It needs real per-pixel transparency in the
content itself, which the protocol has no way to express today
(`WM_FORMAT_XRGB8888` is the only defined pixel format, and it is
opaque by construction).

The fix reuses an extensibility point that already exists in the wire
protocol (`wm_attach_buffer.format`) rather than inventing a new one:

```c
#define WM_FORMAT_XRGB8888 1   // existing -- opaque, top byte ignored
#define WM_FORMAT_ARGB8888 2   // new -- top byte is real alpha, straight (non-premultiplied)
```

- `struct client` gains `uint32_t format`, set from `wm_attach_buffer.format`
  in the `WM_ATTACH_BUFFER` handler (which currently rejects anything
  but `WM_FORMAT_XRGB8888` outright -- now accepts both).
- `draw_window`'s content-blit loop: when `c->format == WM_FORMAT_ARGB8888`,
  alpha-blend each pixel (`dst = src.rgb*src.a/255 + dst.rgb*(255-src.a)/255`,
  straight alpha, source pixel already whatever `draw_glass` +
  section 3's intensity blend left in `back`) instead of the straight
  copy. `WM_FORMAT_XRGB8888` surfaces are completely unaffected --
  same unconditional opaque copy as today, zero behavior change.
- `wm_create_surface_ex` (section 1) requests `WM_FORMAT_ARGB8888`
  automatically whenever `WM_SURFACE_GLASS` is set, `WM_FORMAT_XRGB8888`
  otherwise -- alpha compositing is only meaningful for glass surfaces,
  so this needs no new flag bit; a client never chooses the format
  directly.
- **Regression fix required for the existing glass demo**: `wmdemo.c`'s
  glass-mode fill (`px[y*stride+x] = 0x00FFFFFF`) relied on the top
  byte being ignored under `WM_FORMAT_XRGB8888`. Under the new
  automatic `WM_FORMAT_ARGB8888` for glass surfaces, that same value
  means alpha=0 (fully transparent) -- the demo's content would
  silently vanish. Must become `0xFFFFFFFF` (alpha=255, opaque) to
  keep the `wm-glass` target's existing visual output unchanged.
- **Taskbar/start-menu content**: fill the surface buffer's alpha
  channel to 0 (fully transparent) everywhere by default -- the
  frosted/lensed backdrop shows through the entire panel unless a
  pixel is explicitly drawn opaque. The ring logo (already has its own
  real alpha from the source PNG -- section 6) and the clock glyphs
  (opaque ink pixels, alpha=255, transparent everywhere else in each
  glyph's cell) are the only non-transparent pixels. This is what
  actually produces "mostly bare-visible background, opaque UI
  elements on top."

Still no shader or `neoos-tinygl` VM change -- this is a compositor-
side content-blit change only, same boundary section 3 already drew.

### 4. Taskbar client (`taskbar.nex`)

New file in `neoos-wm`, **libneoos-linked** (like `wmdemo.c` -- crt0.o
+ `libneoos.a`, not musl). This is a correction from an earlier draft
of this spec, which assumed musl was needed for `clock_gettime`/
`gmtime`/`snprintf`: `clock_gettime` is a thin direct syscall
passthrough in `neoos-libneoos/src/syscall.c`, identical to what
musl's shim ultimately reaches, and `docs/stdlib.md`'s own divergence
note confirms `CLOCK_REALTIME` is genuinely RTC-anchored wall time at
the kernel level (the comment in `neoos-libneoos/include/time.h`
claiming a boot epoch is stale documentation, not current behavior --
verified against the actual syscall wrapper, not trusted from the
comment alone). And since the clock is drawn via bitmap-glyph lookup
(section 5), not `printf`-style formatting, no `gmtime`/`snprintf` is
needed either -- just `tv_sec % 86400` arithmetic to extract hour/
minute digits directly. `pthread_create` (needed below) is already
proven working on this exact plain libneoos+crt0.o toolchain, not just
musl -- `userland/mmstress.c` uses it today. `taskbar.nex` does **not**
link `neoos-tinygl` either way, since glass shading is entirely the
compositor's job -- the client only ever draws into its own buffer.

- One surface: `wm_create_surface_ex(c, 0, screen_h - 40, screen_w, 40, WM_SURFACE_GLASS | WM_SURFACE_FIXED_POS, 50, "taskbar")`
  -- per section 3b this transparently gets an `ARGB8888` buffer, not
  `XRGB8888`.
- Every redraw: first clear the whole buffer to alpha 0 (fully
  transparent -- `memset` the buffer to 0 is exactly this, since
  alpha is the top byte of each `0xAARRGGBB` word and RGB does not
  matter when alpha is 0), then draw, opaque, on top of that:
  - The ring logo (see section 6) as the Start button, left-aligned
    with a small margin, vertically centered in the 40px bar --
    alpha-composited using the logo PNG's own real per-pixel alpha
    (straight, not premultiplied -- section 6).
  - The clock, right-aligned, using the existing 12x24 bitmap font
    (`userland/term/font_term.c`'s `term_glyphs`, reused as-is -- see
    section 5): `clock_gettime(CLOCK_REALTIME, &ts)`, then
    `total = ts.tv_sec % 86400; hh = total/3600; mm = (total/60)%60;`,
    each of the 4 digits (plus the `:` glyph) looked up in
    `term_glyphs` directly by character code -- no string formatting
    anywhere in this path. Each glyph's "ink" pixels are drawn fully
    opaque (alpha=255); every other pixel in the glyph's cell stays
    transparent (untouched from the initial clear).
  - Everywhere else in the 40px bar -- the vast majority of its area
    -- stays fully transparent, so the frosted, lensed backdrop
    (section 3) is what is actually seen there. This is the mechanism
    behind "mostly bare-visible background, opaque only where there
    is real UI."
- Main loop: `poll()`s the wm connection's fd with a 1-second timeout
  (matching the redraw cadence the clock needs), drains
  `wm_poll_event` each wake, redraws+recommits when the minute value
  changes, and on `WM_POINTER_BUTTON` checks whether the click's last-
  known surface-relative position (tracked from the preceding
  `WM_POINTER_MOTION`) falls inside the Start button's rect.
- On a Start click: if `menu_open` is false, `spawn("/usr/local/bin/startmenu.nex")`,
  record the child pid, set `menu_open = true`, and spawn a reaper
  pthread (`pthread_create`) that calls the blocking `wait(child_pid)`
  and, on return, sets `menu_open = false`. This is what lets a second
  Start click -- after the menu already self-closed from a focus-loss
  event -- open a fresh one instead of doing nothing; without reaping
  via `wait`, the child would also sit as an unreaped zombie (a known
  problem class in this project -- see the TLB deferred-free
  investigation memory). If `menu_open` is already true, the click
  still lands on the taskbar (which is a different surface than the
  menu), so `set_focus` already closes the existing menu on the
  compositor side -- the taskbar does not need to also explicitly kill
  it.

### 5. Bitmap font reuse

`userland/term/font_term.c`/`.h` (`third_party/spleen/spleen-12x24.bdf`,
generated by `tools/bdf2c.py`) is reused verbatim: `term_glyphs[256][48]`,
1bpp rows packed 2 bytes each, MSB-first
(`(g[gy*2 + (gx>>3)] >> (7 - (gx&7))) & 1`, the exact extraction
`userland/term/render.c` already uses). `taskbar.nex`'s Makefile rule
compiles `font_term.c` directly alongside its own source -- it is
already a small, dependency-free translation unit (just a byte table),
not something that needs its own library target.

### 6. Ring logo asset pipeline

Source: `/home/neo/Desktop/ring/neoos-start-neon-ring.svg` (100x100
viewBox, gradient/glow SVG). Rasterized at build time only -- consistent
with this project's "no SVG tooling on-target" convention (see the
cursor-theme spec's Non-goals) -- to a 28x28 RGBA8 PNG via `inkscape
--export-type=png --export-width=28 --export-height=28`, then a small
host-only Python script (using PIL, already relied on by
`tools/screenshot.sh`) converts the PNG to an embedded C array:

```c
// generated by neoos-wm/tools/png2c.py -- do not edit
#define LOGO_W 28
#define LOGO_H 28
extern const uint8_t neoos_logo_rgba[LOGO_W * LOGO_H * 4]; // straight (non-premultiplied) RGBA8
```

`taskbar.c` alpha-blends this (straight alpha this time, not
premultiplied -- PIL's PNG export is straight alpha; confirm during
implementation, do not assume the cursor theme's premultiplied
convention carries over) into its own buffer once at the Start
button's fixed screen position, no per-frame re-rasterization.

Asset committed alongside the source SVG (also copy the SVG itself into
`neoos-wm/assets/` for provenance, matching how `gm_cursors` carries
its own source-of-truth alongside the compiled artifact) with a
`CREDITS`-equivalent note of its origin (user-supplied, not a
third-party asset needing attribution -- state this explicitly so a
future reader does not go looking for a license that does not apply).

### 7. Start menu client (`startmenu.nex`)

New file in `neoos-wm`, libneoos-linked (like `wmdemo.c` -- no
`clock_gettime`/text rendering needed for an empty panel).

- One surface: `wm_create_surface_ex(c, 0, screen_h - 40 - 420, 320, 420, WM_SURFACE_GLASS | WM_SURFACE_FIXED_POS, 50, "start-menu")`
  (320x420, docked bottom-left, directly above the taskbar's 40px
  strip).
- Fills its buffer entirely with alpha 0 (fully transparent -- a
  `memset` to 0, same reasoning as the taskbar in section 4) and
  commits that. Content is out of scope this round, but something
  still has to be committed so the surface is `mapped` at all (an
  uncommitted surface never gets composited -- `draw_window` and
  `draw_glass` both gate on `c->mapped`). Being fully transparent
  everywhere is not "no content" in a way that breaks anything -- it
  is simply the correct empty state: a frosted 320x420 panel with
  nothing drawn on it yet, exactly matching "empty for now."
- Main loop: blocks in `wm_poll_event`-driven wait, and the instant a
  `WM_FOCUS{focused=0}` event arrives, calls `wm_disconnect` and
  `exit(0)`. No other logic. This is the entire "closes on outside
  click" mechanism -- no signal from the taskbar required.

## Cross-repo scope

Implementation touches only **neoos-wm**:
- `wmproto.h` (`WM_SURFACE_FIXED_POS`, `WM_FORMAT_ARGB8888`,
  `wm_create_surface`'s new fields)
- `wm.c` (`set_focus` helper, `WM_CREATE_SURFACE` handling, `WM_ATTACH_BUFFER`
  accepting `WM_FORMAT_ARGB8888`, `draw_window`'s content-blit alpha
  path, `draw_glass` intensity blend, `struct client.glass_intensity`/
  `.fixed_pos`/`.format`)
- `wmclient.h`/`.c` (`wm_create_surface_ex`, existing wrappers
  refactored onto it, automatic `WM_FORMAT_ARGB8888` request when
  `WM_SURFACE_GLASS` is set)
- `wmdemo.c` (one-line fix: glass-mode fill `0x00FFFFFF` ->
  `0xFFFFFFFF` so it stays opaque under the new automatic
  `WM_FORMAT_ARGB8888` -- see section 3b)
- `taskbar.c` (new), `startmenu.c` (new)
- `assets/logo/` (new: the SVG, generated PNG, generated header)
- `tools/png2c.py` (new, host-only)
- `Makefile` (new build rules for `taskbar.nex`, `startmenu.nex`, the
  SVG->PNG->header asset pipeline)

**NeoOS**: `Makefile` gains install steps for `taskbar.nex` and
`startmenu.nex` at `/usr/local/bin/` (same conditional-on-build-existing
pattern as `wm.nex`/the cursor asset), and a new test target (see
Testing) -- following the cursor-theme milestone's precedent that
disk-image assembly is NeoOS's job, not neoos-wm's.

**docs/stdlib.md**: update the window-system section to document
`WM_SURFACE_FIXED_POS`, the new `wm_create_surface` fields, and
`wm_create_surface_ex` -- this is a NeoOS-native protocol extension
with no POSIX analogue, so per CLAUDE.md's stdlib-doc requirement it
needs a real entry, not just inline comments.

## Error handling

- `spawn("/usr/local/bin/startmenu.nex")` failing (binary missing,
  process-table exhaustion): log and leave `menu_open` false -- Start
  simply does nothing that click, not a taskbar crash.
- `startmenu.nex` failing to connect to the compositor (e.g. `wm.nex`
  gone): exits immediately, same as any other client's existing
  connect-failure handling elsewhere in this codebase (`wmdemo.c`'s
  precedent).
- A `glass_intensity` value out of the documented 0-100 range: treat
  anything `> 100` as `100` (clamp, do not reject the surface) --
  matches this project's general preference for graceful degradation
  over hard failure on a cosmetic parameter.
- Logo asset missing/corrupt at build time: build failure, loud and
  immediate (this is a build-time asset baked into the binary, not a
  runtime-loaded one like the cursor theme -- there is no meaningful
  "fallback" for a Start button with no icon at build time the way
  there was for a swappable cursor).
- `WM_ATTACH_BUFFER` with a `format` that is neither `WM_FORMAT_XRGB8888`
  nor `WM_FORMAT_ARGB8888`: rejected exactly as an unrecognized format
  is today (`close(passed_fd); break;`) -- widening the accepted set
  by one value does not change the "reject anything else" rule.

## Testing

Same headless-QEMU + serial-log convention as every other milestone in
this repo, plus a host-native piece where one exists:

- **`draw_glass` intensity blend**: no host-native test exists for
  `wm.c` (it is target-only, freestanding -- matches the cursor-theme
  and glass-lensing precedent of testing compositor logic only via
  target boot). Verify via a new Makefile target booting `wm.nex` with
  a test client requesting `glass_intensity=50` over a known backdrop,
  confirming the resulting pixels sit between the unshaded and 100%-
  shaded checksums (bounded between the two known reference outputs,
  not required to match a third exact golden checksum -- the blend
  math is simple enough that "between the two extremes, closer to
  their midpoint" is a real, non-tautological check).
- **Alpha-compositing regression**: confirm `wm-glass` (the existing
  glass-demo smoke test) still passes after the `WM_FORMAT_ARGB8888`
  change, with `wmdemo.c`'s one-line fix applied -- this is the
  concrete check that the automatic-ARGB8888-for-glass change did not
  silently break the one glass surface that already existed before
  this milestone.
- **Taskbar + start menu end-to-end**: a new target boots `wm.nex` +
  `taskbar.nex`, confirms (serial log) the taskbar surface mapped and
  drew without error, then drives a synthetic click at the Start
  button's known screen coordinates (matching how `wm-cursor`'s
  hotspot verification reasoned about known fixed coordinates),
  confirms `startmenu.nex` connected and its surface mapped, confirms
  a second synthetic click on bare desktop is followed by
  `startmenu.nex` exiting (its own `[startmenu]`-prefixed log line for
  "closing on focus loss"), and confirms `wm.nex` itself never panics
  throughout.
- **Clock correctness**: confirm the rendered clock text, sampled via
  screenshot at a known injected time (QEMU's `-rtc base=...` flag),
  matches the expected `HH:MM` -- exercises the actual font-rendering
  and `clock_gettime`/digit-extraction path, not just "some text
  appeared."

## Open questions for planning

- Exact Start-button hit-rect and logo margins: pick concrete pixel
  values during planning against the real 40px bar height and 28x28
  logo, not re-litigated here.
- `taskbar.nex`'s 1-second `poll()` timeout vs. `wm_poll_event`'s
  already-nonblocking drain loop: confirm during planning whether the
  existing `wm_poll_event` (which itself polls with a 0 timeout) needs
  a small wrapper/adjustment for `taskbar.nex`'s outer blocking wait,
  or whether `taskbar.nex` should just `poll()` the raw fd itself
  before calling `wm_poll_event` in a drain loop -- both are valid,
  the plan should pick one and say why.
- `taskbar.nex`'s Makefile rule should mirror `MMSTRESS.ELF`'s exactly
  (crt0.o + `libneoos.a`, `pthread_create` already proven there) --
  confirm no extra flags are needed during planning by checking that
  rule directly rather than re-deriving one.
