# Project goal: Gallium3D on NeoOS

As of 2026-09-22, the project's goal is to get **Gallium3D working on
NeoOS**. This supersedes any narrower graphics-stack milestone that
came before it, including the abandoned
`docs/superpowers/specs/2026-09-22-wm-tinygl-everywhere-design.md`
(routing `neoos-wm`'s compositor through TinyGL) -- that work is
half-baked and not being continued; this document is what replaces it
as the north star for the graphics stack.

## How this goal was reached

A request to route all of `neoos-wm`'s rendering through TinyGL (so a
later swap to real OpenGL/hardware acceleration would be a backend
change, not a rewrite) led to a design spec
(`2026-09-22-wm-tinygl-everywhere-design.md`). Self-review before
implementation found the vendored `neoos-tinygl` fork's `glBlendFunc`
doesn't implement alpha-based blend factors at all -- only
`GL_ONE`/`GL_ZERO`/`GL_ONE_MINUS_SRC_COLOR`, with no per-pixel alpha
tracked through the rasterizer. That's a real gap in TinyGL itself,
not something the compositor's own code can work around.

That prompted the real question: NeoOS has no GPU driver of any kind
today (no DRM/KMS, no virtio-gpu -- confirmed by searching the whole
NeoOS repo and every sibling `neoos-*` repo). A research spike into
what real hardware-accelerated 3D would take found:

- QEMU on the reference host (10.2.1) does support `virtio-gpu-gl-pci`
  (virgl-capable) as a device, and NeoOS already has a clean,
  device-agnostic virtio transport layer
  (`kernel/drivers/virtio/virtio.c`), proven by the working virtio-net
  driver -- real, reusable infrastructure, not a from-scratch job on
  the transport side.
- But `VIRTIO_GPU_CMD_SUBMIT_3D`'s command stream is not a simple
  public wire protocol -- it's explicitly "modelled around Gallium
  driver commands... to make it easier to implement a Mesa driver."
  A guest driver has to speak Gallium3D's own internal pipe-driver
  shape, including compiling shaders to **TGSI** (Gallium's internal
  shader IR) rather than GLSL or SPIR-V. NeoOS's TinyGL uses its own
  custom SPIR-V-subset VM, which has no TGSI representation at all.
- There is no independent specification document for the exact
  command encoding beyond Mesa's own source
  (`src/gallium/drivers/virgl/`) and virglrenderer's decoder --
  understanding it means reading that source directly, not following
  a written protocol.

**Conclusion of the spike:** hooking into virgl as a client (treating
Gallium3D as an opaque host-side service reached over virtio-gpu) is
not a scoped, near-term project -- it is comparable to implementing a
real chunk of a Gallium3D driver from scratch, reverse-engineered from
Mesa's own C source, with the added twist that TinyGL's shader IR
doesn't match Gallium's. Given that, the decision was made to stop
treating "GPU acceleration" as a side effect of some other milestone
(taskbar polish, compositor cleanup) and instead make it the project's
direct, named goal: get Gallium3D itself running on NeoOS, not merely
talking to a host's Gallium3D over virtio-gpu.

## What "Gallium3D working on NeoOS" means

