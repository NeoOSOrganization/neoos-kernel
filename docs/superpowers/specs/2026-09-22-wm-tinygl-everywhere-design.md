# Routing all of neoos-wm's rendering through TinyGL

> **STATUS: half-baked, superseded, not implemented.** This spec was
> brainstormed and approved section-by-section, but self-review before
> handoff to writing-plans found that Section 3 (GL-native alpha
> blending replacing the hand-rolled blend loops) is not achievable as
> written: the vendored `neoos-tinygl` fork's `glBlendFunc` only
> supports `GL_ONE`/`GL_ZERO`/`GL_ONE_MINUS_SRC_COLOR` -- no
> `GL_SRC_ALPHA`/`GL_ONE_MINUS_SRC_ALPHA`, and no per-pixel alpha is
> tracked through the rasterizer's blend path at all (see
> `neoos-tinygl/upstream/include/zbuffer.h`'s `TGL_BLEND_FUNC` macro).
> No code from this spec was ever written. The project's direction has
> since changed -- see `docs/project-goal.md` -- and this document is
> kept only as a record of that research, not as a plan to execute.

## Context

`neoos-wm`'s compositor (`wm.c`) already links `neoos-tinygl` and uses
it for exactly one thing: the glass-lensing shader pass
(`docs/superpowers/specs/2026-09-20-wm-glass-lensing-design.md`).
Everywhere else -- the desktop background fill, a window's border and
title bar, a window's content blit (including the per-pixel alpha
blend added for glass surfaces in the taskbar/start-menu milestone,
`docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md`
section 3b), and the cursor -- draws by walking raw pointers into
`back`, the compositor's RAM-side composite buffer, with hand-written
C loops.

