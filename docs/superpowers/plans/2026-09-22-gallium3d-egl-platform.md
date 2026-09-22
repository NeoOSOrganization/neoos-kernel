# Gallium3D EGL Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a real NeoOS EGL platform to Mesa (`platform_neoos.c`), so a
standard `eglGetPlatformDisplay`/`eglCreateWindowSurface`/`eglSwapBuffers`
application renders a real GL triangle into an actual `neoos-wm` window,
composited on screen — verified by screenshot.

**Architecture:** Mesa's EGL/DRI2 frontend already has a generic **swrast
loader** mechanism (`putImage`/`getImage` callbacks) used by old
X11-without-DRI and software Wayland for exactly this situation ("no real
GPU, present via callback"). `platform_neoos.c` is modeled directly on
`platform_x11.c`'s swrast code path (`swrastCreateDrawable`/
`swrastPutImage`/`swrastGetImage`/`dri2_x11_create_window_surface`/
`dri2_x11_swap_buffers`/`dri2_initialize_x11_swrast`), with every
XCB/X11-protocol call replaced by a call into `neoos-wm`'s existing client
library (`wmclient.h`: `wm_pixels()`/`wm_damage()`/`wm_commit()`). Adding the
platform required patching six existing Mesa files (verified by reading
their real source, not assumed) plus three new files.

**Tech Stack:** Mesa 22.3.5 (already vendored in `neoos-mesa`), the hosted
`x86_64-neoos-linux-musl` toolchain, `neoos-wm`'s `wmclient.h`/`wmclient.c`.

**Spec:** `docs/superpowers/specs/2026-09-22-gallium3d-egl-platform-design.md`

## Global Constraints

- `neoos-tinygl` and `neoos-wm` itself are untouched by this plan. Every
  file this plan modifies lives in `neoos-mesa` (patches to the vendored
  Mesa submodule, or new files); `wmclient.c`/`.h` are only *read from*
  `../neoos-wm`, never modified.