Not yet scoped into a design spec or implementation plan -- this
document records the goal and the research that led to it; the actual
architecture (what pieces of Gallium3D, which pipe driver target,
whether this still goes through virtio-gpu/virglrenderer as the pipe
driver's backend or targets something else, what NeoOS-side subsystems
it depends on: shared memory primitives, threading, `dlopen` for
Mesa's runtime pieces, etc.) is future brainstorming work, to be
brainstormed properly (`docs/superpowers/specs/` design spec +
`docs/superpowers/plans/` implementation plan) before any code is
written.

## Status of prior graphics-stack work

None of this invalidates what's already shipped and tested:
- `neoos-tinygl`'s software rasterizer and SPIR-V shader stage
  (`docs/superpowers/specs/2026-09-20-tinygl-spirv-shader-stage-design.md`)
- Compositor-level glass lensing
  (`docs/superpowers/specs/2026-09-20-wm-glass-lensing-design.md`)
- The taskbar/start menu milestone
  (`docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md`)

These remain the working desktop stack. Gallium3D is the new target
for what comes *after* them, not a replacement for what already works.

## Status

**Sub-project 1 (OSMesa/softpipe bring-up) complete**, as of
2026-09-22 -- see
`docs/superpowers/specs/2026-09-22-gallium3d-osmesa-bringup-design.md`
and `docs/superpowers/plans/2026-09-22-gallium3d-osmesa-bringup.md`.
Mesa's OSMesa target (real `glBegin`/`glVertex2f`/GLSL-capable OpenGL
API, `softpipe` Gallium pipe driver) cross-compiles for NeoOS via the
hosted `x86_64-neoos-linux-musl` toolchain and Meson, in a new
`neoos-mesa` repo, and a real triangle renders off-screen with correct
pixel output (`center pixel: r=255 g=0 b=0`, `corner pixel: r=0 g=0
b=0`), verified under headless QEMU. Full 15/15-zero-retry gauntlet
regression stays green.

