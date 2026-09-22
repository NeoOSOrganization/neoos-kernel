# Routing all of neoos-wm's rendering through real Mesa

## Context

`docs/superpowers/specs/2026-09-22-wm-tinygl-everywhere-design.md`
("TinyGL everywhere") was brainstormed and approved section-by-section
in September 2026, then marked half-baked and abandoned before
implementation: self-review found that its Section 3 (GL-native alpha
blending replacing `wm.c`'s two hand-rolled per-pixel blend loops) was
not achievable, because `neoos-tinygl`'s `glBlendFunc` only supports
`GL_ONE`/`GL_ZERO`/`GL_ONE_MINUS_SRC_COLOR` — no
`GL_SRC_ALPHA`/`GL_ONE_MINUS_SRC_ALPHA`, and no per-pixel alpha is
tracked through its rasterizer's blend path at all. No code from that
spec was ever written.

Since then, Gallium3D sub-projects 1 and 2
(`docs/project-goal.md`) proved that real Mesa — the actual upstream
project, `softpipe` Gallium driver, full GLSL compiler — cross-compiles
for NeoOS and renders correctly, both off-screen (OSMesa,
sub-project 1) and into a real composited window via a new
`EGL_PLATFORM_NEOOS_MESA` platform (sub-project 2). Real Mesa has
none of `neoos-tinygl`'s blending limitation. This spec revives the
TinyGL-everywhere design with Mesa's OSMesa API as the backend instead
of TinyGL, closing the exact gap that stalled the original attempt.

`neoos-wm`'s compositor (`wm.c`) currently links `neoos-tinygl` for
exactly one thing: the glass-lensing shader pass
(`docs/superpowers/specs/2026-09-20-wm-glass-lensing-design.md`).
Everywhere else — desktop background fill, window border/title bar,
window content blit (including the per-pixel ARGB8888 alpha blend
added in the taskbar/start-menu milestone,
`docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md`
section 3b), and the cursor — draws by walking raw pointers into
`back`, the compositor's RAM-side composite buffer, with hand-written
C loops.

**Constraint carried forward unchanged: `neoos-tinygl` itself is not
touched, removed, or repurposed by this milestone.** It stays available
for direct TinyGL application use. This milestone only stops `wm.c`
itself from depending on it.

## Goals

- Every pixel `wm.c` writes into `back` — desktop fill, window
  border/title bar, window content (both `WM_FORMAT_XRGB8888` and
  `WM_FORMAT_ARGB8888`), the glass shader pass, and the cursor — goes
  through real Mesa GL draw calls. The only raw-pointer pixel write
  left in `wm.c` after this milestone is the final damaged-rect copy
  from `back` to the real framebuffer `fb` (see Non-goals).
- `wm.c` itself never calls a GL function directly. All GL calls live
  in one file, `renderer.c`/`renderer.h`, behind a small
  backend-agnostic API — unchanged in spirit from the original design,
  now backed by OSMesa instead of TinyGL.
- The two hand-rolled per-pixel blend loops — the glass intensity
  blend and the ARGB8888 content alpha blend — are deleted and
  replaced by real GL blend state (`glBlendFunc(GL_SRC_ALPHA,
  GL_ONE_MINUS_SRC_ALPHA)`), driven from `renderer.c`. This is the
  exact goal the original design couldn't reach.
- The glass shader is rewritten as standard GLSL, compiled at runtime
  via `glCreateShader`/`glCompileShader`/`glLinkProgram`, preserving
  the existing shader's exact semantics (recovered from the
  still-present `glass_vertex.spvasm`/`glass_fragment.spvasm`: a
  passthrough vertex stage, and a fragment stage computing
  `sample(uv + (uv - 0.5) * 0.06) * tint(0.92, 0.97, 1.0, 1.0)`).
  `neoos-tinygl`'s custom SPIR-V VM (`spv_vm.h`/`spv_gl.h`) and the
  compiled `.spv`/`.h` modules are dropped from `wm.c` and
  `neoos-wm`'s `Makefile` entirely.
