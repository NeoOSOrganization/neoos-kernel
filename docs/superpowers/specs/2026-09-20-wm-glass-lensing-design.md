# Compositor-level glass lensing in neoos-wm

## Context

Sub-project 1 (`docs/superpowers/specs/2026-09-20-tinygl-spirv-shader-stage-design.md`,
shipped) gave `neoos-tinygl` a SPIR-V vertex+fragment shader stage: a
program can be bound with `tglUseSpvProgram`, and the fragment stage
can sample a bound 2D texture. That spec named this as sub-project 2:
compositor-level lensing, where glass panels sample *other* windows'
content behind them.

This spec covers only sub-project 2: making `neoos-wm` (now a sibling
repo, extracted from NeoOS this session -- see the wm-extraction
commits) composite designated surfaces as Apple-style "Liquid Glass":
translucent panels that refract and tint whatever is behind them,
rather than a flat alpha blend.

Sub-project 3 (`neoos-uikit`, a themed widget layer built on this) and
sub-project 4 (a terminal emulator as the first real app) are out of
scope here.

## Goals

- A client can request a surface that the compositor renders as glass:
  the compositor snapshots what is directly behind that surface's
  screen rect, runs it through a built-in refraction+tint shader, and
  composites the result in the surface's place.
- The effect is visibly a *lens*, not a blur or a flat translucency:
  pixels near a glass surface's edges sample displaced positions from
  the backdrop, growing with distance from center, the way a curved
  glass edge bends light more than its center.
- Closes sub-project 1's known gap so the fragment shader can express
  this at all: `spv_vm_run_fragment`'s `inputs`/`ninputs` are wired to
  a real `OpLoad` of an `Input`-class SPIR-V variable, so a fragment
  shader can read a per-pixel varying (a quad-local UV) instead of
  only compile-time constants.
- Existing (non-glass) window compositing is unaffected in output and
  cost when no surface requests `WM_SURFACE_GLASS`.

## Non-goals

- Per-pixel blur. Deferred; expressible later with multi-tap sampling
  of the one bound texture, but not attempted this milestone.
- Specular highlights / dynamic lighting response. Deferred to uikit
  polish (sub-project 3).
- True z-order-correct backdrop capture (a glass surface sampling only
  what is *actually* beneath it in stacking order, including other
  glass surfaces at arbitrary positions in the stack). See "Known
  limitation" below -- explicitly accepted, not solved here.
- Client-supplied shader bytecode. The compositor never loads a
  `.spv` module it did not ship itself.
- Any change to `neoos-tinygl`'s public API beyond wiring up the
  existing `inputs`/`ninputs` parameters -- `tglUseSpvProgram`,
  `tglTexImage2D`, `tglViewport`, `tglBegin(GL_QUADS)` are used as
  they exist today.

## Design

### 1. Protocol: `WM_SURFACE_GLASS`

`wmproto.h` gains a second surface flag bit, alongside the existing
`WM_SURFACE_SHELL`:

```c
#define WM_SURFACE_NORMAL 0
#define WM_SURFACE_SHELL  1
#define WM_SURFACE_GLASS  2
```

Any client may set it in `wm_create_surface.flags` -- it is not
reserved for compositor-owned decoration. This is deliberate: sub-
project 3 (`neoos-uikit`) needs ordinary application clients to be
able to ask for real glass panels, not just a compositor-drawn chrome
effect. A surface can be `WM_SURFACE_GLASS` and otherwise behave like
a normal window (has decoration, is positioned and moved the same
way, receives input the same way) -- the flag changes only how
`repaint()` draws its content.

`struct client` (in `wm.c`) gains `int is_glass`, set from the flag at
`WM_CREATE_SURFACE` time exactly as `is_shell` is today.

### 2. Compositing: two-pass, not z-order-interleaved

Today's `repaint()` does one pass: shell first, then every other
client in slot order, each drawn straight into `back`. This changes to
two passes:

**Pass 1** -- unchanged from today, except glass clients are skipped:
composite the desktop background, the shell, and all non-glass windows
into `back`, in the existing order (shell, then normal windows by
slot, cursor last -- cursor still drawn after pass 2, see below).

**Pass 2** -- walk glass clients in the same slot order normal windows
use today (i.e., connection order, not a separate z-list):
1. Snapshot the client's screen rect (`x, y, w, h` including its
   decoration, matching what `damage_window` already computes) out of
   the *current* contents of `back`, converting from `back`'s packed
   `0x00RRGGBB` to RGBA8 as it's copied.
2. Bind that snapshot as the VM's sampled texture
   (`spv_vm_bind_texture`).
3. Draw a textured quad over the same rect using the built-in glass
   shader (`tglUseSpvProgram` + `glViewport` + `glBegin(GL_QUADS)`) --
   TinyGL's shared render target IS `back` (see section 3), so this
   writes the shaded result directly into `back` at the same location,
   no separate copy-back step.
4. The client's own buffer content (what it drew via
   `WM_ATTACH_BUFFER`/`WM_COMMIT`) is composited *after* the lensed
   backdrop, inside the same quad's footprint, the same way normal
   window content is blitted today -- glass is the panel's
   *background* treatment, not a replacement for its content. (A
   glass surface with no attached buffer -- pure chrome -- is the
   common case for sub-project 3's panels; this rule still applies,
   it just has nothing extra to draw.)

Cursor is drawn last, after both passes, as today.

