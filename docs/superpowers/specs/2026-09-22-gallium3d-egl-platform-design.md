# Gallium3D bring-up sub-project 2: a NeoOS EGL platform for neoos-wm

## Context

Sub-project 1 (`docs/superpowers/specs/2026-09-22-gallium3d-osmesa-bringup-design.md`)
proved Mesa's Gallium3D runs on NeoOS at all: OSMesa + the `softpipe`
pipe driver render a real OpenGL triangle off-screen, cross-compiled
with the hosted `x86_64-neoos-linux-musl` toolchain. That sub-project
deliberately produced nothing an application could use — OSMesa's
context-creation API is Mesa-specific, not how a real GL application
(written against EGL) creates a context or gets pixels on screen.

This sub-project closes that gap: a real NeoOS **EGL platform**, so a
standard `eglGetPlatformDisplay`/`eglCreateWindowSurface`/
`eglSwapBuffers` application renders into an actual `neoos-wm` window,
composited on screen like any other client.

Decomposition agreed with the user before this spec was written:

1. Sub-project 1 (done): OSMesa/softpipe bring-up, off-screen proof of
   life.
2. **This spec**: the EGL platform itself, proved by a standalone test
   client that renders a real GL window through `neoos-wm` — the
   compositor itself is untouched.
3. Next (not yet brainstormed): swap `neoos-wm`'s own internal
   TinyGL usage (the glass-lensing shader pass) onto this stack —
   reviving `docs/superpowers/specs/2026-09-22-wm-tinygl-everywhere-design.md`,
   whose actual blocker (TinyGL's `glBlendFunc` having no alpha
   factors) is exactly what real Mesa fixes. `wm` is a different kind
   of EGL consumer (compositing into its own back buffer, not a window
   on someone else's desktop), so it gets its own design pass.

**`neoos-tinygl` is explicitly out of scope and untouched.** The user
intends to keep writing TinyGL applications directly against it later;
nothing in this sub-project (or sub-project 3) removes, deprecates, or
repurposes that library or its repo. This sub-project adds a second,
independent rendering path (real Mesa/Gallium via EGL) alongside
TinyGL, not a replacement for it.

## Goals

- A new NeoOS EGL platform, `platform_neoos.c`, added to Mesa's EGL/
  DRI2 frontend (`neoos-mesa/upstream/src/egl/drivers/dri2/`),
  registered as a new Meson `platforms` option value (`neoos`)
  alongside Mesa's existing `x11`/`wayland`/`surfaceless`/etc.
- The platform uses Mesa's existing **generic swrast loader path**
  (`__DRIswrastLoaderExtension`'s `putImage`/`getImage`, already
  implemented driver-independently as `dri2_put_image`/`dri2_get_image`
  in `egl_dri2.c`) — the same mechanism X11-without-DRI and software
  Wayland already use for "no real GPU, present via callback"
  rendering. No DMA-BUF, no GEM, no zero-copy buffer sharing: NeoOS has
  none of those, and swrast's plain-copy callbacks need none of them.
- `eglGetPlatformDisplay(EGL_PLATFORM_NEOOS_MESA, wm_conn*, attribs)`
  takes an already-connected `struct wm_conn*` (from `neoos-wm`'s
  existing `wmclient.h`) as the native display. Explicit platform
  selection by the application, not `eglGetDisplay`'s auto-detection
  heuristic.
- `eglCreateWindowSurface`'s native-window argument is a small
  NeoOS-defined handle wrapping the surface id `wm_create_window`/
  `wm_create_glass_window`/`wm_create_surface_ex` already returns.
- On `eglSwapBuffers`, the platform's `putImage` callback copies
  Mesa's rendered buffer directly into that window's existing
  `wm_pixels()` memfd-backed buffer, then calls `wm_damage()` +
  `wm_commit()` — reusing `neoos-wm`'s existing client protocol
  exactly as any hand-written client already does, not inventing a
  second buffer-sharing mechanism.
- Vendored Mesa's own build files that must change to add the
  platform (`meson_options.txt`'s fixed `platforms` enum,
  `src/egl/meson.build`'s platform dispatch, `egldisplay.c`'s platform
  enum/name table) are patched via real unified diffs at
  `neoos-mesa/patches/mesa-22.3.5/*.diff`, applied by `build.sh` before
  `meson setup` — the same mechanism `neoos-hosted-gcc` already
  established for patching vendored musl. `platform_neoos.c` itself,
  being a wholly new file, is copied in directly by `build.sh` rather
  than expressed as a diff, matching how that same milestone's
  `neoos_syscall.c` was handled. `upstream/` stays a pristine,
  resettable submodule.
- Meson build gains `-Dplatforms=neoos` and real `libEGL.so`/`libGL.so`
  targets (`-Dopengl=true -Dgles1=false -Dgles2=false`, keeping scope
  to desktop GL) alongside sub-project 1's still-working
  `-Dosmesa=true`.
- A new standalone test client (`neoos-mesa/test/egl_triangle_client.c`,
  the same category of thing as `neoos-wm`'s existing `wmdemo.c`)
  connects to the compositor, creates a real wm window, gets an EGL
  context on it through the new platform, draws a real GL triangle,
  and calls `eglSwapBuffers`. Proof of done: `tools/screenshot.sh`
  captures the actual composited screen, and a pixel-sampling check
  (matching `wm-glass`'s own screenshot-check standard) confirms red
  pixels at the expected on-screen location.

## Non-goals

- Any change to `neoos-wm` itself (`wm.c`, its TinyGL glass-shader
  usage, its rendering pipeline). The compositor composites the new
  test client's window the same way it composites any other client's
  — that's sub-project 3.
- Any change to, or removal of, `neoos-tinygl`. It remains a fully
  independent, working library for direct use.
- `eglGetDisplay`'s native-display auto-detection heuristic
  (`_eglNativePlatformDetectNativeDisplay`). The application always
  selects the NeoOS platform explicitly via `eglGetPlatformDisplay`.
- GLES (`gles1`/`gles2`) support — desktop GL only, matching sub-project
  1's scope.
- Resize handling, multiple windows per EGL display beyond what the
  swrast loader path already gives for free, or any other polish
  beyond what the proof-of-done test client needs.
- `llvmpipe`, Vulkan, or any pipe driver beyond `softpipe` — unchanged
  from sub-project 1.

## Design

### 1. Patch mechanism

`neoos-mesa/patches/mesa-22.3.5/` holds unified diffs (`diff -u`
against a clean `mesa-22.3.5` checkout) for each existing Mesa file
this sub-project modifies:
- `meson_options.txt` — adds `'neoos'` to the `platforms` option's
  allowed-choices list.
- `src/egl/meson.build` — adds a `with_platform_neoos` boolean
  (derived the same way `with_platform_x11`/etc already are from the
  `platforms` option) gating compilation of `platform_neoos.c`.
- `src/egl/main/egldisplay.c` — adds `_EGL_PLATFORM_NEOOS` to the
  platform enum and its `"neoos"` name-table entry, and a
  `eglGetPlatformDisplay` case (mirroring `_EGL_PLATFORM_SURFACELESS`'s
  handling) that accepts a `struct wm_conn*` as the native display
  without attempting to validate it against any auto-detection
  heuristic.

`build.sh` applies these with `patch -p1` (or `git apply`) against
`upstream/` before `meson setup`, and copies
`platform_neoos.c`/`platform_neoos.h` (the wholly new files) into
`upstream/src/egl/drivers/dri2/` the same way. A `regen-patches.sh`
script, matching `neoos-hosted-gcc`'s own, re-diffs against a clean
checkout whenever the patched files change — the patches' source of
truth is the working tree state at authoring time, not hand-maintained
diff text.

### 2. `platform_neoos.c`

Structured like `platform_surfaceless.c` (`~400` lines) but wired to
the swrast loader extension instead of the `__DRIimage` buffer-sharing
one `platform_surfaceless.c` itself uses for pbuffers. Concretely:

```c
// platform_neoos.c -- sketch of the pieces that differ from
// platform_surfaceless.c's structure.

struct dri2_neoos_display {
   struct wm_conn *wm;
};

struct dri2_neoos_surface {
   int32_t wm_surface_id;   // from wm_create_window/_ex
};

// eglGetPlatformDisplay(EGL_PLATFORM_NEOOS_MESA, wm_conn*, ...) lands
// here via egl_dri2.c's platform dispatch table.
static _EGLDisplay *
neoos_add_configs_for_visuals(...);   // one XRGB8888 config, matching
                                       // wm's own WM_FORMAT_XRGB8888

// The actual bridge to neoos-wm: called from dri2_put_image (the
// generic swrast loader glue already in egl_dri2.c) whenever the
// application calls eglSwapBuffers.
static void
neoos_put_image(__DRIdrawable *draw, int op,
                 int x, int y, int w, int h,
                 char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_neoos_surface *neoos_surf = ...;
   struct wm_conn *wm = ...;

   // wm_pixels() is already the exact memfd-backed buffer wm expects
   // committed content in -- no separate copy/import step.
   memcpy(wm_pixels(wm), data, (size_t)w * h * 4);
   wm_damage(wm, x, y, w, h);
   wm_commit(wm);
}
```

`getImage` (read-back) is implemented via the mirror-image `memcpy`
out of `wm_pixels()`, for interface completeness — EGL's swrast loader
extension requires both callbacks to be present even though a window
surface's normal draw path only exercises `putImage`.

### 3. Native display/window types

`EGL_PLATFORM_NEOOS_MESA` is a new enum value NeoOS defines in its own
EGL headers (installed alongside `libEGL.so`, the way `EGL_PLATFORM_X11_KHR`
etc. are defined in `eglplatform.h`/vendor extension headers). The
native display parameter to `eglGetPlatformDisplay` is a
`struct wm_conn *`, already owned and connected by the calling
application (via `neoos-wm`'s existing `wm_connect()`) — the platform
never opens or closes the compositor connection itself, matching how
X11's platform takes an already-open `Display*` rather than opening
its own.

The native window parameter to `eglCreateWindowSurface` is the
`int32_t` surface id `wm_create_window`/`wm_create_glass_window`/
`wm_create_surface_ex` already returns, reinterpret-cast the same
integer-handle way X11's `Window` (an XID, also an integer) is passed.
No new handle type or wrapper struct crosses the application/Mesa
boundary — this keeps the platform's public contract to types
`wmclient.h` already defines.

### 4. Build configuration

`neoos-mesa/build.sh` gains, after applying the patches from section 1
and before `meson setup`:

```
-Dplatforms=neoos
-Dopengl=true
-Dgles1=false
-Dgles2=false
```

alongside the unchanged `-Dosmesa=true -Dgallium-drivers=swrast
-Dllvm=disabled -Dshader-cache=disabled` from sub-project 1.
`libEGL.so`/`libGL.so` become new build products, verified the same
way sub-project 1 verified `libOSMesa.so` (symbol presence via `nm
-D`, headers landing where the test client expects them).

The platform needs `neoos-wm`'s `wmclient.h` at compile time
(`platform_neoos.c` calls `wm_connect`/`wm_create_window`/`wm_pixels`/
`wm_damage`/`wm_commit` directly) — `build.sh` passes
`-I../neoos-wm` (or copies the single header into
`neoos-mesa/third_party/`, decided during implementation based on
which keeps the Meson cross-file's include handling simplest) and
links the final `libEGL.so` against `neoos-wm`'s client library object
file(s), the same way any other wm client links `wmclient.c`/`.o` in.

### 5. Test client and verification

`neoos-mesa/test/egl_triangle_client.c`:

1. `wm_connect()`.
2. `eglGetPlatformDisplay(EGL_PLATFORM_NEOOS_MESA, wm, NULL)` →
   `eglInitialize` → `eglChooseConfig` (the single XRGB8888 config
   section 2 registers).
3. `wm_create_window(wm, W, H, "egl-triangle")` → `eglCreateWindowSurface`
   on that surface id.
4. `eglCreateContext`/`eglMakeCurrent`.
5. The same fixed-function triangle draw as sub-project 1's
   `triangle_test.c` (`glClear`/`glBegin(GL_TRIANGLES)`/...).
6. `eglSwapBuffers` — this is what exercises `neoos_put_image` and
   actually pushes pixels to the compositor.
7. A short `wm_poll_event`/sleep loop holding the window open long
   enough for `tools/screenshot.sh`'s capture window, then a clean
   `wm_disconnect`.

Verification: boot NeoOS with this client's `.nex` in the normal GUI
boot path (the same `-vga std` path `neoos-wm`'s own end-to-end tests
already use, per `docs/superpowers/specs/2026-09-20-wm-glass-lensing-design.md`'s
own screenshot-check precedent), run `tools/screenshot.sh`, and a
small Python/PIL pixel-sampling check confirms red at the triangle's
expected on-screen coordinates and the window's background color
elsewhere within the window's bounds — not just "a screenshot was
produced."

### 6. Error handling

The platform introduces no NeoOS-specific error semantics: EGL's
standard error codes (`EGL_NOT_INITIALIZED`, `EGL_BAD_DISPLAY`,
`EGL_BAD_NATIVE_WINDOW`) cover every failure this platform can
produce, matching every other Mesa EGL platform's contract. If
`wm_connect()` itself fails (no compositor running), that surfaces as
`eglGetPlatformDisplay` returning `EGL_NO_DISPLAY` — the standard EGL
shape for "no display available," not a NeoOS-specific path an
application needs to special-case.

## Testing

- `tools/gauntlet.sh 15 3`, zero retries — this sub-project touches no
  kernel code; a regression would be a real signal, same standard as
  sub-project 1.
- The screenshot-verified on-screen triangle (section 5) is this
  sub-project's actual proof of done, run as its own ad hoc GUI-mode
  boot check (matching `wm-glass`'s own precedent) rather than folded
  into the headless regression suite, since it needs the `-vga std`
  boot path rather than `-display none`.
- Sub-project 1's existing OSMesa proof-of-life
  (`triangle_test.nex`) is re-run unchanged to confirm adding the
  `neoos` platform didn't regress the off-screen path — both link
  against the same `softpipe`/Gallium code underneath.

## Known limitation carried forward

`neoos-wm`'s own internal rendering (the glass-lensing shader pass,
currently TinyGL) is untouched by this sub-project. Swapping it onto
this new EGL platform — reviving
`docs/superpowers/specs/2026-09-22-wm-tinygl-everywhere-design.md` on
a backend that actually has working alpha blending — is sub-project
3, to be brainstormed separately once this platform is implemented and
verified.