- **Correction found during planning, superseding the design spec's
  Section 1 assumption:** adding the platform does NOT require touching
  `meson_options.txt`'s `platforms` array or adding a `with_platform_neoos`
  boolean to top-level `meson.build`. Verified by reading the real source:
  `platform_device.c`/`platform_surfaceless.c` already compile
  unconditionally inside `src/egl/meson.build`'s `if with_dri2` block,
  regardless of the `platforms` option's contents — `platform_neoos.c`
  joins that same unconditional list. `with_dri`/`with_dri2`/`with_egl`
  only need `-Degl=enabled -Dglx=disabled` (verified against
  `meson.build`'s real `with_dri` derivation: `with_gallium and
  system_has_kms_drm and (_glx=='dri' or _egl=='enabled' or ...)`). This
  is simpler than the spec assumed; the spec gets a deviation note in
  Task 9.
- Every patched file gets a real unified diff at
  `neoos-mesa/patches/mesa-22.3.5/<file>.diff`, applied by `build.sh`
  before `meson setup` — the same mechanism `neoos-hosted-gcc` already
  established. New files are copied in directly by `build.sh`, not
  expressed as diffs.
- `EGL_PLATFORM_NEOOS_MESA` = `0x3400` (verified: no collision anywhere in
  `include/EGL/eglext.h`).
- The native window handle is `struct NeoosEGLWindow { int32_t surface_id;
  uint32_t width, height; }` (not a bare surface id) — `wmclient.h` has no
  "query an existing surface's size" accessor, and adding one would touch
  `neoos-wm` (out of scope), so the caller (our test client) bundles the
  size it already knows from its own `wm_create_window` call. This keeps
  the platform's `wmclient.h` usage read-only.
- Window surfaces only (`EGL_WINDOW_BIT`) — no pixmap/pbuffer support via
  this platform. Fixed-size windows only — no resize handling, matching
  the design spec's Non-goals.

---

## Task 1: Meson wiring for `with_dri`/`with_egl`, no platform code yet

**Files:**
- Modify: `neoos-mesa/build.sh`

**Interfaces:**
- Consumes: nothing new.
- Produces: a Mesa build with `libEGL.so`/`libGL.so` real targets
  (via the existing `surfaceless`/`device` platforms, no NeoOS platform
  yet) — later tasks add `platform_neoos.c` on top of this working base.

- [ ] **Step 1: Change the Meson options**

```bash
cd /home/neo/projects/personal/neoos-mesa
```

In `build.sh`, replace:

```
    -Dglx=disabled \
    -Degl=disabled \
```

with:

```
    -Dglx=disabled \
    -Degl=enabled \
    -Dgles1=false \
    -Dgles2=false \
```

(`-Dosmesa=true -Dgallium-drivers=swrast -Dllvm=disabled
-Dshader-cache=disabled -Dplatforms=` and everything else from
sub-project 1 stays unchanged.)

- [ ] **Step 2: Rebuild and verify `libEGL.so`/`libGL.so` now exist**

```bash
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
./build.sh 2>&1 | tail -60
ls build-output/lib/libEGL.so* build-output/lib/libGL.so* 2>&1
```

Expected: both exist (Meson's configure summary should now show `EGL:
yes`, and the `swrast` `softpipe` Gallium driver unchanged from
sub-project 1). If the build instead errors, read the actual Meson
error before assuming this plan's `with_dri` derivation reasoning
(Global Constraints) is wrong — recheck against `meson.build`'s real
source at that error's line number first.

- [ ] **Step 3: Commit**

```bash
git add build.sh
git commit -q -m "build: enable EGL (real libEGL.so/libGL.so, no NeoOS platform yet)"
```

---

## Task 2: Patch infrastructure and `regen-patches.sh`

**Files:**
- Create: `neoos-mesa/patches/mesa-22.3.5/` (directory)
- Create: `neoos-mesa/patches/mesa-22.3.5/regen-patches.sh`
- Modify: `neoos-mesa/build.sh` (apply-patches step, before `meson setup`)

**Interfaces:**
- Consumes: nothing.
- Produces: a `patches/mesa-22.3.5/*.diff` application step every later
  task's patches land in.

- [ ] **Step 1: Write `regen-patches.sh`**

```bash
mkdir -p /home/neo/projects/personal/neoos-mesa/patches/mesa-22.3.5
cat > /home/neo/projects/personal/neoos-mesa/patches/mesa-22.3.5/regen-patches.sh <<'EOF'
#!/bin/bash
# Re-diffs the patched files in upstream/ against a clean mesa-22.3.5
# checkout, refreshing this directory's *.diff files. Run this after
# editing any patched file directly in upstream/ (during development),
# before committing -- the diffs are the source of truth build.sh
# applies, not upstream/'s own working-tree state.
set -e
cd "$(dirname "$0")/../.."   # neoos-mesa/

CLEAN=$(mktemp -d)
git -C upstream worktree add --detach "$CLEAN/mesa-22.3.5" mesa-22.3.5 >/dev/null

FILES=(
  src/egl/meson.build
  src/egl/drivers/dri2/egl_dri2.h
  src/egl/drivers/dri2/egl_dri2.c
  src/egl/main/egldisplay.h
  src/egl/main/egldisplay.c
  src/egl/main/eglapi.c
)

for f in "${FILES[@]}"; do
  diff -u "$CLEAN/mesa-22.3.5/$f" "upstream/$f" \
    > "patches/mesa-22.3.5/$(echo "$f" | tr / _).diff" || true
  echo "regenerated patches/mesa-22.3.5/$(echo "$f" | tr / _).diff"
done

git -C upstream worktree remove --force "$CLEAN/mesa-22.3.5"
rm -rf "$CLEAN"
EOF
chmod +x /home/neo/projects/personal/neoos-mesa/patches/mesa-22.3.5/regen-patches.sh
```

- [ ] **Step 2: Add the apply-patches step to `build.sh`**

Insert, right after the `if [ ! -f upstream/meson.build ]` check and
before `mkdir -p "$PREFIX"`:

```bash
echo "Resetting upstream/ to pristine mesa-22.3.5 before patching..."
git -C upstream checkout -- . 2>/dev/null || true
git -C upstream clean -fd src/egl/drivers/dri2/platform_neoos.c \
    src/egl/drivers/dri2/platform_neoos.h \
    src/egl/drivers/dri2/wmclient.c \
    src/egl/drivers/dri2/wmclient.h 2>/dev/null || true

for diff in patches/mesa-22.3.5/*.diff; do
    [ -s "$diff" ] || continue
    echo "Applying $diff"
    patch -p1 -d upstream < "$diff"
done

echo "Copying new files (platform_neoos, wmclient) into upstream/..."
cp src-new/platform_neoos.c src-new/platform_neoos.h \
   upstream/src/egl/drivers/dri2/
cp ../neoos-wm/wmclient.c ../neoos-wm/wmclient.h \
   upstream/src/egl/drivers/dri2/
```

(`src-new/` is a new directory in `neoos-mesa` holding `platform_neoos.c`/
`.h` as committed, editable source — Task 4 creates it. `wmclient.c`/`.h`
are copied fresh from `../neoos-wm` on every build, never duplicated as a
committed copy, so they can never drift out of sync with the real
client library.)

- [ ] **Step 3: Verify the reset+no-op-apply step doesn't break the
  Task 1 build (no patches exist yet, so this should be a no-op)**

```bash
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
cd /home/neo/projects/personal/neoos-mesa
mkdir -p src-new
touch src-new/platform_neoos.c src-new/platform_neoos.h   # placeholders for this step only
./build.sh 2>&1 | tail -20
```

Expected: same result as Task 1 Step 2 (no `*.diff` files exist yet, so
nothing is applied; the two placeholder files get copied in but aren't
referenced by any `meson.build` yet, so they're inert). Remove the
placeholders before Task 4 writes the real files.

- [ ] **Step 4: Commit**

```bash
git add patches/mesa-22.3.5/regen-patches.sh build.sh
git commit -q -m "build: patch-application infrastructure for vendored Mesa"
```

---

## Task 3: The six file patches

**Files:**
- Create (via editing a live copy, then diffing): six `.diff` files
  under `neoos-mesa/patches/mesa-22.3.5/`
- Modify (temporarily, to generate the diffs; reset after):
  `neoos-mesa/upstream/src/egl/meson.build`,
  `neoos-mesa/upstream/src/egl/drivers/dri2/egl_dri2.h`,
  `neoos-mesa/upstream/src/egl/drivers/dri2/egl_dri2.c`,
  `neoos-mesa/upstream/src/egl/main/egldisplay.h`,
  `neoos-mesa/upstream/src/egl/main/egldisplay.c`,
  `neoos-mesa/upstream/src/egl/main/eglapi.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `EGL_PLATFORM_NEOOS_MESA` dispatched all the way to
  `dri2_initialize_neoos` (Task 4 supplies that function's body) —
  everything downstream of `eglGetPlatformDisplay` up to the platform
  implementation itself.

- [ ] **Step 1: `src/egl/meson.build` — add the new files to the
  unconditional dri2 file list**

Edit `upstream/src/egl/meson.build`, changing:

```meson
  files_egl += files(
    'drivers/dri2/platform_device.c',
    'drivers/dri2/platform_surfaceless.c',
  )
```

to:

```meson
  files_egl += files(
    'drivers/dri2/platform_device.c',
    'drivers/dri2/platform_surfaceless.c',
    'drivers/dri2/platform_neoos.c',
    'drivers/dri2/wmclient.c',
  )
```

- [ ] **Step 2: `src/egl/drivers/dri2/egl_dri2.h` — surface fields and
  the init-function declaration**

Add two unconditional fields to `struct dri2_egl_surface` (right after
`bool have_fake_front;`, before the `#ifdef HAVE_X11_PLATFORM` block —
deliberately NOT gated behind a `HAVE_NEOOS_PLATFORM` macro, since that
would require the `meson_options.txt` change this plan's Global
Constraints established is unnecessary):

```c
   int32_t neoos_surface_id;   /* neoos-mesa sub-project 2 */
   int neoos_bytes_per_pixel;
```

Add the init-function declaration next to `dri2_initialize_surfaceless`/
`dri2_initialize_device`:

```c
EGLBoolean
dri2_initialize_neoos(_EGLDisplay *disp);
```

- [ ] **Step 3: `src/egl/drivers/dri2/egl_dri2.c` — dispatch case**

In `dri2_initialize`'s switch (the one starting `case
_EGL_PLATFORM_SURFACELESS:`), add:

```c
   case _EGL_PLATFORM_NEOOS:
      ret = dri2_initialize_neoos(disp);
      break;
```

- [ ] **Step 4: `src/egl/main/egldisplay.h` — the platform enum**

Add `_EGL_PLATFORM_NEOOS,` to `enum _egl_platform_type` (right after
`_EGL_PLATFORM_SURFACELESS,`), and declare:

```c
_EGLDisplay*
_eglGetNeoosDisplay(void *native_display,
                    const EGLAttrib *attrib_list);
```

(next to `_eglGetSurfacelessDisplay`'s declaration).

- [ ] **Step 5: `src/egl/main/egldisplay.c` — name table and the
  display-lookup function**

Add `{ _EGL_PLATFORM_NEOOS, "neoos" },` to the `egl_platforms[]` table.

Add, modeled directly on `_eglGetSurfacelessDisplay` but requiring a
non-NULL native display (the opposite validation, since NeoOS's
platform always needs a real `wm_conn*`):

```c
_EGLDisplay*
_eglGetNeoosDisplay(void *native_display,
                    const EGLAttrib *attrib_list)
{
   /* This platform REQUIRES a native display: an already-connected
    * struct wm_conn* from neoos-wm's wm_connect(). */
   if (native_display == NULL) {
      _eglError(EGL_BAD_PARAMETER, "eglGetPlatformDisplay");
      return NULL;
   }

   /* This platform recognizes no display attributes. */
   if (attrib_list != NULL && attrib_list[0] != EGL_NONE) {
      _eglError(EGL_BAD_ATTRIBUTE, "eglGetPlatformDisplay");
      return NULL;
   }

   return _eglFindDisplay(_EGL_PLATFORM_NEOOS, native_display,
                          attrib_list);
}
```

- [ ] **Step 6: `src/egl/main/eglapi.c` — the `EGLenum` dispatch case**

Add `#include "drivers/dri2/platform_neoos.h"` near the top (for
`EGL_PLATFORM_NEOOS_MESA`'s definition), and in
`_eglGetPlatformDisplayCommon`'s switch, add:

```c
   case EGL_PLATFORM_NEOOS_MESA:
      disp = _eglGetNeoosDisplay(native_display, attrib_list);
      break;
```

- [ ] **Step 7: Generate the diffs**

```bash
cd /home/neo/projects/personal/neoos-mesa
rm -f src-new/platform_neoos.c src-new/platform_neoos.h   # remove Task 2's placeholders
patches/mesa-22.3.5/regen-patches.sh
ls -la patches/mesa-22.3.5/*.diff
```

Expected: six non-empty `.diff` files, one per file edited above.

- [ ] **Step 8: Reset `upstream/` back to pristine and re-apply via
  `build.sh` to prove the diffs are self-sufficient**

```bash
git -C upstream checkout -- .
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
./build.sh 2>&1 | tail -40
```

Expected: the patch-apply step (Task 2) applies all six diffs cleanly.
The build itself will fail at this point (no `platform_neoos.c`/`.h`
exist yet — Task 4 adds them) — that failure is expected here; what
this step actually verifies is that `patch -p1` applied every diff
without a `.rej` file.

```bash
find upstream -name '*.rej'
```

Expected: no output.

- [ ] **Step 9: Commit**

```bash
git add patches/mesa-22.3.5/*.diff
git commit -q -m "build: patches wiring EGL_PLATFORM_NEOOS_MESA through Mesa's EGL frontend"
```

---

## Task 4: `platform_neoos.c`/`.h` — the platform implementation

**Files:**
- Create: `neoos-mesa/src-new/platform_neoos.h`
- Create: `neoos-mesa/src-new/platform_neoos.c`

**Interfaces:**
- Consumes: `wmclient.h`'s `wm_pixels`/`wm_stride_px`/`wm_damage`/
  `wm_commit` (copied into the Mesa tree by `build.sh`, Task 2);
  `dri2_egl_surface`'s `neoos_surface_id`/`neoos_bytes_per_pixel` fields
  and `dri2_initialize_neoos`'s declaration (Task 3).
- Produces: `dri2_initialize_neoos`, satisfying Task 3's
  `egl_dri2.c` dispatch case. `EGL_PLATFORM_NEOOS_MESA` and
  `struct NeoosEGLWindow`, consumed by Task 6's test client.

- [ ] **Step 1: `platform_neoos.h`**

```bash
mkdir -p /home/neo/projects/personal/neoos-mesa/src-new
cat > /home/neo/projects/personal/neoos-mesa/src-new/platform_neoos.h <<'EOF'
#ifndef PLATFORM_NEOOS_H
#define PLATFORM_NEOOS_H

#include <EGL/egl.h>
#include <stdint.h>

/* Not a Khronos-registered value -- 0x3400 is verified free of any
 * collision in this Mesa tree's include/EGL/eglext.h. NeoOS-only. */
#define EGL_PLATFORM_NEOOS_MESA 0x3400

/* eglCreateWindowSurface's native-window argument, cast through
 * EGLNativeWindowType. wmclient.h has no "query an existing surface's
 * size" accessor (and adding one would touch neoos-wm, out of scope
 * for this sub-project) -- the caller already knows the size from its
 * own wm_create_window() call, so it bundles it here. Matches how
 * Wayland's EGLNativeWindowType (struct wl_egl_window*) also carries
 * cached size alongside the real surface, rather than X11's bare-XID
 * pattern which relies on being able to query the window manager.
 */
struct NeoosEGLWindow {
   int32_t surface_id;
   uint32_t width;
   uint32_t height;
};

EGLBoolean
dri2_initialize_neoos(_EGLDisplay *disp);

#endif
EOF
```

- [ ] **Step 2: `platform_neoos.c`**

```bash
cat > /home/neo/projects/personal/neoos-mesa/src-new/platform_neoos.c <<'EOF'
/*
 * NeoOS EGL platform: bridges Mesa's generic swrast loader path
 * (putImage/getImage callbacks -- the same mechanism old
 * X11-without-DRI and software Wayland use) into neoos-wm's existing
 * memfd-backed client protocol (wmclient.h). Modeled directly on
 * platform_x11.c's swrast code path (swrastCreateDrawable/
 * swrastPutImage/swrastGetImage/dri2_x11_create_window_surface/
 * dri2_x11_swap_buffers/dri2_initialize_x11_swrast), with every
 * XCB/X11-protocol call replaced by a wmclient.h call.
 */

#include <stdlib.h>
#include <string.h>

#include "egl_dri2.h"
#include "loader.h"
#include "platform_neoos.h"
#include "wmclient.h"

/* ARGB8888 shifts/sizes (red@16, green@8, blue@0, alpha@24, 8 bits
 * each) -- the same values egl_dri2.c's own dri2_pbuffer_visuals[]
 * table uses for its "ARGB8888" entry. wm's WM_FORMAT_XRGB8888 has
 * the identical byte layout; alpha is simply unused for our opaque
 * window surfaces. */
static const int neoos_rgba_shifts[4] = { 16, 8, 0, 24 };
static const unsigned int neoos_rgba_sizes[4] = { 8, 8, 8, 8 };

static void
neoosCreateDrawable(struct dri2_egl_surface *dri2_surf)
{
   dri2_surf->neoos_bytes_per_pixel = 4;
}

static void
neoosGetDrawableInfo(__DRIdrawable *draw,
                     int *x, int *y, int *w, int *h,
                     void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   /* Fixed-size windows only (Non-goals) -- the size recorded at
    * eglCreateWindowSurface time is authoritative, no live query. */
   *x = 0;
   *y = 0;
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static void
neoosPutImage(__DRIdrawable *draw, int op,
             int x, int y, int w, int h,
             char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   struct wm_conn *wm = (struct wm_conn *) dri2_dpy->base.PlatformDisplay;
   uint32_t *dst = wm_pixels(wm);
   uint32_t stride = wm_stride_px(wm);
   int bpp = dri2_surf->neoos_bytes_per_pixel;
   int row;

   if (!dst || bpp <= 0)
      return;

   for (row = 0; row < h; row++) {
      memcpy(&dst[(y + row) * (int) stride + x],
             data + (size_t) row * w * bpp,
             (size_t) w * bpp);
   }

   wm_damage(wm, x, y, w, h);
   wm_commit(wm);
}

static void
neoosGetImage(__DRIdrawable *read,
             int x, int y, int w, int h,
             char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   struct wm_conn *wm = (struct wm_conn *) dri2_dpy->base.PlatformDisplay;
   uint32_t *src = wm_pixels(wm);
   uint32_t stride = wm_stride_px(wm);
   int bpp = dri2_surf->neoos_bytes_per_pixel;
   int row;

   if (!src || bpp <= 0)
      return;

   for (row = 0; row < h; row++) {
      memcpy(data + (size_t) row * w * bpp,
             &src[(y + row) * (int) stride + x],
             (size_t) w * bpp);
   }
}

static const __DRIswrastLoaderExtension neoos_swrast_loader_extension = {
   .base            = { __DRI_SWRAST_LOADER, 3 },
   .getDrawableInfo = neoosGetDrawableInfo,
   .putImage        = neoosPutImage,
   .getImage        = neoosGetImage,
};

static const __DRIextension *neoos_swrast_loader_extensions[] = {
   &neoos_swrast_loader_extension.base,
   &image_lookup_extension.base,
   &use_invalidate.base,
   NULL,
};

static EGLBoolean
dri2_neoos_add_configs_for_visuals(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned int config_count = 0;
   unsigned i;

   for (i = 0; dri2_dpy->driver_configs[i] != NULL; i++) {
      struct dri2_egl_config *dri2_conf =
         dri2_add_config(disp, dri2_dpy->driver_configs[i],
                         config_count + 1, EGL_WINDOW_BIT, NULL,
                         neoos_rgba_shifts, neoos_rgba_sizes);
      if (dri2_conf && dri2_conf->base.ConfigID == config_count + 1)
         config_count++;
   }

   return config_count != 0;
}

static _EGLSurface *
dri2_neoos_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                                 void *native_window,
                                 const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const __DRIconfig *config;
   struct NeoosEGLWindow *win = native_window;

   if (!win) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "eglCreateWindowSurface");
      return NULL;
   }

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateWindowSurface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf,
                          attrib_list, false, native_window))
      goto cleanup_surface;

   config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT,
                                dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH,
               "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }

   dri2_surf->neoos_surface_id = win->surface_id;
   dri2_surf->base.Width = win->width;
   dri2_surf->base.Height = win->height;
   neoosCreateDrawable(dri2_surf);

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;

   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static EGLBoolean
dri2_neoos_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   dri2_dpy->core->destroyDrawable(dri2_surf->dri_drawable);
   dri2_fini_surface(surf);
   free(dri2_surf);
   return EGL_TRUE;
}

static EGLBoolean
dri2_neoos_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   /* swrast has no flush extension -- this directly invokes
    * neoosPutImage(op=__DRI_SWRAST_IMAGE_OP_SWAP) under the hood,
    * exactly like platform_x11.c's swrast fallback path. */
   dri2_dpy->core->swapBuffers(dri2_surf->dri_drawable);
   return EGL_TRUE;
}

static const struct dri2_egl_display_vtbl dri2_neoos_display_vtbl = {
   .create_window_surface = dri2_neoos_create_window_surface,
   .destroy_surface       = dri2_neoos_destroy_surface,
   .create_image          = dri2_create_image_khr,
   .swap_buffers          = dri2_neoos_swap_buffers,
   .get_dri_drawable      = dri2_surface_get_dri_drawable,
};

EGLBoolean
dri2_initialize_neoos(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy;
   const char *err;

   dri2_dpy = calloc(1, sizeof *dri2_dpy);
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   dri2_dpy->fd = -1;
   disp->DriverData = (void *) dri2_dpy;

   disp->Device = _eglAddDevice(dri2_dpy->fd, true);
   if (!disp->Device) {
      err = "DRI2: failed to find EGLDevice";
      goto cleanup;
   }

   dri2_dpy->driver_name = strdup("swrast");
   if (!dri2_dpy->driver_name || !dri2_load_driver_swrast(disp)) {
      err = "DRI2: failed to load swrast driver";
      goto cleanup;
   }

   dri2_dpy->loader_extensions = neoos_swrast_loader_extensions;

   if (!dri2_create_screen(disp)) {
      err = "DRI2: failed to create screen";
      goto cleanup;
   }

   if (!dri2_setup_extensions(disp)) {
      err = "DRI2: failed to find required DRI extensions";
      goto cleanup;
   }

   dri2_setup_screen(disp);

   if (!dri2_neoos_add_configs_for_visuals(disp)) {
      err = "DRI2: failed to add configs";
      goto cleanup;
   }

   dri2_dpy->vtbl = &dri2_neoos_display_vtbl;

   return EGL_TRUE;

cleanup:
   dri2_display_destroy(disp);
   return _eglError(EGL_NOT_INITIALIZED, err);
}
EOF
```

- [ ] **Step 3: Commit**

```bash
cd /home/neo/projects/personal/neoos-mesa
git add src-new/platform_neoos.c src-new/platform_neoos.h
git commit -q -m "feat: platform_neoos.c -- swrast-loader EGL platform backed by wmclient"
```

---

## Task 5: Build `libEGL.so`/`libGL.so` with the NeoOS platform and fix
real compile errors

**Files:**
- Modify: whichever of Task 3/4's files a real compiler error points
  at (expected: this task is where genuine struct-field-name or
  header-order mistakes surface and get fixed).

**Interfaces:**
- Consumes: everything from Tasks 1-4.
- Produces: a real, working `libEGL.so`/`libGL.so` with
  `EGL_PLATFORM_NEOOS_MESA` fully wired, verified by symbol presence.

- [ ] **Step 1: Full build**

```bash
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
cd /home/neo/projects/personal/neoos-mesa
./build.sh 2>&1 | tail -150
```

This is the real proof of Task 4's code: expect at least one real
compiler error on the first attempt (an unverified struct field name
or missing include is likely, given this file was hand-written against
read source rather than compiled). Read the actual error, fix the
specific line in `src-new/platform_neoos.c`/`.h`, and re-run
`patches/mesa-22.3.5/regen-patches.sh` is NOT needed for `src-new/`
changes (only for the six patched *existing* files) — just re-run
`./build.sh`, which re-copies `src-new/*` fresh each time.

- [ ] **Step 2: Verify the new symbols and platform dispatch**

```bash
x86_64-neoos-linux-musl-nm -D build-output/lib/libEGL.so* 2>&1 | grep -i "eglGetPlatformDisplay\|eglCreateWindowSurface\|eglSwapBuffers"
```

Expected: all three listed as defined.

- [ ] **Step 3: Commit any fixes made in Step 1**

```bash
git add -A
git commit -q -m "fix: real compile errors found building platform_neoos.c"
```

(Skip this step, no-op, if Step 1 succeeded on the first try.)

---

## Task 6: Test client — `egl_triangle_client.c`

**Files:**
- Create: `neoos-mesa/test/egl_triangle_client.c`
- Modify: `neoos-mesa/test/Makefile` (new target)

**Interfaces:**
- Consumes: `libEGL.so`/`libGL.so` (Task 5), `wmclient.h` (from
  `../neoos-wm`), `EGL_PLATFORM_NEOOS_MESA`/`struct NeoosEGLWindow`
  (Task 4's `platform_neoos.h`, copied for the test client's own
  `#include` the same way `build.sh` copies it into the Mesa tree).
- Produces: `egl_triangle_client.nex` — consumed by Task 7's boot test.

- [ ] **Step 1: Write the test client**

```bash
cat > /home/neo/projects/personal/neoos-mesa/test/egl_triangle_client.c <<'EOF'
/* neoos-mesa sub-project 2 proof of life: a standard EGL application
 * (eglGetPlatformDisplay/eglCreateWindowSurface/eglSwapBuffers -- not
 * OSMesa's private API) rendering a real GL triangle into an actual
 * neoos-wm window, composited on screen like any other client.
 */
#include <EGL/egl.h>
#include <GL/gl.h>
#include <stdio.h>
#include <unistd.h>

#include "wmclient.h"
#include "platform_neoos.h"

#define W 200
#define H 200

int main(void) {
    struct wm_conn *wm = wm_connect();
    if (!wm) {
        printf("FAILED: wm_connect returned NULL\n");
        return 1;
    }

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_NEOOS_MESA, wm, NULL);
    if (dpy == EGL_NO_DISPLAY) {
        printf("FAILED: eglGetPlatformDisplay returned EGL_NO_DISPLAY\n");
        return 1;
    }

    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("FAILED: eglInitialize failed\n");
        return 1;
    }
    printf("EGL %d.%d initialized\n", major, minor);

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(dpy, config_attribs, &config, 1, &num_configs)
        || num_configs < 1) {
        printf("FAILED: eglChooseConfig found no matching config\n");
        return 1;
    }

    int32_t sid = wm_create_window(wm, W, H, "egl-triangle");
    if (sid < 0) {
        printf("FAILED: wm_create_window failed (%d)\n", sid);
        return 1;
    }
    struct NeoosEGLWindow win = { sid, W, H };

    EGLSurface surf = eglCreateWindowSurface(dpy, config,
                                             (EGLNativeWindowType) &win, NULL);
    if (surf == EGL_NO_SURFACE) {
        printf("FAILED: eglCreateWindowSurface failed\n");
        return 1;
    }

    eglBindAPI(EGL_OPENGL_API);
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) {
        printf("FAILED: eglCreateContext failed\n");
        return 1;
    }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("FAILED: eglMakeCurrent failed\n");
        return 1;
    }

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(0.0f, 0.8f);
    glVertex2f(-0.8f, -0.8f);
    glVertex2f(0.8f, -0.8f);
    glEnd();

    eglSwapBuffers(dpy, surf);
    printf("PASS neoos-egl-triangle: swap complete, window committed\n");

    /* Hold the window open long enough for tools/screenshot.sh's
     * capture window (default 24s wait). */
    for (int i = 0; i < 40 && wm_alive(wm); i++)
        sleep(1);

    wm_disconnect(wm);
    return 0;
}
EOF
```

- [ ] **Step 2: Extend the test Makefile**

```bash
cat >> /home/neo/projects/personal/neoos-mesa/test/Makefile <<'EOF'

WM_DIR := ../../neoos-wm
EGL_CFLAGS := -O2 -Wall -I$(OSMESA_DIR)/include -I../src-new -I$(WM_DIR)
EGL_LDFLAGS := -L$(OSMESA_DIR)/lib -Wl,-rpath-link,$(OSMESA_DIR)/lib -lEGL -lGL -lm -lpthread

build/egl_triangle_client.nex: egl_triangle_client.c $(WM_DIR)/wmclient.c
	mkdir -p build
	$(CC) $(EGL_CFLAGS) egl_triangle_client.c $(WM_DIR)/wmclient.c -o $@ $(EGL_LDFLAGS)

all: build/egl_triangle_client.nex
EOF
```

- [ ] **Step 3: Cross-compile**

```bash
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
cd /home/neo/projects/personal/neoos-mesa/test
make build/egl_triangle_client.nex 2>&1 | tail -40
```

Expected: links cleanly (fix any real error the same way Task 5 Step 1
did — most likely candidate: `wmclient.c`'s own include assumptions,
or a missing `-lEGL` transitive symbol needing `-Wl,-rpath-link` to
also cover `libGL.so`'s own dependencies).

- [ ] **Step 4: Commit**

```bash
cd /home/neo/projects/personal/neoos-mesa
git add test/egl_triangle_client.c test/Makefile
git commit -q -m "test: standard EGL triangle client, real wm window"
```

---

## Task 7: Boot, screenshot, and pixel-verify

**Files:** none created in `neoos-kernel` beyond throwaway build
artifacts under `build/` (not committed).

**Interfaces:**
- Consumes: `egl_triangle_client.nex` (Task 6), the runtime `.so`s
  (`libEGL.so`, `libGL.so`, plus everything sub-project 1's Task 7
  already established: `libglapi.so.0`, `libstdc++.so.6`,
  `libgcc_s.so.1`, `libc.so`/`ld-musl-x86_64.so.1`).
- Produces: nothing for later tasks — this is the sub-project's actual
  proof of done.

- [ ] **Step 1: Stage runtime libraries (extends sub-project 1's set)**

```bash
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
cd /home/neo/projects/personal/neoos-mesa
mkdir -p build-output-runtime-libs
LIBS=build-output-runtime-libs
x86_64-neoos-linux-musl-strip -o $LIBS/libEGL.so.1 build-output/lib/libEGL.so.1*
x86_64-neoos-linux-musl-strip -o $LIBS/libGL.so.1 build-output/lib/libGL.so.1*
ls -la $LIBS/
```

(sub-project 1's Task 7 already staged `libOSMesa.so.8`, `libglapi.so.0`,
`libstdc++.so.6`, `libgcc_s.so.1`, `libc.so`, `ld-musl-x86_64.so.1` in
this same directory -- re-run those steps if the directory was cleaned
since then.)

- [ ] **Step 2: Check the actual SONAMEs `egl_triangle_client.nex`
  needs, and confirm every one is staged**

```bash
x86_64-neoos-linux-musl-readelf -d test/build/egl_triangle_client.nex 2>&1 | grep NEEDED
```

Stage anything listed that isn't already in `build-output-runtime-libs/`
(strip it the same way as Step 1).

- [ ] **Step 3: Build a GUI-mode disk image with the client installed**

```bash
cd /home/neo/projects/personal/NeoOS
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
mkdir -p /tmp/neoos-egl-portdir
cp /home/neo/projects/personal/neoos-mesa/test/build/egl_triangle_client.nex /tmp/neoos-egl-portdir/
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    PORT_DIRS="mesa=/tmp/neoos-egl-portdir" iso disk-image 2>&1 | tail -30
```

- [ ] **Step 4: Copy the runtime `.so`s to `::lib` and set the boot
  workload**

```bash
cd /home/neo/projects/personal/NeoOS
mmd -i build/disk.img ::lib 2>/dev/null || true
LIBS=/home/neo/projects/personal/neoos-mesa/build-output-runtime-libs
for f in "$LIBS"/*; do
    mcopy -o -i build/disk.img "$f" "::lib/$(basename "$f")"
done
printf 'wait /usr/local/bin/egl_triangle_client.nex\n' > /tmp/egl-triangle-inittab
mcopy -o -i build/disk.img /tmp/egl-triangle-inittab ::etc/inittab
mdir -i build/disk.img ::lib
```

- [ ] **Step 5: Boot with the GUI framebuffer path, screenshot**

```bash
cd /home/neo/projects/personal/NeoOS
tools/screenshot.sh build/egl-triangle.ppm 30
```

(Uses the disk images just built above -- `screenshot.sh` boots
`build/disk.img`/`build/disk2.img` as they currently stand, per its
own script body.)

- [ ] **Step 6: Pixel-verify**

```bash
/home/neo/anaconda3/bin/python3 <<'EOF'
from PIL import Image
img = Image.open("build/egl-triangle.png")
w, h = img.size
print(f"screenshot size: {w}x{h}")
# The triangle's centroid, in screen coordinates -- wherever wm placed
# the 200x200 window (top-left by wm's default placement, matching
# every other wmdemo.c-style test's convention) plus the triangle's
# own center within that window (W/2, roughly H*0.4 from the top,
# matching the glOrtho(-1,1,-1,1) + the three glVertex2f calls in
# egl_triangle_client.c). Confirm against wm's actual placement first
# if this doesn't land on red.
cx, cy = 100, 100
px = img.getpixel((cx, cy))
print(f"pixel at ({cx},{cy}): {px}")
assert px[0] > 150 and px[1] < 100 and px[2] < 100, f"expected red-ish, got {px}"
print("PASS: red triangle pixel confirmed on screen")
EOF
```

Expected: `PASS: red triangle pixel confirmed on screen`. If the pixel
doesn't land on red, open `build/egl-triangle.png` and find the
triangle's actual on-screen coordinates first (wm's window placement
and this test's exact `glOrtho`/vertex choices interact) rather than
assuming the render itself is broken.

---

## Task 8: Regression — sub-project 1's OSMesa path, and the full gauntlet

**Files:** none.

- [ ] **Step 1: Re-run sub-project 1's OSMesa triangle test unchanged**

```bash
cd /home/neo/projects/personal/neoos-mesa/test
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
make build/triangle_test.nex
```

Then repeat `docs/superpowers/plans/2026-09-22-gallium3d-osmesa-bringup.md`
Task 7's boot steps (PORT_DIRS + custom inittab wait entry + headless
QEMU + serial log check for `PASS neoos-mesa-triangle`). Expected:
unchanged pass -- confirms adding the `neoos` EGL platform didn't
regress the off-screen OSMesa path, since both share the same
underlying `softpipe`/Gallium code.

- [ ] **Step 2: Full gauntlet regression**

```bash
cd /home/neo/projects/personal/NeoOS
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
tools/gauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`, zero retries.

---

## Task 9: Documentation

**Files:**
- Modify: `docs/project-goal.md` (Status section)
- Modify: `docs/superpowers/specs/2026-09-22-gallium3d-egl-platform-design.md`
  (deviations-found note, matching sub-project 1's own pattern)

- [ ] **Step 1: Append to `docs/project-goal.md`'s Status section**

Add a new paragraph recording sub-project 2's completion: the real
`EGL_PLATFORM_NEOOS_MESA` platform, a standard EGL application
rendering into an actual wm window verified by screenshot, and the
concrete list of new runtime `.so`s (`libEGL.so`, `libGL.so`) any
future EGL-based application needs alongside sub-project 1's list.
Note that sub-project 3 (swapping `neoos-wm`'s own internal TinyGL
usage onto this platform) remains open, not yet brainstormed.

- [ ] **Step 2: Add a deviations-found note to the design spec**

Record, in a new section matching sub-project 1's "Deviations found
during implementation" pattern: the `meson_options.txt`/
`with_platform_neoos` simplification (Global Constraints above), and
any other real surprises Task 5's first compile attempt surfaced.

- [ ] **Step 3: Commit**

```bash
cd /home/neo/projects/personal/NeoOS
git add docs/project-goal.md docs/superpowers/specs/2026-09-22-gallium3d-egl-platform-design.md
git commit -m "$(cat <<'EOF'
docs: Gallium3D sub-project 2 (NeoOS EGL platform) complete

A real EGL_PLATFORM_NEOOS_MESA platform renders standard EGL/GL
applications into actual neoos-wm windows, verified by screenshot.
Sub-project 3 (swapping wm's own internal TinyGL usage onto this
stack) is next, not yet brainstormed.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

**Spec coverage:** every Goals bullet in
`2026-09-22-gallium3d-egl-platform-design.md` maps to a task —
patch mechanism (Tasks 2-3), `platform_neoos.c` (Task 4), native
display/window types (Task 4/6), build config (Task 1), test client +
verification (Tasks 6-7), regression (Task 8), docs (Task 9). The one
correction (no `meson_options.txt`/`platforms`-array change needed) is
called out explicitly in Global Constraints and Task 9 rather than
silently contradicting the spec.

**Placeholder scan:** no TBD; `platform_neoos.c`/`.h` are complete,
real code derived directly from reading `platform_x11.c`'s actual
swrast functions, `egl_dri2.c`/`.h`'s real struct/function
signatures, and `egldisplay.c`'s real `_eglGetSurfacelessDisplay`
pattern -- not fabricated against guessed APIs. Task 5 explicitly
expects and budgets for a first-compile-attempt error, which is honest
uncertainty about hand-written driver code, not a hidden gap in the
plan.

**Type/interface consistency:** `EGL_PLATFORM_NEOOS_MESA` (Task 4's
header) is the exact symbol Task 6's test client includes and calls;
`struct NeoosEGLWindow` is defined once (Task 4) and consumed once
(Task 6); `dri2_initialize_neoos` is declared in Task 3's
`egl_dri2.h` patch and defined in Task 4, with matching signature.