- `wm.nex` moves off its current freestanding build
  (`x86_64-elf-gcc -ffreestanding -nostdlib -static`, musl's `crt1.o`
  linked statically purely for TinyGL's malloc needs) onto the
  **hosted** `x86_64-neoos-linux-musl` toolchain with real dynamic
  linking (`PT_INTERP`/`ld-musl-x86_64.so.1`) — the same toolchain and
  pattern `neoos-mesa`'s own test clients already use.
  `wmclient.c`/`wmdemo.c`/`taskbar.c`/`startmenu.c` are unaffected —
  separate binaries, unchanged toolchain.
- `disk.img` grows from its current 32MB to a size that permanently
  fits `libOSMesa.so.8`, `libglapi.so.0`, `libstdc++.so.6`,
  `libgcc_s.so.1`, `libc.so`, `ld-musl-x86_64.so.1` at `/lib` on every
  build (not opt-in) — `wm` runs on every real desktop boot and every
  `wm-*` gauntlet target, so these libraries must always be present,
  not staged ad hoc per test.
- Per-client GL textures are cached across frames and only re-uploaded
  when that client's content actually changed (`WM_COMMIT`), so this
  milestone does not regress compositing performance versus today's
  direct-memcpy blits.
- The glass shader's backdrop snapshot samples with `GL_LINEAR`
  (bilinear) filtering instead of today's implicit nearest-neighbor,
  for a visibly smoother frosted look.
- Every existing end-to-end target (`wm`, `wm-glass`, `wm-cursor`,
  `wm-cursor-fallback`, `wm-taskbar`) passes unchanged after this
  conversion, and the full gauntlet stays at 15/15 zero-retry.

## Non-goals

- Any change to the wire protocol (`wmproto.h`) or to any client
  (`wmclient.c`, `taskbar.c`, `startmenu.c`, `wmdemo.c`). This is
  compositor-internal; nothing a client can observe changes.
- Any change to, removal of, or repurposing of `neoos-tinygl`. It
  remains a fully independent, working library, unaffected by this
  milestone, for direct application use later.
- Routing the final `back` → `fb` scanout copy through a GL draw call.
  That copy is presentation, not rendering — the same boundary a real
  GL pipeline draws between draw calls and swap-buffers/DMA — and
  stays a raw `memcpy` of the damaged rect, same as today.
- Using the new `EGL_PLATFORM_NEOOS_MESA` platform (sub-project 2) for
  `wm`'s own rendering. That platform exists for *client* applications
  rendering into a `wm`-owned window; `wm` itself already owns its
  target buffer (`back`) directly, so OSMesa's "render into an
  arbitrary buffer I already have" API is the correct fit, exactly as
  `ZB_open(..., back)` was.
- Multi-pass or deferred rendering, batching multiple windows into one
  draw call, or any other performance technique beyond the per-client
  texture cache described above. "Must not regress" is the bar, not
  "must get faster."
- A permanent instrumentation/telemetry counter for GL uploads. A
  throwaway debug log line, used during this milestone's own
  verification, is removed before the final commit.
- Any raw-pointer fallback rendering path for when Mesa initialization
  fails. See Error Handling.
- Any change to `neoos-os-builder`'s desktop config beyond the disk
  size and runtime-library staging this milestone requires directly
  (tracked in Testing/Documentation, not scoped further here).

## Design

### 1. `renderer.h`'s API surface

Unchanged in shape from the original TinyGL-everywhere design — this
milestone is a backend swap, not an API redesign:

```c
// renderer.h -- the only header wm.c includes for drawing. No GL type
// (GLuint, etc.) appears here; renderer.c owns those internally.

// Binds the render target once at startup via OSMesaCreateContext +
// OSMesaMakeCurrent(ctx, target, GL_UNSIGNED_BYTE, w, h) -- mirrors
// today's ZB_open(..., ZB_MODE_RGBA, back) call. Returns 0 on success;
// on failure, wm.c logs and exits (see Error Handling) rather than
// falling back to raw-pointer drawing.
int  renderer_init(uint32_t *target, uint32_t w, uint32_t h);

// A flat-colored rectangle -- desktop background, window border,
// title bar. No texture involved.
void renderer_fill_rect(int x, int y, int w, int h, uint32_t rgb);

// Draws a client's pixel buffer as a textured quad at (x,y). `cache`
// is the client's persistent, opaque GL texture handle (see struct
// client's new gl_tex field, section 2) -- allocated on first use,
// reused thereafter. `dirty` tells the renderer whether to re-upload
// this frame; the caller (wm.c) owns when that's true (WM_COMMIT),
// the renderer owns the upload itself. `format` selects blend state:
// WM_FORMAT_ARGB8888 enables glBlendFunc(GL_SRC_ALPHA,
// GL_ONE_MINUS_SRC_ALPHA), WM_FORMAT_XRGB8888 disables blending
// (matching today's straight-copy behavior for opaque surfaces).
void renderer_blit_surface(int x, int y, int w, int h,
                            const uint32_t *pixels, uint32_t stride_px,
                            uint32_t format, int dirty, void **cache);

// The cursor: same shape as renderer_blit_surface but the caller
// never sets dirty after the first call -- the bitmap is static once
// loaded.
void renderer_blit_cursor(int x, int y, int w, int h,
                           const uint32_t *argb, void **cache);

// Runs the glass shader pass: uploads `snapshot_rgba` (GL_LINEAR
// filtered) as the shader's input texture, draws the shaded quad,
// blends it over the backdrop already in `target` at `intensity`% via
// real alpha blending -- replaces wm.c's current two-step "run
// shader, then hand-blend toward original" with one blended draw.
// `program` is renderer.c's own compiled-and-linked GLSL program
// handle (opaque to wm.c), created once at renderer_init time.
void renderer_draw_glass(int x, int y, int w, int h,
                          const unsigned char *snapshot_rgba,
                          uint8_t intensity);

// Frees a cache handle (renderer_blit_surface's or
// renderer_blit_cursor's) -- called from drop_client.
void renderer_free_cache(void **cache);

// Torn down at exit, or on a failed renderer_init (nothing to do in
// the latter case, provided for symmetry).
void renderer_shutdown(void);
```

Two shape changes from the original design, both consequences of the
backend swap: `renderer_draw_glass` no longer takes `vs`/`fs` module
pointers (the GLSL program is compiled once, internally, at init —
there is no per-call module hand-off the way `wm.c` owned
`TGLspvModule*` pointers before), and `renderer_init`'s failure
contract is now hard-failure, not "caller decides a fallback."

`renderer.c` owns an `OSMesaContext`, the compiled glass GLSL program,
and both textures the glass pass needs (backdrop snapshot, and any
scratch state) entirely internally. `wm.c` drops `zbuffer.h`,
`GL/gl.h`, `spv_vm.h`, `spv_gl.h`, `glass_vertex_spv.h`, and
`glass_fragment_spv.h` — it includes only `renderer.h`.

### 2. Per-client texture cache

`struct client` gains one new field: `void *gl_tex` (opaque — owned
and interpreted only by `renderer.c`, which stores a `GLuint` cast
through it). `drop_client` calls `renderer_free_cache(&c->gl_tex)`
where it previously had nothing to free for TinyGL (glass was the only
GL consumer before, and it had no per-client state — each glass
surface's backdrop snapshot was transient, one-shot per frame).

`WM_COMMIT` already flips a per-client dirty flag `wm.c` owns
today for deciding whether to re-blit; that same flag becomes the
`dirty` argument to `renderer_blit_surface`, unchanged in meaning.

### 3. Build system: `neoos-wm`'s `Makefile`

`WM.ELF`'s build rule drops `CC := x86_64-elf-gcc` and its entire
freestanding flag set (`-ffreestanding -fno-stack-protector
-mno-red-zone ... -static -nostdlib -fno-pic -mcmodel=large
-ftls-model=local-exec`, `-T user.ld`) in favor of the hosted
`x86_64-neoos-linux-musl-gcc` toolchain, ordinary dynamic linking, and
no custom linker script — mirroring exactly how `neoos-mesa/test`'s
`egl_triangle_client.nex` already builds. `TINYGL_DIR`,
`$(TINYGL_DIR)/build/libTinyGL.a`, and the `-I$(TINYGL_DIR)/upstream/include
-I$(TINYGL_DIR)/spv` include paths are removed from `WM_CFLAGS`;
`$(MUSL_DIR)/lib/crt1.o`/`libc.a` static-link dependencies are replaced
by a normal `-L.../lib -lOSMesa -lGL... ` (whichever subset
`renderer.c` actually calls into — likely just `-lOSMesa`, since this
milestone uses OSMesa's API surface directly, not the EGL/GL split)
dynamic link against the Mesa build already produced by
`neoos-mesa/build.sh`.

`wmclient.c`, `wmdemo.c`, `taskbar.c`, `startmenu.c` keep their
existing `x86_64-elf-gcc`/`libneoos`/`crt0.o` build entirely
unchanged — only `WM.ELF` moves.

### 4. Disk image: permanent runtime library staging

`disk.img` (`NeoOS/Makefile`, currently `dd ... bs=1M count=32`) grows
to a fixed larger size — sized to comfortably fit the existing test
fixtures plus `libOSMesa.so.8`/`libglapi.so.0`/`libstdc++.so.6`/
`libgcc_s.so.1`/`libc.so`/`ld-musl-x86_64.so.1` (roughly 16MB
stripped, per sub-project 1's measurements) at `/lib`, with headroom —
the exact number is an implementation-plan detail (measured against
real staged sizes, not guessed here). This staging step moves from
this milestone's own ad hoc `mcopy` commands into the disk image's
standard build rule, so every disk image — every gauntlet run, every
`wm-*` target, every real boot — carries these libraries
unconditionally, the same way `libneoos`/musl's own static runtime is
always present today.

### 5. Error handling

`renderer_init`'s failure (OSMesa context creation, `MakeCurrent`, or
the glass GLSL program's compile/link all failing) is a **hard startup
failure**: `wm.c` logs the specific failure and exits non-zero, the
same severity class as today's `wm_connect`/socket-setup failures.
There is no raw-pointer drawing path preserved as a fallback — one
existed today only for the glass pass specifically (glass surfaces
degrade to opaque windows if `ZB_open` fails), because glass was the
*only* thing routed through GL. Once every pixel routes through Mesa,
a "fallback" would mean maintaining a second, parallel rendering
implementation for a failure mode (a working Mesa runtime environment)
that Testing (below) already verifies unconditionally on every image
build — the cost of keeping that path alive permanently outweighs the
value of degrading gracefully from a misconfiguration that shouldn't
reach a shipped image.

Mid-run GL errors (a `glGetError()` after a draw call, say) are logged
and the frame continues — matching today's precedent of logging and
continuing past a single bad `WM_COMMIT` rather than tearing down the
whole compositor over one client's malformed buffer.

## Testing

- `tools/gauntlet.sh 15 3`, zero retries — same standard as
  sub-projects 1 and 2. This milestone touches userland build/link
  configuration and disk layout, not kernel code, but `wm` is exercised
  by several gauntlet-adjacent targets (`wm`, `wm-glass`, `wm-cursor`,
  `wm-cursor-fallback`, `wm-taskbar`), so a regression here is a real
  signal.
- Each of `wm`, `wm-glass`, `wm-cursor`, `wm-cursor-fallback`,
  `wm-taskbar` re-run and passing unchanged is this milestone's
  primary functional proof — same log markers as today
  (`[wm] glass: built-in shaders loaded`, etc.), now produced by the
  Mesa-backed `renderer.c` instead of TinyGL.
- A screenshot comparison of the glass-lensing effect
  (`tools/screenshot.sh`) against a saved pre-migration reference
  image, to confirm the GLSL rewrite preserves the shader's visible
  effect (refraction + tint), not just "it compiles and runs."
- A boot-time check that `renderer_init` failure actually produces the
  documented hard-failure log line and non-zero exit, exercised by
  temporarily pointing `LIBGL_DRIVERS_PATH`-equivalent OSMesa lookup
  at a broken path in one throwaway test run (implementation-plan
  detail: OSMesa is statically-resolvable via `dlopen` internals the
  same way `swrast_dri.so` is for EGL, or fully linked-in — the exact
  failure trigger is confirmed during planning, not guessed here).

## Known limitations carried forward

None beyond what's already recorded in `docs/project-goal.md`'s
Status section for sub-projects 1 and 2 (Vulkan/`llvmpipe`/hardware
acceleration remain out of scope; `softpipe` is the only Gallium pipe
driver in use).

## Deviations found during implementation

- **`OSMESA_ARGB` vs `OSMESA_BGRA` byte-order mapping was backwards**
  in this spec's own Global Constraints. The claim ("`OSMESA_ARGB`
  maps to `PIPE_FORMAT_B8G8R8A8_UNORM` on little-endian") had the two
  cases of `neoos-mesa/upstream/src/gallium/frontends/osmesa/osmesa.c`'s
  format table swapped: on little-endian, `OSMESA_ARGB` actually maps
  to `PIPE_FORMAT_A8R8G8B8_UNORM` (memory bytes low→high: A,R,G,B) and
  `OSMESA_BGRA` maps to `PIPE_FORMAT_B8G8R8A8_UNORM` (memory bytes
  low→high: B,G,R,A — the layout `back` actually needs). Caught by
  `renderer_test.c`'s own fill/blit checks failing with
  byte-reversed-looking pixel values on first boot; confirmed against
  the real Mesa source before fixing. `renderer_init` uses
  `OSMESA_BGRA`, not `OSMESA_ARGB`.
- **`glViewport`'s window coordinates are always bottom-up**, per the
  GL spec, regardless of `OSMesaPixelStore(OSMESA_Y_UP, GL_FALSE)` --
  Y_UP only affects how the color buffer itself is read back/written,
  not `glViewport`'s own coordinate convention. Every `renderer.c`
  function except `renderer_draw_glass` is unaffected because it
  always uses the *full* `(0,0,g_w,g_h)` viewport (identical bottom-up
  or top-down) and reaches window coordinates through the
  top-down-remapped `glOrtho(0,w,h,0,-1,1)` matrix. `renderer_draw_glass`
  narrows the viewport to `(x,y,w,h)` AND feeds raw `[-1,1]` NDC
  vertices (bypassing `glOrtho` entirely), so it alone must flip y by
  hand: `glViewport(x, g_h-y-h, w, h)`. Caught by the glass smoke-test
  check landing at the wrong (mirrored) row; verified by direct pixel
  inspection before fixing.
- **OSMesa's software rasterizer does not write `target` synchronously
  with a draw call.** Every `renderer.c` entry point that touches
  pixels calls `glFinish()` before returning (matching
  `neoos-mesa/test/triangle_test.c`'s own pattern from sub-project 1),
  since callers (`wm.c`) read/copy `back` immediately after each call
  returns. Not called out as a risk anywhere in this spec's Error
  Handling or Testing sections; caught by the first boot of
  `renderer_test.c` reading back all-zero pixels.
- **`GL_GLEXT_PROTOTYPES` must be defined before including `GL/gl.h`**
  for the GL 2.0+ shader entry points (`glCreateShader` etc.) to get
  real prototypes rather than K&R-style implicit declarations. Not a
  link error (the symbols are real, per this spec's own `nm -D`
  verification) but worth avoiding; a one-line addition to
  `renderer.c`'s top-of-file includes.
- **The visual glass-effect check (Task 5 Step 2) could not use a
  saved pre-migration reference screenshot** as this spec's Testing
  section proposed — none existed for the specific two-window
  (plain + glass) composition needed to make the lensing visible
  (`wmdemo.c`'s glass-demo client deliberately fills its own content
  with an opaque flat colour; only the undecorated ring around it,
  sized by `BORDER`/`TITLE_H`, ever shows the shaded backdrop). The
  implementation plan's own Task 5 Step 2 already downgraded this to
  a qualitative human/agent visual check rather than a pixel-diff
  gate, which is what was actually done: a zoomed crop of the glass
  window's ring confirms a smoothly blended, cool-tinted rendering of
  the backdrop window behind it, distinct from a flat copy.
- **`WM.ELF`'s new dynamic-linking startup cost races `wmdemo`/
  `taskbar`'s connection attempt** — unrelated to GL/rendering
  correctness, but a real regression risk in the `wm`/`wm-glass`/
  `wm-cursor`/`wm-cursor-fallback`/`wm-taskbar`/`wm-shot`
  dev-convenience Makefile targets once `WM.ELF` became a
  dynamically-linked binary loading ~16.5MB of Mesa runtime `.so`s off
  NeoOS's ATA-PIO disk driver before reaching `listen()`. `wm.c`'s own
  existing listen-before-heavy-setup ordering (predating this
  migration) only helps with slowness *inside* `main()`; it cannot
  help with the new cost, which is entirely in the dynamic linker,
  before `main()` runs at all. Fixed outside `neoos-wm` entirely: a
  new `wmwait` userland binary (`NeoOS/userland/wmwait.c`, a bare
  5-second `nanosleep`, built with the existing freestanding
  toolchain) inserted as a `wait` inittab entry between spawning
  `wm.nex` and spawning/waiting on any wm client, across all six
  affected Makefile targets. `wmclient.c`/`wmdemo.c`/`taskbar.c` (wire
  protocol/client code) were deliberately left untouched, matching
  this spec's Non-goals.
- **`wm-taskbar`'s Start-menu-click failure is a pre-existing flake,
  not a regression.** Investigated by building the pre-migration
  TinyGL-linked `WM.ELF` in a throwaway git worktree at the last
  commit before this milestone and re-running the same
  click-injection flow against it: it failed identically ("start menu
  never opened"). Out of this milestone's scope to fix.