The motivation for this milestone: TinyGL's `ZBuffer` already targets
`back` directly (`ZB_open(..., ZB_MODE_RGBA, back)`), so the plumbing
to have *everything* render through it already exists for the one
draw call that uses it. Routing every other draw call through the
same GL pipeline is what makes a later swap to real OpenGL / hardware
acceleration a backend change instead of a rewrite -- and, along the
way, two pieces of hand-rolled per-pixel blending math (Task 3's glass
intensity blend, Task 4's ARGB alpha blend) become GL blend state
instead, which is a real code deletion, not just plumbing.

## Goals

- Every pixel `wm.c` writes into `back` -- desktop fill, window
  border/title bar, window content (both `WM_FORMAT_XRGB8888` and
  `WM_FORMAT_ARGB8888`), the glass shader pass, and the cursor -- goes
  through TinyGL draw calls. The only raw-pointer pixel write left in
  `wm.c` after this milestone is the final damaged-rect copy from
  `back` to the real framebuffer `fb` (see Non-goals).
- `wm.c` itself never calls a TinyGL/GL function directly. All GL
  calls live in one new file, `renderer.c`/`renderer.h`, behind a
  small backend-agnostic API. Swapping TinyGL for a real OpenGL
  context later means writing a new `renderer.c`, not touching `wm.c`.
- The two hand-rolled per-pixel blend loops this milestone can delete
  -- the glass intensity blend (`wm.c`, added for the taskbar/start
  menu milestone's Task 3) and the ARGB8888 content alpha blend (same
  milestone's Task 4) -- are deleted and replaced by GL blend state
  (`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`), driven from
  `renderer.c`.
- Per-client GL textures are cached across frames and only re-uploaded
  when that client's content actually changed (`WM_COMMIT`), so this
  milestone does not regress compositing performance versus today's
  direct-memcpy blits.
- The glass shader's backdrop snapshot samples with `GL_LINEAR`
  (bilinear) filtering instead of today's implicit nearest-neighbor,
  for a visibly smoother frosted look. Shader code itself (the SPIR-V
  vertex/fragment programs) is unchanged.
- Every existing end-to-end target (`wm`, `wm-glass`, `wm-cursor`,
  `wm-cursor-fallback`, `wm-taskbar`) passes unchanged after this
  conversion.

## Non-goals

- Swapping TinyGL for real OpenGL or a hardware-accelerated backend.
  This milestone only creates the seam (`renderer.c`) that a future
  milestone would replace -- it does not attempt the replacement.
- Any change to the wire protocol (`wmproto.h`) or to any client
  (`wmclient.c`, `taskbar.c`, `startmenu.c`, `wmdemo.c`). This is
  compositor-internal; nothing a client can observe changes.
- Routing the final `back` -> `fb` scanout copy through a GL draw
  call. That copy is presentation, not rendering -- the same
  boundary a real GL pipeline draws between draw calls and
  swap-buffers/DMA -- and stays a raw memcpy of the damaged rect, same
  as today. Treating `fb` as a render target is explicitly out of
  scope.
- Multi-pass or deferred rendering, batching multiple windows into one
  draw call, or any other performance technique beyond the per-client
  texture cache described below. "Must not regress" is the bar, not
  "must get faster."
- A permanent instrumentation/telemetry counter for GL uploads. A
  throwaway debug log line is used during this milestone's own
  verification and removed before the final commit (see Testing).

## Design

### 1. `renderer.h`'s API surface

```c
// renderer.h -- the only header wm.c includes for drawing. No GL type
// (GLuint, etc.) appears here; renderer.c owns those internally.

// Binds the render target once at startup. Mirrors today's
// ZB_open(..., ZB_MODE_RGBA, back) call, which renderer_init performs
// internally.
int  renderer_init(uint32_t *target, uint32_t w, uint32_t h);

// A flat-colored rectangle -- desktop background, window border,
// title bar. No texture involved.
void renderer_fill_rect(int x, int y, int w, int h, uint32_t rgb);

// Draws a client's pixel buffer as a textured quad at (x,y). `cache`
// is the client's persistent, opaque texture handle (see struct
// client's new gl_tex field, section 2) -- renderer_blit_surface
// allocates it on first use and reuses it thereafter. `dirty` tells
// the renderer whether to re-upload this frame; the caller (wm.c)
// owns when that's true (WM_COMMIT), the renderer owns the upload
// itself. format selects GL_BLEND on/off: WM_FORMAT_ARGB8888 blends
// (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), WM_FORMAT_XRGB8888 does not
// (matching today's straight-copy behavior for opaque surfaces).
void renderer_blit_surface(int x, int y, int w, int h,
                            const uint32_t *pixels, uint32_t stride_px,
                            uint32_t format, int dirty, void **cache);

// The cursor: same shape as renderer_blit_surface but the caller
// never sets dirty after the first call -- the bitmap is static once
// loaded (see section 2).
void renderer_blit_cursor(int x, int y, int w, int h,
                           const uint32_t *argb, void **cache);

// Runs the existing glass shader pass: uploads `snapshot_rgba`
// (GL_LINEAR filtered, see Goals) as the shader's input texture,
// draws the shaded quad, then blends it over the backdrop already in
// `target` at `intensity`% via glColor4f-modulated alpha -- replaces
// wm.c's current two-step "run shader, then hand-blend toward
// original" with one blended draw. vs/fs are the existing
// TGLspvModule pointers wm.c already owns (glass_vertex_spv.h /
// glass_fragment_spv.h); renderer.c does not know or care that
// they're SPIR-V specifically, it just forwards them to
// tglUseSpvProgram the way draw_glass does today.
void renderer_draw_glass(int x, int y, int w, int h,
                          const unsigned char *snapshot_rgba,
                          uint8_t intensity, void *vs, void *fs);

// Frees a cache handle (renderer_blit_surface's or
// renderer_blit_cursor's) -- called from drop_client.
void renderer_free_cache(void **cache);
```

`renderer.c` keeps `g_glass_zb` (renamed `g_zb`, no longer
glass-specific) and all `#include "zbuffer.h" / "GL/gl.h" / "spv_vm.h"
/ "spv_gl.h"` internally; `wm.c` drops those includes entirely once
its own draw calls no longer need them (it still includes
`glass_vertex_spv.h`/`glass_fragment_spv.h` to own the `TGLspvModule*`
globals it passes into `renderer_draw_glass`, exactly as it owns them
today).

### 2. `struct client`'s new field, and cache lifetime

```c
struct client {
    ...
    void *gl_tex;   // opaque to wm.c -- renderer.c's internal
                     // { GLuint id; int w, h; } *, allocated lazily
                     // by renderer_blit_surface's first call, freed
                     // by renderer_free_cache from drop_client
};
```

`struct client` also gains `int content_dirty`, but it costs no new
tracking logic: `WM_COMMIT`'s handler is already the one and only
place a client's buffer can change, so that handler just sets
`content_dirty = 1` alongside whatever it already does. `wm.c` passes
`content_dirty` as `renderer_blit_surface`'s `dirty` argument each
repaint and clears it right after the call returns -- clearing lives
in `wm.c`, not `renderer.c`, so the dirty/clean state machine stays in
the one file that already owns `mapped`.

The cursor's cache (`g_cursor`'s texture) is a single global handle in
`wm.c` (not per-client), allocated on the cursor's first
`renderer_blit_cursor` call and never invalidated after -- the bitmap
(`xcursor.c`'s loaded theme, or the built-in fallback) is fixed for
the process's lifetime; there is no live cursor-theme-reload feature
to invalidate it for.

### 3. Blend state replaces two hand-rolled loops

Both deletions are pure subtractions from `wm.c`, with the equivalent
math now expressed as GL state inside `renderer.c`:

- **Window content alpha** (`draw_window`'s per-pixel loop, added
  Task 4 of the taskbar/start-menu milestone): deleted. Its job --
  blend a straight-alpha ARGB8888 source over whatever's already in
  `back`, or a flat opaque copy for XRGB8888 -- becomes
  `renderer_blit_surface`'s `format` argument selecting
  `glEnable(GL_BLEND)` + `glBlendFunc(GL_SRC_ALPHA,
  GL_ONE_MINUS_SRC_ALPHA)` (ARGB8888) vs `glDisable(GL_BLEND)`
  (XRGB8888) before the textured quad draw.
- **Glass intensity blend** (`draw_glass`'s post-shader loop, added
  Task 3 of the same milestone): deleted. `renderer_draw_glass` draws
  the shaded result as a textured quad over the (already-present,
  unshaded) backdrop with the same blend function, modulated by
  `glColor4f(1, 1, 1, intensity / 100.0f)` -- mathematically the same
  `(shaded*intensity + orig*(100-intensity))/100` the loop computed,
  now the rasterizer's job.

### 4. Glass backdrop filtering

`renderer_draw_glass`'s texture upload sets
`glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)` and
same for `GL_TEXTURE_MAG_FILTER` before binding the snapshot -- today
this parameter is left at TinyGL's default (nearest-neighbor). No
shader (SPIR-V) change; the fragment program's texture sample is
unaffected by which filter mode the sampled texture was uploaded with.

### 5. Where each existing call site changes

| `wm.c` call site (today) | Becomes |
|---|---|
| `fill(0,0,fb_w,fb_h,0x00202830)` (desktop clear, in `repaint`) | `renderer_fill_rect(0,0,fb_w,fb_h,0x00202830)` |
| `draw_window`'s border/title-bar `fill(...)` calls | `renderer_fill_rect(...)`, unchanged call sites, new implementation |
| `draw_window`'s content copy loop (Task 4's ARGB blend + the original straight XRGB copy) | one `renderer_blit_surface(..., c->px, c->stride_px, c->format, c->content_dirty, &c->gl_tex)` call |
| `draw_cursor`'s ARGB blit loop | `renderer_blit_cursor(..., g_cursor.argb, &g_cursor_gl_tex)` |
| `draw_glass`'s snapshot-capture + `tglUseSpvProgram`/`glBegin(GL_QUADS)` block + intensity-blend loop | snapshot capture (`back` -> `unsigned char *snapshot`, this part is inherently a CPU-side read of the pre-shade backdrop, stays as-is) followed by one `renderer_draw_glass(...)` call |
| `repaint`'s damaged-rect `back` -> `fb` copy loop | unchanged (Non-goals) |

### 6. Error handling

`renderer_init`'s failure path (today: `ZB_open` returning null)
matches the existing behavior exactly -- `wm.c` already handles
`g_glass_zb` being null by printing `"[wm] glass: ZB_open failed --
glass surfaces will render as opaque windows"` and skipping
`draw_glass`. After this milestone, `renderer_init` failing is more
severe (nothing renders at all, not just glass), so `wm.c` treats it
as a boot-time fatal error (matches how `screen_open`'s other
failure modes are already handled) rather than a degraded-mode
fallback.

## Testing

- Every existing end-to-end target (`make WM_DIR=../neoos-wm wm`,
  `wm-glass`, `wm-cursor`, `wm-cursor-fallback`, `wm-taskbar`) must
  pass unchanged after the conversion -- this is the regression net,
  run in full after implementation.
- `wm-glass`'s screenshot check gains an explicit pixel-sampling
  assertion (not just "a surface mapped"): confirm no fully-transparent
  gap at a glass surface's texture edge, since nearest-vs-linear
  filtering (section 4) is exactly the kind of thing that silently
  regresses without a real pixel check.
- A throwaway `[wm] gl_upload_count=N` log line (behind no `#ifdef` --
  cheap enough to leave compiled in, but explicitly not a permanent
  feature per Non-goals) is added during implementation to confirm the
  texture cache actually skips re-uploading an unchanged client's
  content across repeated repaints, then removed before the
  milestone's final commit -- the same instrument-then-remove pattern
  used for `pump_mouse` during the taskbar/start-menu milestone's
  Task 7 debugging.
- No change to `neoos-wm/test/`'s host-side unit tests (Xcursor binary
  parsing, etc.) -- this milestone is compositor-internal rendering,
  not protocol or parsing logic.

## Known limitation carried forward

Z-order-correct backdrop capture for glass surfaces (a glass panel
sampling only what's *actually* beneath it in stacking order) remains
the same known, explicitly-accepted limitation named in
`docs/superpowers/specs/2026-09-20-wm-glass-lensing-design.md`. This
milestone does not touch stacking-order logic at all.