**Sub-project 2 (NeoOS EGL platform) complete**, as of 2026-09-22 --
see
`docs/superpowers/specs/2026-09-22-gallium3d-egl-platform-design.md`
and `docs/superpowers/plans/2026-09-22-gallium3d-egl-platform.md`. A
real `EGL_PLATFORM_NEOOS_MESA` platform (`platform_neoos.c`, modeled
on `platform_x11.c`'s swrast-loader path) is now wired all the way
through Mesa's EGL/DRI2 frontend, so a standard
`eglGetPlatformDisplay`/`eglCreateWindowSurface`/`eglSwapBuffers`
application -- not OSMesa's private off-screen API -- renders a real
GL triangle into an actual `neoos-wm` window, composited on screen and
verified by `tools/screenshot.sh` (pure red `(255, 0, 0)` at the
triangle's actual on-screen location). Full 15/15-zero-retry gauntlet
regression stays green, and sub-project 1's off-screen OSMesa path is
unaffected.

Two new sibling repos/dependencies this pulled in, beyond sub-project
1's zlib/expat:
- `neoos-libdrm`: Mesa's `with_dri2` hard-requires libdrm at configure
  time whenever `-Degl=enabled`, regardless of real DRM/KMS hardware.
  NeoOS has none, so this is a core-only libdrm build (every
  hardware-specific backend disabled) -- device enumeration correctly
  finds no `/dev/dri` and reports zero devices at runtime, which is
  real behavior, not emulation.
- A hand-assembled `libGL.so`: upstream Mesa's meson build only ever
  produces `libGL.so` via `src/glx/meson.build` (the full GLX/X11
  stack), inapplicable with no X11. `neoos-mesa/build.sh` instead
  links Mesa's own always-built `libglapi_bridge.a` (pure per-symbol
  dispatch thunks, zero GLX code) against the same shared
  `libglapi.so.0` `libEGL.so` uses, so `eglMakeCurrent`'s internal
  `_glapi_set_dispatch()` call is visible to both. No Mesa source
  patched for this.

Any future NeoOS EGL/GL application needs, at `/lib` on the disk
image: everything sub-project 1 already listed
(`libOSMesa.so.8`/`libglapi.so.0`/`libstdc++.so.6`/`libgcc_s.so.1`/
`libc.so`/`ld-musl-x86_64.so.1`), plus `libEGL.so.1`, `libGL.so.1`,
and `/lib/dri/swrast_dri.so` (`dlopen`'d at runtime by
`dri2_load_driver_swrast` -- the app must `setenv("LIBGL_DRIVERS_PATH",
"/lib/dri", 1)` before `eglInitialize`, since Mesa's compiled-in
default is an absolute build-host path). Strip all of these before
staging; unstripped `swrast_dri.so` alone is ~89MB, well past a 32MB
test disk.

**Sub-project 3 (routing `neoos-wm`'s own rendering through real Mesa)
complete**, as of 2026-09-22 -- see
`docs/superpowers/specs/2026-09-22-wm-mesa-everywhere-design.md` and
`docs/superpowers/plans/2026-09-22-wm-mesa-everywhere.md`. `wm.c`
renders entirely through a new `renderer.c`/`renderer.h` seam backed
by real Mesa (OSMesa) -- desktop fill, window border/title, content
blit, and cursor are now real `glBegin`/`glVertex2f`/`glTexImage2D`
draw calls, and the glass-lensing pass is a real compiled GLSL
program (`glCreateShader`/`glCompileShader`/`glLinkProgram`), its
semantics recovered exactly from the still-present
`glass_vertex.spvasm`/`glass_fragment.spvasm`. Both of `wm.c`'s old
hand-rolled per-pixel alpha-blend loops (glass intensity, ARGB8888
content) are deleted, replaced by real `glBlendFunc` state
(`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` for straight-alpha content,
`GL_ONE, GL_ONE_MINUS_SRC_ALPHA` for the cursor's premultiplied
alpha). `neoos-tinygl` is no longer a `wm.c` dependency, though it
remains available, untouched, for direct application use later.
`WM.ELF` itself moves off the freestanding, statically-linked
`x86_64-elf-gcc` build onto the hosted `x86_64-neoos-linux-musl`
toolchain with real dynamic linking, matching `neoos-mesa`'s own test
clients -- `wm.nex` is now a dynamically-linked binary requiring the
same Mesa runtime `.so`s sub-project 1 introduced, staged
permanently on every disk image (not opt-in, since `wm` runs on
every real desktop boot). Verified via a standalone smoke test
(`neoos-wm/test/renderer_test.c`, seven checks: flat fills, opaque
and alpha-blended blits, premultiplied cursor blend, and an
analytically-computed glass tint), every existing `wm`/`wm-glass`/
`wm-cursor`/`wm-cursor-fallback` end-to-end target, and a screenshot
confirming the glass window's shaded ring visibly lenses/tints the
window behind it. Full 15/15-zero-retry gauntlet regression stays
green.

Two corrections to the design spec's own "verified" facts surfaced
during implementation -- both fixed in code, recorded in the spec's
own deviations section: `OSMESA_ARGB` vs `OSMESA_BGRA`'s byte-order
mapping was backwards (the spec had them swapped relative to Mesa's
actual `osmesa.c` format table), and `glViewport`'s window
coordinates are always bottom-up regardless of `OSMESA_Y_UP`, which
only `renderer_draw_glass` needed to hand-correct (every other
`renderer.c` function reaches window coordinates through the
top-down-remapped `glOrtho` matrix instead of raw NDC). A third,
unrelated fix went into the `wm`/`wm-glass`/`wm-cursor`/
`wm-cursor-fallback`/`wm-taskbar`/`wm-shot` dev-convenience Makefile
targets: `WM.ELF`'s new dynamic-linking startup cost (loading
~16.5MB of Mesa runtime `.so`s off NeoOS's ATA-PIO disk driver) races
`wmdemo`/`taskbar`'s single, un-retried connection attempt; a new
`wmwait` userland binary (`userland/wmwait.c`, a bare 5-second
`nanosleep`) inserted between spawning `wm.nex` and spawning its
client buys the needed head start without touching client code.
(Superseded 2026-09-22: `wmwait` is gone. `wm_connect()` now waits for
the compositor's listener itself (see `docs/stdlib.md`), and the
Start-click "flake" below turned out to be a harness bug: the
`wm-taskbar` wait loop matched the *previous* run's stale serial log,
so the clicks fired before the compositor was reading the mouse.)
`wm-taskbar`'s separate Start-menu-click flake was investigated and
confirmed pre-existing (reproduces identically against the old,
pre-migration TinyGL-linked `WM.ELF`), not a regression from this
work.

The Gallium3D project-goal's three sub-projects are now all complete.
Any further graphics-stack work (e.g. giving `neoos-tinygl`-based
applications a path onto real Mesa, or a second EGL/GL application
beyond the sub-project 2 test client) is a new, not-yet-brainstormed
milestone.