**Known limitation, explicitly accepted:** because pass 2 runs after
*all* of pass 1, every glass surface reads a backdrop that already
includes every non-glass window, regardless of true connection order
-- a glass panel "under" a normal window in real stacking order still
sees that window through itself. Only relative order *among* glass
surfaces is preserved (a second glass panel drawn after a first can
lens the first's already-lensed output). This is a real deviation from
correct z-order compositing. It is accepted because: interleaving
would mean re-deriving `back`'s partial state at each z-order
crossing, a materially more complex compositor loop, for a case
(multiple overlapping glass and non-glass windows in a specific
stacking arrangement) that does not arise in the sub-project 3 uikit
usage this is built for (glass panels are typically top-level chrome:
menu bars, control panels, sheets -- not interleaved with arbitrary
app windows). Revisit if/when a real usage needs it.

### 3. Shader invocation: wm.c depends on neoos-tinygl directly

`wm.c` links `neoos-tinygl` (new build dependency: `TINYGL_DIR`,
mirroring how `NeoOS/Makefile` already consumes `WM_DIR` and
`TINYGL_DIR` for `spvtest`).

TinyGL's `ZB_open(xsize, ysize, ZB_MODE_RGBA, frame_buffer)` renders
into a caller-supplied pixel buffer when `frame_buffer` is non-NULL,
and its 32-bit pixel layout (`0x00RRGGBB`, per `zbuffer.h`'s
`GET_RED`/`GET_GREEN`/`GET_BLUE`) is exactly `wm`'s own `XRGB8888
back` buffer format. So at startup `wm` opens ONE TinyGL context with
`back` itself as the render target -- `ZB_open(fb_w, fb_h,
ZB_MODE_RGBA, back)` + `glInit(zb)`, once, not per-frame -- and a
shaded quad written by TinyGL lands directly in `back` at the right
screen location with no separate copy-back step. `glViewport` is what
confines a full `-1..1` NDC quad to one glass surface's screen rect
within that shared buffer:

```c
spv_vm_bind_texture(snapshot_rgba, rect.w, rect.h); // this surface's captured backdrop, converted to RGBA8
tglUseSpvProgram(glass_vertex_spv, glass_fragment_spv);
glViewport(rect.x, rect.y, rect.w, rect.h);
glBegin(GL_QUADS);
  glVertex3f(-1,-1,0); glVertex3f(1,-1,0); glVertex3f(1,1,0); glVertex3f(-1,1,0);
glEnd();
```

`spv_vm_bind_texture` (the VM-level texture bind sub-project 1 already
uses in its own fixture test) is used directly rather than going
through `glTexImage2D`/`glBindTexture` -- TinyGL's GL-level texture
path forces every texture to a fixed square `TGL_FEATURE_TEXTURE_DIM`
and only accepts 3-byte RGB input, neither of which fits an
arbitrary-sized, arbitrary-aspect-ratio window snapshot; the VM-level
bind takes raw RGBA8 at any width/height, which is what a captured
screen rect actually is.

This reuses sub-project 1's tested vertex-transform and rasterizer
path as-is (`spv_gl_run_vertex` -> clip -> `spv_gl_fill_triangle`)
rather than calling `spv_vm_run_fragment`/`spv_vm_run_vertex`
directly -- no new entry point into the VM, no new draw path to test
independently of what sub-project 1 already proved.

### 4. Shader source: compositor-built-in only

Two `.spv` modules (vertex + fragment), authored in GLSL, compiled
host-side with `glslang` at build time (same toolchain sub-project 1
established), embedded into `wm.c` as byte arrays (mirroring
`spvtest.c`'s fixture-embedding pattern in neoos-tinygl's test tree).
No wire message exists, or will exist, for a client to hand the
compositor shader bytecode. `wm` is the one process that owns the
screen and every client's pixels; making it interpret client-supplied
bytecode would turn that trust boundary into a code-execution surface
reachable by any connected client. If a future milestone wants
per-client-authored glass effects, that needs its own threat-modeled
design -- not assumed here.

**Vertex shader**: passes through clip-space position, emits a
quad-local UV (0..0 at one corner, 1..1 at the opposite) as an output
varying.

**Fragment shader**: built-in effect scope for this milestone --
1. **Refraction offset**: computes a per-pixel sample displacement
   from the UV, growing with distance from the quad's center (e.g.
   `offset = (uv - 0.5) * edge_strength * distance_from_center`),
   applied when sampling the bound backdrop texture. This is the one
   effect a flat 2D blit categorically cannot do, and is the actual
   point of this milestone.
2. **Uniform tint multiply**: a constant color multiplied into the
   sampled result, giving the glass a frosted/tinted look. A single
   built-in constant for this milestone (not yet client-configurable
   -- sub-project 3 can plumb a per-surface tint uniform later if
   needed).

### 5. Closing sub-project 1's gap: real fragment inputs

`spv_vm_run_fragment(mod, inputs, ninputs, output)` in
`neoos-tinygl/spv/spv_vm.c` currently does:

```c
(void)inputs; (void)ninputs; /* Task 8/9 bind varyings to fixed ids */
```

-- every sub-project-1 fixture fragment shader sampled a compile-time-
constant UV, because nothing wired `inputs` to the VM's execution of
`OpLoad` against an `Input`-storage-class `%var`. This sub-project
wires it for real:

- `spv_gl_fill_triangle`'s per-pixel loop already has the rasterizer-
  interpolated varyings available (the vertex stage emits the UV
  output, the rasterizer interpolates it across the triangle -- this
  interpolation path already works, sub-project 1 built it for
  texcoords). Pack the interpolated varying(s) for the current pixel
  into a `spv_value_t inputs[]` array before calling
  `spv_vm_run_fragment`.
- Inside `spv_vm_run_fragment`, `OpLoad` against a variable declared
  `Input` storage class resolves to the matching slot in `inputs[]`
  (matched by `Location` decoration, the same mechanism already used
  for the vertex stage's attribute inputs, if sub-project 1 established
  one -- confirm against `spv_vm.c`'s existing `Input`-class handling
  for the vertex stage and mirror it; do not invent a second
  convention).
- This is the mechanism that lets the fragment shader compute the
  refraction offset from a genuinely per-pixel UV rather than a
  constant -- without it, "lensing that grows toward the edges" is not
  expressible at all.

This is a `neoos-tinygl` change, not a `neoos-wm` change; it lands as
part of this milestone's implementation because sub-project 2 cannot
produce a real lensing effect without it, but it is general-purpose
VM plumbing, not glass-specific.

## Cross-repo scope

Implementation touches:

- **neoos-tinygl**: wire `spv_vm_run_fragment`'s `inputs`/`ninputs`
  (section 5). No public API signature changes.
- **neoos-wm**: `wmproto.h` (`WM_SURFACE_GLASS`), `wm.c` (`is_glass`
  tracking, two-pass `repaint()`, TinyGL linkage, built-in shader
  embedding), `Makefile` (new `TINYGL_DIR` build dependency).
- **NeoOS**: none expected. `NeoOS/Makefile` already builds `wm.nex`
  from a prebuilt `WM_DIR`; it does not need to know `wm` now also
  depends on `TINYGL_DIR` internally, the same way it does not know
  `wm`'s other internal dependencies today. Confirm during planning
  whether `wm-shot`/`wm-run` targets need a `TINYGL_DIR` passthrough
  for local iteration convenience -- not a hard requirement, a
  convenience call for the plan to make.
- **docs/stdlib.md**: update the `WM_SURFACE_*` flag documentation to
  add `WM_SURFACE_GLASS` and describe what requesting it does
  observably (a client sees no protocol difference in how it draws --
  glass is purely a compositor-side rendering choice -- but the doc
  should say so explicitly, since "what does this flag change from the
  client's point of view" is exactly the kind of thing an ABI-facing
  doc must not leave implicit).

## Error handling

- Built-in shader modules fail to load (should be impossible for
  compositor-authored, build-time-validated `.spv` -- but `wm` must
  not assume its own assets are trustworthy any more than a client's):
  `wm` logs the reason and falls back to compositing that surface as
  an ordinary opaque window (skip pass 2 for it) rather than crashing
  or leaving that screen region undrawn. A degraded desktop beats a
  dead compositor.
- `tglTexImage2D`/snapshot allocation failure for an oversized or
  numerous glass surface: same fallback -- draw that surface without
  the lensing pass rather than aborting `repaint()` for the whole
  screen. One misbehaving or oversized glass client must not be able
  to freeze compositing for every other window.
- A glass surface with zero width/height or an off-screen rect: skip
  pass 2 for it (nothing to snapshot), matching how `damage_window`
  already no-ops for unmapped/zero-size clients today.

## Testing

Same headless-QEMU + serial-checksum convention sub-project 1 used:

- **Golden-output checksum test**: boot `wm` headless with a known
  static backdrop and one glass surface at a known rect, force a
  repaint, checksum the resulting framebuffer region the same way
  sub-project 1's `spvtest` checksums shader output. Catches
  regressions in the refraction math or the two-pass ordering.
- **`wm`-level smoke test**: a glass-surface client (extend `wmdemo` or
  add a new minimal test client) that creates a `WM_SURFACE_GLASS`
  surface over a busy backdrop (e.g. the existing gradient test
  window) and confirms the compositor does not crash, hang, or drop
  the client, and that *some* visible change occurs relative to the
  same layout with the flag unset (a coarse "the shader ran" check,
  not exact-pixel).
- **neoos-tinygl regression**: confirm sub-project 1's existing
  `spvtest` fixtures still pass unchanged after `spv_vm_run_fragment`
  starts consuming real inputs -- fixtures that never declared an
  `Input`-class fragment variable must be unaffected (empty
  `inputs[]`/`ninputs == 0` stays a legal, working call).

## Open questions for planning

- Exact `Input`/`Location` matching convention for fragment varyings:
  confirm sub-project 1 already established one for some stage (likely
  the vertex stage's object-space attributes) and mirror it exactly,
  rather than deciding fresh here.
- Snapshot format/size limits: is there a maximum glass-surface size
  this milestone commits to (memory/time cost of `tglTexImage2D` per
  repaint, per glass surface, every frame it's dirty)? Not addressed
  above; the plan should size this against `neoos-wm`'s existing
  damage-tracking cost model rather than assume it is free.
