# Taskbar and Start Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A glass taskbar (ring-logo Start button + live clock) docked at the bottom of the screen, and a Start menu that opens on click and closes on a second click or any click elsewhere, both rendered through a new configurable-intensity Liquid Glass.

**Architecture:** Two protocol-level compositor primitives (fixed-position surfaces, real `WM_FOCUS` delivery) plus a compositor-side glass-intensity post-blend, all general-purpose and reusable beyond this feature. Two new client binaries (`taskbar.nex`, libneoos-linked like `wmdemo.c`; `startmenu.nex`, spawned by the taskbar and self-closing on focus loss) consume them.

**Tech Stack:** C (gnu11), `x86_64-elf-gcc` freestanding cross-compile (matches every other NeoOS userland build), `inkscape` + Python/PIL for the one build-time SVG->C-array asset (host-only, never on-target).

**Spec:** `docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md`

## Global Constraints

- Glass intensity is a compositor-side post-blend only -- no changes to `neoos-tinygl` or the SPIR-V shader. (spec Non-goals, Design section 3)
- Start menu ships with no content this round -- an empty glass panel. (spec Non-goals)
- No work-area reservation, no wallpaper image, no taskbar entries for windows. (spec Non-goals)
- `taskbar.nex` and `startmenu.nex` are libneoos-linked (crt0.o + `libneoos.a`), NOT musl -- `clock_gettime` and `pthread_create` are both already available there. (spec Design section 4, corrected during spec review)
- Both the taskbar (40px tall) and start menu (320x420) request `glass_intensity = 50`. (spec Goals)
- `wm_create_surface`'s wire struct gains `x, y, glass_intensity` fields -- 21 bytes plus padding, still under `WM_MAX_BODY` (64). (spec Design section 1)
- Every deliberate protocol addition gets documented in `docs/stdlib.md` (NeoOS-native, no POSIX analogue) per repo convention.

---

## File Structure

```
neoos-wm/
  wmproto.h            # Task 1: WM_SURFACE_FIXED_POS, wm_create_surface's new fields
  wmclient.h / .c       # Task 1: wm_create_surface_ex, existing wrappers refactored onto it
  wm.c                  # Task 2: struct client.fixed_pos/glass_intensity, positioning,
                        #         draw_glass rect fix + intensity blend
                        # Task 3: set_focus() helper, WM_FOCUS wiring
  assets/logo/
    neoos-start-neon-ring.svg   # Task 4: copied from the user-supplied source
    logo28.png                   # Task 4: generated (inkscape), committed for provenance
    logo.h                        # Task 4: generated (png2c.py), committed
  tools/png2c.py         # Task 4: host-only PNG -> C array converter
  taskbar.c             # Task 4: the taskbar client
  startmenu.c            # Task 5: the start-menu client
  Makefile               # Task 4: TASKBAR.ELF + asset pipeline rules
                        # Task 5: STARTMENU.ELF rule

NeoOS/ (this repo)
  Makefile               # Task 6: install taskbar.nex/startmenu.nex; wm-taskbar test target
  docs/stdlib.md         # Task 7: document WM_SURFACE_FIXED_POS, wm_create_surface_ex,
                        #         glass_intensity
```

---

## Task 1: Protocol extension -- fixed-position surfaces and glass intensity

**Files:**
- Modify: `neoos-wm/wmproto.h`
- Modify: `neoos-wm/wmclient.h`
- Modify: `neoos-wm/wmclient.c`

**Interfaces:**
- Produces (for Task 2): `#define WM_SURFACE_FIXED_POS 4`, `struct wm_create_surface { uint32_t width, height, flags; int32_t x, y; uint8_t glass_intensity; };`
- Produces (for Task 4): `#define WM_FORMAT_ARGB8888 2`
- Produces (for Task 4/5): `int32_t wm_create_surface_ex(struct wm_conn *c, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t flags, uint8_t glass_intensity, const char *title);`

- [ ] **Step 1: Add the new flag, the new pixel format, and widen `wm_create_surface` in `wmproto.h`**

Replace:
```c
// The only pixel format G4 speaks: 32bpp, opaque, no alpha, no
// blending -- the same layout /dev/fb0 uses, so compositing is a copy.
#define WM_FORMAT_XRGB8888 1
```
with:
```c
// 32bpp, opaque, no alpha, no blending -- the same layout /dev/fb0
// uses, so compositing a WM_FORMAT_XRGB8888 surface is a straight
// copy. The only format that existed before this milestone.
#define WM_FORMAT_XRGB8888 1
// 32bpp, top byte is real alpha (straight, not premultiplied). Only
// meaningful for WM_SURFACE_GLASS surfaces -- see
// docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md
// section 3b. A client never requests this directly: wm_create_surface_ex
// (Task 1, Step 3) selects it automatically whenever WM_SURFACE_GLASS
// is set.
#define WM_FORMAT_ARGB8888 2
```

Replace:
```c
#define WM_SURFACE_GLASS  2

struct wm_create_surface { uint32_t width, height, flags; };
```
with:
```c
#define WM_SURFACE_GLASS  2
// WM_SURFACE_FIXED_POS: honor wm_create_surface's x,y fields instead
// of the compositor's cascade placement. Orthogonal to SHELL/GLASS --
// a fixed-position surface may also be glass (the taskbar and start
// menu both are). See docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md.
#define WM_SURFACE_FIXED_POS 4

struct wm_create_surface {
    uint32_t width, height, flags;
    int32_t  x, y;              // only consulted when WM_SURFACE_FIXED_POS is set
    uint8_t  glass_intensity;   // 0-100; only consulted when WM_SURFACE_GLASS is set.
                                 // Default 100 (full effect, today's existing behavior)
                                 // when a caller does not set it explicitly.
};
```

- [ ] **Step 2: Add `wm_create_surface_ex` to `wmclient.h`**

Replace the three existing declarations' surrounding comment block --
keep `wm_create_window`/`wm_create_glass_window`/`wm_create_shell`
exactly as they are (signatures unchanged), and add immediately after
`wm_create_shell`'s declaration:

```c
// Full control: explicit position (only honored with flags &
// WM_SURFACE_FIXED_POS) and glass intensity (only meaningful with
// flags & WM_SURFACE_GLASS; ignored otherwise). wm_create_window/
// wm_create_glass_window/wm_create_shell are thin wrappers over this
// with x=y=0, glass_intensity=100.
int32_t wm_create_surface_ex(struct wm_conn *c, int32_t x, int32_t y,
                              uint32_t w, uint32_t h, uint32_t flags,
                              uint8_t glass_intensity, const char *title);
```

- [ ] **Step 3: Refactor `wmclient.c`'s `create_surface` into `wm_create_surface_ex`**

The existing static `create_surface(c, w, h, title, flags)` (around
line 110) becomes the new public `wm_create_surface_ex`. Replace:

```c
static int32_t create_surface(struct wm_conn *c, uint32_t w, uint32_t h,
                              const char *title, uint32_t flags) {
    if (!c || !c->alive) { return -1; }
    c->w = w; c->h = h;
    c->id = 1;

    struct wm_create_surface cs = { w, h, flags };
```
with:
```c
int32_t wm_create_surface_ex(struct wm_conn *c, int32_t x, int32_t y,
                              uint32_t w, uint32_t h, uint32_t flags,
                              uint8_t glass_intensity, const char *title) {
    if (!c || !c->alive) { return -1; }
    c->w = w; c->h = h;
    c->id = 1;

    struct wm_create_surface cs = { w, h, flags, x, y, glass_intensity };
```

(the rest of the function body -- memfd creation, `WM_ATTACH_BUFFER`
send, title send -- is unchanged, only the signature and the `cs`
initializer change). **Deliberately NOT changed here**: the
`WM_ATTACH_BUFFER` message this function builds still hardcodes
`WM_FORMAT_XRGB8888` for now -- switching it to request
`WM_FORMAT_ARGB8888` automatically for glass surfaces happens in Task 4,
together with the compositor-side changes that actually accept and
composite that format. Doing it here instead would make every glass
surface's `WM_ATTACH_BUFFER` get rejected by `wm.c` (which still only
accepts `WM_FORMAT_XRGB8888` until Task 4) -- breaking the existing
`wm-glass` test between this task and Task 4.

Then update the three wrappers at the bottom of the file:

```c
int32_t wm_create_window(struct wm_conn *c, uint32_t w, uint32_t h, const char *title) {
    return wm_create_surface_ex(c, 0, 0, w, h, WM_SURFACE_NORMAL, 100, title);
}

int32_t wm_create_glass_window(struct wm_conn *c, uint32_t w, uint32_t h, const char *title) {
    return wm_create_surface_ex(c, 0, 0, w, h, WM_SURFACE_GLASS, 100, title);
}

int32_t wm_create_shell(struct wm_conn *c, uint32_t w, uint32_t h) {
    if (!c) { return -1; }
    if (w == 0) { w = c->screen_w; }
    if (h == 0) { h = c->screen_h; }
    return wm_create_surface_ex(c, 0, 0, w, h, WM_SURFACE_SHELL, 100, 0);
}
```

- [ ] **Step 4: Build to confirm it compiles**

```bash
cd /home/neo/projects/personal/neoos-wm
make clean && make
```

Expected: `build/WM.ELF`, `build/libwmclient.a`, `build/WMDEMO.ELF` all
build with no errors (this changes `wmdemo.c`'s call path through the
unchanged wrapper signatures, so `wmdemo.c` itself needs no edits).

- [ ] **Step 5: Commit**

```bash
cd /home/neo/projects/personal/neoos-wm
git add wmproto.h wmclient.h wmclient.c
git commit -m "wm: add WM_SURFACE_FIXED_POS, WM_FORMAT_ARGB8888, and per-surface glass intensity to the protocol

wm_create_surface gains x,y (honored only with WM_SURFACE_FIXED_POS)
and glass_intensity (honored only with WM_SURFACE_GLASS, default 100
= today's existing full effect). wm_create_surface_ex is the new
general entry point; the three existing wrappers are now thin calls
onto it with unchanged signatures and behavior. WM_FORMAT_ARGB8888 is
declared but not yet requested or accepted anywhere -- that lands in
Task 4 together, so no intermediate commit ever has one side of the
wire expecting a format the other side rejects."
```

---

## Task 2: Real `WM_FOCUS` delivery (self-contained, general-purpose)

**Files:**
- Modify: `neoos-wm/wm.c`

**Interfaces:**
- Produces (for Task 3): `static void set_focus(int new_slot);`

This task is independent of Task 1's protocol changes and of Task 3 --
it only touches focus bookkeeping that already exists (`focus_slot`,
`clients[]`), so it builds and is testable entirely on its own.

- [ ] **Step 1: Add the `set_focus` helper**

Insert immediately after `place_window` (which ends around line 447)
and before `handle_message` (which starts around line 449):

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

- [ ] **Step 2: Use it from the click handler, including bare-desktop clicks**

Replace (around line 567-581, inside `pump_mouse`):
```c
        else if (ev[i].type == EV_KEY && ev[i].code == BTN_LEFT) {
            int hit = slot_at(cur_x, cur_y);
            if (ev[i].value == 1 && hit >= 0 && hit != focus_slot) {
                // The old and new focus both change colour.
                if (focus_slot >= 0) { damage_window(&clients[focus_slot]); }
                focus_slot = hit;
                damage_window(&clients[hit]);
                screen_dirty = 1;
            }
            if (hit >= 0) {
                struct wm_pointer_button pb = { (uint16_t)ev[i].code,
                                                (uint16_t)ev[i].value };
                send_to(&clients[hit], WM_POINTER_BUTTON, &pb, sizeof pb);
            }
        }
```
with:
```c
        else if (ev[i].type == EV_KEY && ev[i].code == BTN_LEFT) {
            int hit = slot_at(cur_x, cur_y);
            if (ev[i].value == 1) { set_focus(hit); }
            if (hit >= 0) {
                struct wm_pointer_button pb = { (uint16_t)ev[i].code,
                                                (uint16_t)ev[i].value };
                send_to(&clients[hit], WM_POINTER_BUTTON, &pb, sizeof pb);
            }
        }
```

This is the actual behavior change from the old code: a left-click on
bare desktop (`hit == -1`) now calls `set_focus(-1)`, which notifies
whoever previously held focus that they lost it -- previously this
case (`hit < 0`) was silently ignored.

- [ ] **Step 3: Build to confirm it compiles**

```bash
cd /home/neo/projects/personal/neoos-wm
make clean && make
```

Expected: builds cleanly, `WM.ELF`/`libwmclient.a`/`WMDEMO.ELF` all
produced.

- [ ] **Step 4: Verify on target -- existing focus-change behavior (border color) still works**

```bash
cd /home/neo/projects/personal/NeoOS
make WM_DIR=../neoos-wm wm-glass
```

Expected: passes exactly as before (this target predates this change
and does not exercise `WM_FOCUS` delivery specifically, but confirms
the click/focus code path did not regress the existing smoke test).

- [ ] **Step 5: Commit**

```bash
cd /home/neo/projects/personal/neoos-wm
git add wm.c
git commit -m "wm: actually deliver WM_FOCUS (was defined, never sent)

WM_FOCUS has existed in the wire protocol and in wmclient's event
decode since the protocol was designed, but the compositor never sent
it. Factored the existing click-driven focus-change code into
set_focus(), used from the click handler (now also firing on a bare-
desktop click, which previously did nothing) and, in the next commit,
from surface creation. General-purpose fix, not specific to any one
feature."
```

---

## Task 3: Compositor support -- positioning and glass intensity

**Files:**
- Modify: `neoos-wm/wm.c`

**Interfaces:**
- Consumes: `WM_SURFACE_FIXED_POS`, `wm_create_surface`'s new fields (Task 1); `set_focus` (Task 2).

- [ ] **Step 1: Add `fixed_pos` and `glass_intensity` to `struct client`**

Replace (around line 132-145):
```c
struct client {
    int       fd;             // -1 when the slot is free
    uint32_t  id;
    int       has_surface;
    uint32_t  w, h;           // content size
    int       x, y;           // content origin on screen
    uint32_t *px;             // the client's memfd, mapped here
    uint64_t  bytes;
    uint32_t  stride_px;
    char      title[64];
    int       mapped;         // committed at least once
    int       is_shell;       // undecorated, at the origin, behind all
    int       is_glass;       // rendered through the glass shader (Task 7)
};
```
with:
```c
struct client {
    int       fd;             // -1 when the slot is free
    uint32_t  id;
    int       has_surface;
    uint32_t  w, h;           // content size
    int       x, y;           // content origin on screen
    uint32_t *px;             // the client's memfd, mapped here
    uint64_t  bytes;
    uint32_t  stride_px;
    char      title[64];
    int       mapped;         // committed at least once
    int       is_shell;       // undecorated, at the origin, behind all
    int       is_glass;       // rendered through the glass shader (Task 7)
    int       fixed_pos;      // positioned by the client, not cascaded (Task 3)
    uint32_t  glass_intensity; // 0-100, only meaningful when is_glass (Task 3)
};
```

- [ ] **Step 2: Honor `WM_SURFACE_FIXED_POS` and `glass_intensity`, and grant focus on creation, in `WM_CREATE_SURFACE`**

Replace (around line 464-481):
```c
    case WM_CREATE_SURFACE: {
        struct wm_create_surface *cs = (struct wm_create_surface *)body;
        if (cs->width == 0 || cs->height == 0 ||
            cs->width > fb_w || cs->height > fb_h) { break; }
        c->id = h->surface_id;
        c->w = cs->width; c->h = cs->height;
        c->has_surface = 1;
        c->is_shell = (cs->flags & WM_SURFACE_SHELL) != 0;
        c->is_glass = (cs->flags & WM_SURFACE_GLASS) != 0;
        if (c->is_shell) {
            c->x = 0; c->y = 0;         // the shell owns the whole screen
        } else {
            place_window(c);
        }
        struct wm_configure cfg = { c->w, c->h };
        send_to(c, WM_CONFIGURE, &cfg, sizeof cfg);
        printf("[wm] surface %ux%u\n", c->w, c->h);
        break;
    }
```
with:
```c
    case WM_CREATE_SURFACE: {
        struct wm_create_surface *cs = (struct wm_create_surface *)body;
        if (cs->width == 0 || cs->height == 0 ||
            cs->width > fb_w || cs->height > fb_h) { break; }
        c->id = h->surface_id;
        c->w = cs->width; c->h = cs->height;
        c->has_surface = 1;
        c->is_shell = (cs->flags & WM_SURFACE_SHELL) != 0;
        c->is_glass = (cs->flags & WM_SURFACE_GLASS) != 0;
        c->fixed_pos = (cs->flags & WM_SURFACE_FIXED_POS) != 0;
        c->glass_intensity = cs->glass_intensity > 100 ? 100 : cs->glass_intensity;
        if (c->is_shell) {
            c->x = 0; c->y = 0;         // the shell owns the whole screen
        } else if (c->fixed_pos) {
            c->x = cs->x; c->y = cs->y;
        } else {
            place_window(c);
        }
        struct wm_configure cfg = { c->w, c->h };
        send_to(c, WM_CONFIGURE, &cfg, sizeof cfg);
        printf("[wm] surface %ux%u\n", c->w, c->h);
        // A newly created surface takes focus immediately, matching
        // ordinary popup behavior -- this is what lets the start menu
        // (Task 5) receive its own WM_FOCUS{0} the moment focus moves
        // elsewhere, with no signal needed from the taskbar.
        if (!c->is_shell) { set_focus((int)(c - clients)); }
        break;
    }
```

- [ ] **Step 3: Fix `damage_window` and `draw_glass`'s rect calculation for fixed-position (chromeless) surfaces**

Both functions currently key their decoration-margin logic off
`is_shell` alone. A `WM_SURFACE_FIXED_POS` surface (taskbar, start
menu) is equally chromeless and must use its exact rect, not the
decorated-window margin (`BORDER`/`TITLE_H`) -- otherwise the glass
shading rect would extend beyond the surface's real content into
desktop space that is not actually part of it, visibly misaligning the
frosted background from the drawn content.

Replace (around line 257-262):
```c
static void damage_window(struct client *c) {
    if (!c->mapped) { return; }
    if (c->is_shell) { damage(c->x, c->y, (int)c->w, (int)c->h); return; }
    damage(c->x - BORDER, c->y - TITLE_H - BORDER,
           (int)c->w + 2 * BORDER, (int)c->h + TITLE_H + 2 * BORDER);
}
```
with:
```c
static void damage_window(struct client *c) {
    if (!c->mapped) { return; }
    if (c->is_shell || c->fixed_pos) { damage(c->x, c->y, (int)c->w, (int)c->h); return; }
    damage(c->x - BORDER, c->y - TITLE_H - BORDER,
           (int)c->w + 2 * BORDER, (int)c->h + TITLE_H + 2 * BORDER);
}
```

Replace (around line 272-275, inside `draw_glass`):
```c
    int x = c->is_shell ? c->x : c->x - BORDER;
    int y = c->is_shell ? c->y : c->y - TITLE_H - BORDER;
    int w = c->is_shell ? (int)c->w : (int)c->w + 2 * BORDER;
    int h = c->is_shell ? (int)c->h : (int)c->h + TITLE_H + 2 * BORDER;
```
with:
```c
    int chromeless = c->is_shell || c->fixed_pos;
    int x = chromeless ? c->x : c->x - BORDER;
    int y = chromeless ? c->y : c->y - TITLE_H - BORDER;
    int w = chromeless ? (int)c->w : (int)c->w + 2 * BORDER;
    int h = chromeless ? (int)c->h : (int)c->h + TITLE_H + 2 * BORDER;
```

- [ ] **Step 4: Add the intensity post-blend to `draw_glass`**

Immediately after the existing `tglUseSpvProgram(NULL, NULL);` line and
before `free(snap);` (around line 313-315), insert:

```c
    // Blend the just-drawn shaded output back toward the original
    // unshaded backdrop by (100 - intensity)%. intensity=100 (the
    // default) is a full no-op: shaded stays shaded. Both operands
    // (snap = pre-shade, back's current contents = post-shade) already
    // exist here -- no shader/VM changes needed. snap's byte order is
    // R,G,B,A (set a few lines above in this same function).
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

- [ ] **Step 5: Build and run the existing glass regression test**

```bash
cd /home/neo/projects/personal/neoos-wm
make clean && make
cd /home/neo/projects/personal/NeoOS
make WM_DIR=../neoos-wm wm-glass
```

Expected: both build and target test pass. `glass_intensity` defaults
to 100 for every existing call site (Task 1's wrappers), so this is a
true regression check -- `wm-glass`'s visual output must be pixel-for-
pixel unchanged (the intensity branch is skipped entirely at 100).

- [ ] **Step 6: Commit**

```bash
cd /home/neo/projects/personal/neoos-wm
git add wm.c
git commit -m "wm: honor fixed-position surfaces and per-surface glass intensity

struct client gains fixed_pos and glass_intensity. WM_CREATE_SURFACE
positions a fixed_pos surface at its requested x,y instead of
cascading, and now grants a newly created (non-shell) surface focus
immediately. damage_window/draw_glass treat fixed_pos surfaces as
chromeless (exact rect, no title-bar/border margin) the same way
is_shell already is. draw_glass gains an intensity post-blend toward
the unshaded backdrop -- no shader/VM changes. wm-glass regression
test confirmed unaffected (intensity defaults to 100 = no-op)."
```

---

## Task 4: Per-pixel alpha compositing for glass surface content

**Files:**
- Modify: `neoos-wm/wmproto.h`
- Modify: `neoos-wm/wmclient.c`
- Modify: `neoos-wm/wm.c`
- Modify: `neoos-wm/wmdemo.c`

**Interfaces:**
- Produces (for Task 5/6): every `WM_SURFACE_GLASS` surface's buffer
  is `WM_FORMAT_ARGB8888` (top byte = real alpha) from this task
  onward -- `taskbar.c`/`startmenu.c` (Tasks 5-6) are written against
  this from the start, not retrofitted.

This is the fix for the gap found during planning (spec section 3b):
without this, a chromeless glass surface's own opaque content always
fully covers the shaded backdrop, no matter what `glass_intensity` is
set to.

- [ ] **Step 1: Add `WM_FORMAT_ARGB8888` to `wmproto.h`**

Replace:
```c
// The only pixel format G4 speaks: 32bpp, opaque, no alpha, no
// blending -- the same layout /dev/fb0 uses, so compositing is a copy.
#define WM_FORMAT_XRGB8888 1
```
with:
```c
// 32bpp, opaque, no alpha, no blending -- the same layout /dev/fb0
// uses, so compositing a WM_FORMAT_XRGB8888 surface is a straight
// copy. The only format that existed before this milestone.
#define WM_FORMAT_XRGB8888 1
// 32bpp, top byte is real alpha (straight, not premultiplied). Only
// meaningful for WM_SURFACE_GLASS surfaces -- see
// docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md
// section 3b. A client never requests this directly:
// wm_create_surface_ex selects it automatically whenever
// WM_SURFACE_GLASS is set (see wmclient.c).
#define WM_FORMAT_ARGB8888 2
```

- [ ] **Step 2: Request `WM_FORMAT_ARGB8888` automatically for glass surfaces in `wmclient.c`**

In `wm_create_surface_ex` (Task 1), find:
```c
    struct wm_attach_buffer ab = { w * 4, WM_FORMAT_XRGB8888 };
```
Replace with:
```c
    uint32_t pixel_format = (flags & WM_SURFACE_GLASS) ? WM_FORMAT_ARGB8888 : WM_FORMAT_XRGB8888;
    struct wm_attach_buffer ab = { w * 4, pixel_format };
```

- [ ] **Step 3: Accept `WM_FORMAT_ARGB8888` and store it per-client in `wm.c`**

Add `uint32_t format;` to `struct client` (alongside `fixed_pos`/
`glass_intensity` from Task 3):
```c
    int       fixed_pos;      // positioned by the client, not cascaded (Task 3)
    uint32_t  glass_intensity; // 0-100, only meaningful when is_glass (Task 3)
    uint32_t  format;          // WM_FORMAT_*, only ARGB8888 changes blit behavior (Task 4)
```

Replace the `WM_ATTACH_BUFFER` handler's format check (around line
484-497):
```c
    case WM_ATTACH_BUFFER: {
        struct wm_attach_buffer *ab = (struct wm_attach_buffer *)body;
        if (!c->has_surface || passed_fd < 0) { break; }
        if (ab->format != WM_FORMAT_XRGB8888) { close(passed_fd); break; }
```
with:
```c
    case WM_ATTACH_BUFFER: {
        struct wm_attach_buffer *ab = (struct wm_attach_buffer *)body;
        if (!c->has_surface || passed_fd < 0) { break; }
        if (ab->format != WM_FORMAT_XRGB8888 && ab->format != WM_FORMAT_ARGB8888) {
            close(passed_fd); break;
        }
        c->format = ab->format;
```

- [ ] **Step 4: Alpha-blend the content in `draw_window` when `format == WM_FORMAT_ARGB8888`**

Replace (around line 244-253):
```c
    // The content, straight out of the client's own pages.
    for (uint32_t row = 0; row < c->h; row++) {
        int y = c->y + (int)row;
        if (y < 0 || y >= (int)fb_h) { continue; }
        uint32_t *dst = back + (uint64_t)y * fb_w + c->x;
        const uint32_t *src = c->px + (uint64_t)row * c->stride_px;
        int w = (int)c->w;
        if (c->x + w > (int)fb_w) { w = (int)fb_w - c->x; }
        for (int col = 0; col < w; col++) { dst[col] = src[col]; }
    }
```
with:
```c
    // The content, straight out of the client's own pages -- alpha-
    // blended against whatever draw_glass (+ its intensity blend)
    // already left in `back` when the surface is ARGB8888, straight
    // copy otherwise (unchanged from before this task).
    for (uint32_t row = 0; row < c->h; row++) {
        int y = c->y + (int)row;
        if (y < 0 || y >= (int)fb_h) { continue; }
        uint32_t *dst = back + (uint64_t)y * fb_w + c->x;
        const uint32_t *src = c->px + (uint64_t)row * c->stride_px;
        int w = (int)c->w;
        if (c->x + w > (int)fb_w) { w = (int)fb_w - c->x; }
        if (c->format == WM_FORMAT_ARGB8888) {
            for (int col = 0; col < w; col++) {
                uint32_t s = src[col];
                uint32_t sa = (s >> 24) & 0xff;
                if (sa == 0) { continue; }        // fully transparent -- leave the shaded backdrop
                uint32_t sr = (s >> 16) & 0xff, sg = (s >> 8) & 0xff, sb = s & 0xff;
                if (sa == 255) { dst[col] = (sr << 16) | (sg << 8) | sb; continue; }
                uint32_t d = dst[col];
                uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
                uint32_t inv = 255 - sa;
                uint32_t r = (sr * sa + dr * inv) / 255;
                uint32_t g = (sg * sa + dg * inv) / 255;
                uint32_t b = (sb * sa + db * inv) / 255;
                dst[col] = (r << 16) | (g << 8) | b;
            }
        } else {
            for (int col = 0; col < w; col++) { dst[col] = src[col]; }
        }
    }
```

- [ ] **Step 5: Fix `wmdemo.c`'s glass-mode fill for the new default format**

Its glass fill currently relies on the top byte being ignored under
the old always-`XRGB8888` behavior:
```c
        for (uint32_t y = 0; y < 120; y++) {
            for (uint32_t x = 0; x < 160; x++) { px[y * stride + x] = 0x00FFFFFF; }
        }
```
Replace `0x00FFFFFF` with `0xFFFFFFFF` (alpha=255, opaque) -- the
surface is now `WM_FORMAT_ARGB8888` (Step 2), so the top byte is real
alpha and must be set:
```c
        for (uint32_t y = 0; y < 120; y++) {
            for (uint32_t x = 0; x < 160; x++) { px[y * stride + x] = 0xFFFFFFFF; }
        }
```

- [ ] **Step 6: Build and run the glass regression test**

```bash
cd /home/neo/projects/personal/neoos-wm
make clean && make
cd /home/neo/projects/personal/NeoOS
make WM_DIR=../neoos-wm wm-glass
```

Expected: both build and target test pass, with output visually
unchanged from before this task (confirm via `make WM_DIR=../neoos-wm wm-shot`
and comparing the screenshot against the one taken during the original
glass-lensing milestone if still available, or simply confirming the
glass demo window still shows solid white content with no unexpected
transparency -- alpha=255 everywhere in this demo's fill means the
new blend path takes the `sa == 255` fast path and produces the exact
same output as the old straight copy).

- [ ] **Step 7: Commit**

```bash
cd /home/neo/projects/personal/neoos-wm
git add wmproto.h wmclient.c wm.c wmdemo.c
git commit -m "wm: real per-pixel alpha compositing for glass surface content

WM_FORMAT_ARGB8888, requested automatically by wm_create_surface_ex
for any WM_SURFACE_GLASS surface. draw_window alpha-blends such a
surface's content against the already-shaded backdrop instead of an
unconditional opaque copy -- this is what makes glass_intensity's
'mostly see-through, opaque only at real UI' actually achievable:
without it, chromeless glass content always fully covered the shaded
backdrop regardless of intensity. wmdemo.c's glass-mode fill updated
(0x00FFFFFF -> 0xFFFFFFFF) since its top byte is now real alpha, not
an ignored byte -- confirmed wm-glass still passes, unchanged output."
```

---

## Task 5: Taskbar client (`taskbar.nex`)

**Files:**
- Create: `neoos-wm/assets/logo/neoos-start-neon-ring.svg`
- Create: `neoos-wm/assets/logo/logo.h` (generated, committed)
- Create: `neoos-wm/tools/png2c.py`
- Create: `neoos-wm/font_term.c` / `font_term.h` (copied)
- Create: `neoos-wm/taskbar.c`
- Modify: `neoos-wm/Makefile`

**Interfaces:**
- Consumes: `wm_create_surface_ex` (Task 1), `WM_FORMAT_ARGB8888`
  compositing (Task 4), `spawn`/`wait` (`unistd.h`, libneoos),
  `pthread_create`/`pthread_join` (`pthread.h`, libneoos).
- Produces: `/usr/local/bin/taskbar.nex` (installed by Task 7),
  `LOGO_W`, `LOGO_H`, `neoos_logo_rgba` (from `logo.h`, consumed only
  by `taskbar.c` itself).

- [ ] **Step 1: Copy the source SVG for provenance**

```bash
cd /home/neo/projects/personal/neoos-wm
mkdir -p assets/logo
cp /home/neo/Desktop/ring/neoos-start-neon-ring.svg assets/logo/neoos-start-neon-ring.svg
```

- [ ] **Step 2: Write `tools/png2c.py`**

```python
#!/usr/bin/env python3
# neoos-wm/tools/png2c.py -- host-only. Converts a PNG to a straight
# (non-premultiplied) RGBA8 C array. Never run on-target -- see
# docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md
# section 6.
import sys
from PIL import Image

def main():
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} in.png out.h SYMBOL_PREFIX", file=sys.stderr)
        sys.exit(1)
    src, out, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    data = im.tobytes()  # R,G,B,A per pixel, straight alpha -- confirmed during planning
    with open(out, "w") as f:
        f.write(f"// GENERATED by tools/png2c.py from {src} -- do not edit.\n")
        f.write(f"#ifndef NEOOS_WM_{prefix}_H\n#define NEOOS_WM_{prefix}_H\n")
        f.write("#include <stdint.h>\n")
        f.write(f"#define {prefix}_W {w}\n#define {prefix}_H {h}\n")
        f.write(f"static const uint8_t {prefix.lower()}_rgba[{prefix}_W * {prefix}_H * 4] = {{\n")
        f.write(",".join(str(b) for b in data))
        f.write("\n};\n#endif\n")
    print(f"wrote {out}: {w}x{h}")

if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Rasterize the SVG and generate the header**

```bash
cd /home/neo/projects/personal/neoos-wm
inkscape --export-type=png --export-width=28 --export-height=28 \
  --export-filename=assets/logo/logo28.png assets/logo/neoos-start-neon-ring.svg
python3 tools/png2c.py assets/logo/logo28.png assets/logo/logo.h LOGO
```

Expected: `wrote assets/logo/logo.h: 28x28`. Confirm the file exists
and `grep -c , assets/logo/logo.h` shows roughly `28*28*4 - 1 = 3135`
commas (one fewer than the pixel-byte count, since the last value has
no trailing comma).

- [ ] **Step 4: Copy the bitmap font from NeoOS**

```bash
cd /home/neo/projects/personal/neoos-wm
cp /home/neo/projects/personal/NeoOS/userland/term/font_term.c font_term.c
cp /home/neo/projects/personal/NeoOS/userland/term/font_term.h font_term.h
```

(These are committed, generated, dependency-free data files -- copied
verbatim, not referenced across repos, matching this project's existing
convention of copying rather than cross-repo-including build inputs,
e.g. `user.ld`.)

- [ ] **Step 5: Write `taskbar.c`**

```c
// neoos-wm/taskbar.c -- the desktop taskbar: Start button (ring logo)
// and a live clock, docked at the bottom of the screen, rendered as
// Liquid Glass at 50% intensity. See
// docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md.
#include "wmclient.h"
#include "wmproto.h"
#include "font_term.h"
#include "assets/logo/logo.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define TASKBAR_H 40
#define LOGO_MARGIN 6
#define CLOCK_MARGIN 12

static int menu_open = 0;

// Draws one glyph's ink pixels opaque (alpha=255) at (x0,y0); every
// other pixel in the glyph cell is left untouched (already cleared to
// transparent by the caller). Mirrors userland/term/render.c's bit
// extraction exactly -- do not invent a different convention.
static void draw_glyph(uint32_t *px, uint32_t stride, int x0, int y0, unsigned char ch) {
    const uint8_t *g = term_glyphs[ch];
    for (int gy = 0; gy < TERM_GLYPH_H; gy++) {
        for (int gx = 0; gx < TERM_GLYPH_W; gx++) {
            int ink = (g[gy * 2 + (gx >> 3)] >> (7 - (gx & 7))) & 1;
            if (ink) { px[(y0 + gy) * stride + (x0 + gx)] = 0xFFFFFFFF; }
        }
    }
}

// Writes the straight-alpha logo's pixels onto px at (x0,y0), skipping
// fully transparent source pixels. Not a real blend -- px's destination
// pixels here are always still at their just-cleared transparent-zero
// state (nothing else draws under the logo), so a direct copy-through
// of the source's own (r,g,b,a) is exactly correct, not an
// approximation of one.
static void draw_logo(uint32_t *px, uint32_t stride, int x0, int y0) {
    for (int row = 0; row < LOGO_H; row++) {
        for (int col = 0; col < LOGO_W; col++) {
            const uint8_t *p = &logo_rgba[(row * LOGO_W + col) * 4];
            uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
            if (a == 0) { continue; }
            px[(y0 + row) * stride + (x0 + col)] = ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void redraw(struct wm_conn *c, uint32_t screen_w) {
    uint32_t *px = wm_pixels(c);
    uint32_t stride = wm_stride_px(c);
    memset(px, 0, (size_t)stride * TASKBAR_H * 4);   // alpha 0 everywhere -- see spec section 4

    draw_logo(px, stride, LOGO_MARGIN, (TASKBAR_H - LOGO_H) / 2);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long total = ts.tv_sec % 86400;
    int hh = (int)(total / 3600);
    int mm = (int)((total / 60) % 60);
    unsigned char digits[5] = {
        (unsigned char)('0' + hh / 10), (unsigned char)('0' + hh % 10),
        ':',
        (unsigned char)('0' + mm / 10), (unsigned char)('0' + mm % 10),
    };
    int clock_w = 5 * TERM_GLYPH_W;
    int cx = (int)screen_w - CLOCK_MARGIN - clock_w;
    int cy = (TASKBAR_H - TERM_GLYPH_H) / 2;
    for (int i = 0; i < 5; i++) {
        draw_glyph(px, stride, cx + i * TERM_GLYPH_W, cy, digits[i]);
    }

    wm_damage(c, 0, 0, screen_w, TASKBAR_H);
    wm_commit(c);
}

// Reaps startmenu.nex without blocking the taskbar's own event loop --
// wait() is blocking (no non-blocking variant exists), so it runs on
// its own thread. Clears menu_open on return, whether the child exited
// because the user clicked Start again, clicked elsewhere (self-close
// on WM_FOCUS, see startmenu.c), or crashed.
static void *reap_menu(void *arg) {
    int pid = (int)(intptr_t)arg;
    wait(pid);
    menu_open = 0;
    return 0;
}

static int hit_start_button(int x, int y) {
    return x >= 0 && x < LOGO_MARGIN * 2 + LOGO_W && y >= 0 && y < TASKBAR_H;
}

int main(void) {
    struct wm_conn *c = wm_connect();
    if (!c) { printf("[taskbar] cannot reach the compositor\n"); return 1; }
    printf("[taskbar] connected\n");

    uint32_t screen_w = wm_screen_width(c);
    uint32_t screen_h = wm_screen_height(c);
    int32_t id = wm_create_surface_ex(c, 0, (int32_t)(screen_h - TASKBAR_H),
                                       screen_w, TASKBAR_H,
                                       WM_SURFACE_GLASS | WM_SURFACE_FIXED_POS,
                                       50, "taskbar");
    if (id < 0) { printf("[taskbar] create surface failed\n"); return 1; }
    printf("[taskbar] surface %d\n", id);

    redraw(c, screen_w);
    printf("[taskbar] ready\n");

    int last_minute = -1;
    int last_move_x = -1, last_move_y = -1;
    while (wm_alive(c)) {
        int t, a, b;
        while (wm_poll_event(c, &t, &a, &b) == 1) {
            if (t == WM_POINTER_MOTION) { last_move_x = a; last_move_y = b; }
            else if (t == WM_POINTER_BUTTON && b == 1 /* pressed */) {
                if (hit_start_button(last_move_x, last_move_y) && !menu_open) {
                    int pid = spawn("/usr/local/bin/startmenu.nex");
                    if (pid < 0) {
                        printf("[taskbar] spawn startmenu.nex failed\n");
                    } else {
                        menu_open = 1;
                        pthread_t reaper;
                        pthread_create(&reaper, 0, reap_menu, (void *)(intptr_t)pid);
                    }
                }
            }
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int minute = (int)((ts.tv_sec % 3600) / 60);
        if (minute != last_minute) {
            last_minute = minute;
            redraw(c, screen_w);
        }

        struct timespec sleep_ts = { 0, 100000000 };  // 100ms -- responsive clicks, cheap polling
        nanosleep(&sleep_ts, 0);
    }
    printf("[taskbar] done\n");
    wm_disconnect(c);
    return 0;
}
```

- [ ] **Step 6: Add `TASKBAR.ELF` to `neoos-wm/Makefile`**

Add a build rule modeled exactly on `WMDEMO.ELF`'s (libneoos-linked),
with the asset pipeline as prerequisites:

```makefile
# Ring-logo asset pipeline -- host-only tooling (inkscape, PIL),
# committed output. See docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md
# section 6.
assets/logo/logo28.png: assets/logo/neoos-start-neon-ring.svg
	inkscape --export-type=png --export-width=28 --export-height=28 \
	  --export-filename=$@ $<

assets/logo/logo.h: assets/logo/logo28.png tools/png2c.py
	python3 tools/png2c.py assets/logo/logo28.png $@ LOGO

$(BUILD_DIR)/TASKBAR.ELF: taskbar.c font_term.c font_term.h assets/logo/logo.h user.ld $(LIBNEOOS_DIR)/lib/crt0.o $(LIBNEOOS_DIR)/lib/libneoos.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(USER_CFLAGS) -T user.ld -o $@ $(LIBNEOOS_DIR)/lib/crt0.o taskbar.c font_term.c -L$(LIBNEOOS_DIR)/lib -lneoos
```

Add `$(BUILD_DIR)/TASKBAR.ELF` to the `all` target's prerequisite list.

- [ ] **Step 7: Build**

```bash
cd /home/neo/projects/personal/neoos-wm
make clean && make
```

Expected: `build/TASKBAR.ELF` produced alongside the existing
artifacts, no errors.

- [ ] **Step 8: Commit**

```bash
cd /home/neo/projects/personal/neoos-wm
git add assets/logo/ tools/png2c.py font_term.c font_term.h taskbar.c Makefile
git commit -m "wm: add the taskbar client -- Start button (ring logo) + live clock

Docked at the bottom of the screen (WM_SURFACE_FIXED_POS), rendered
glass at 50% intensity. Content is cleared to fully transparent every
redraw, then the logo (real alpha from the source PNG) and clock
digits (opaque ink pixels via the reused term_glyphs bitmap font) are
drawn on top -- the frosted backdrop shows through everywhere else.
Start click spawns startmenu.nex, reaped on a background pthread
(wait() has no non-blocking form) so the taskbar's own event loop
never stalls."
```

---

## Task 6: Start menu client (`startmenu.nex`)

**Files:**
- Create: `neoos-wm/startmenu.c`
- Modify: `neoos-wm/Makefile`

**Interfaces:**
- Consumes: `wm_create_surface_ex` (Task 1), `WM_FORMAT_ARGB8888`
  compositing (Task 4).
- Produces: `/usr/local/bin/startmenu.nex` (installed by Task 7),
  spawned by `taskbar.c` (Task 5) at that fixed path.

- [ ] **Step 1: Write `startmenu.c`**

```c
// neoos-wm/startmenu.c -- the start menu: an empty glass panel this
// round. Opens already-focused (WM_CREATE_SURFACE grants focus to any
// new non-shell surface, Task 3); closes itself the instant it
// receives WM_FOCUS{focused=0} -- clicking Start again or clicking
// anywhere else both look identical from here: focus moved away. See
// docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md.
#include "wmclient.h"
#include "wmproto.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MENU_W 320
#define MENU_H 420
#define TASKBAR_H 40

int main(void) {
    struct wm_conn *c = wm_connect();
    if (!c) { printf("[startmenu] cannot reach the compositor\n"); return 1; }
    printf("[startmenu] connected\n");

    uint32_t screen_h = wm_screen_height(c);
    int32_t id = wm_create_surface_ex(c, 0, (int32_t)(screen_h - TASKBAR_H - MENU_H),
                                       MENU_W, MENU_H,
                                       WM_SURFACE_GLASS | WM_SURFACE_FIXED_POS,
                                       50, "start-menu");
    if (id < 0) { printf("[startmenu] create surface failed\n"); return 1; }
    printf("[startmenu] surface %d\n", id);

    uint32_t *px = wm_pixels(c);
    uint32_t stride = wm_stride_px(c);
    memset(px, 0, (size_t)stride * MENU_H * 4);   // fully transparent -- empty content this round
    wm_damage(c, 0, 0, MENU_W, MENU_H);
    wm_commit(c);
    printf("[startmenu] mapped\n");

    while (wm_alive(c)) {
        int t, a, b;
        (void)b;
        while (wm_poll_event(c, &t, &a, &b) == 1) {
            if (t == WM_FOCUS && a == 0) {
                printf("[startmenu] closing on focus loss\n");
                wm_disconnect(c);
                return 0;
            }
        }
        struct timespec sleep_ts = { 0, 50000000 };  // 50ms
        nanosleep(&sleep_ts, 0);
    }
    printf("[startmenu] done\n");
    wm_disconnect(c);
    return 0;
}
```

- [ ] **Step 2: Add `STARTMENU.ELF` to `neoos-wm/Makefile`**

Modeled exactly on `WMDEMO.ELF`'s rule (no assets, no extra sources):

```makefile
$(BUILD_DIR)/STARTMENU.ELF: startmenu.c user.ld $(LIBNEOOS_DIR)/lib/crt0.o $(LIBNEOOS_DIR)/lib/libneoos.a
	@mkdir -p $(BUILD_DIR)
	$(CC) $(USER_CFLAGS) -T user.ld -o $@ $(LIBNEOOS_DIR)/lib/crt0.o startmenu.c -L$(LIBNEOOS_DIR)/lib -lneoos
```

Add `$(BUILD_DIR)/STARTMENU.ELF` to the `all` target's prerequisite list.

- [ ] **Step 3: Build**

```bash
cd /home/neo/projects/personal/neoos-wm
make clean && make
```

Expected: `build/STARTMENU.ELF` produced alongside every other
artifact, no errors.

- [ ] **Step 4: Commit**

```bash
cd /home/neo/projects/personal/neoos-wm
git add startmenu.c Makefile
git commit -m "wm: add the start menu client -- an empty glass panel

Docked bottom-left above the taskbar. Fully transparent content this
round (no menu items yet, by design -- see the spec's Non-goals).
Self-closes on WM_FOCUS{focused=0}, which fires whether the user
clicked Start again or clicked anywhere else -- both are 'focus moved
away' from this surface's point of view, so no signal from the
taskbar is needed to close it."
```

---

## Task 7: Install into the disk image and end-to-end target verification

**Files:**
- Modify: `NeoOS/Makefile`
- Create: `NeoOS/tools/inject_click.sh`

**Interfaces:**
- Consumes: `$(WM_DIR)/build/TASKBAR.ELF`, `$(WM_DIR)/build/STARTMENU.ELF` (Tasks 5-6).

- [ ] **Step 1: Install `taskbar.nex`/`startmenu.nex` alongside `wm.nex`**

In `NeoOS/Makefile`'s `$(DISK_IMG)` recipe, immediately after the
existing conditional block that installs `wm.nex` and the cursor asset
(the `if [ -f "$(WM_DIR)/build/WM.ELF" ]` block from the cursor-theme
milestone), add a second conditional, gated on `TASKBAR.ELF` existing
(a headless/no-desktop image, or a `neoos-wm` checkout that predates
this milestone, gets neither -- "you get what you built" applies here
exactly as it does for the compositor itself):

```makefile
	@if [ -f "$(WM_DIR)/build/TASKBAR.ELF" ]; then \
		mmd -i $(DISK_IMG) ::usr/local 2>/dev/null || true; \
		mmd -i $(DISK_IMG) ::usr/local/bin 2>/dev/null || true; \
		./tools/nexify.sh $(WM_DIR)/build/TASKBAR.ELF $(BUILD_DIR)/taskbar.nex; \
		./tools/nexify.sh $(WM_DIR)/build/STARTMENU.ELF $(BUILD_DIR)/startmenu.nex; \
		mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/taskbar.nex ::usr/local/bin/taskbar.nex; \
		mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/startmenu.nex ::usr/local/bin/startmenu.nex; \
		echo "disk: taskbar.nex and startmenu.nex installed"; \
	else \
		echo "disk: no TASKBAR.ELF at $(WM_DIR)/build -- no taskbar"; \
	fi
```

- [ ] **Step 2: Write `tools/inject_click.sh`, a small QEMU-monitor click-injection helper**

Reusable beyond this one test -- any future target that needs to
simulate a mouse click can use it. Mirrors `tools/screenshot.sh`'s
existing monitor-socket connection technique.

```bash
#!/bin/bash
# tools/inject_click.sh SOCK dx dy
#
# Moves the PS/2 mouse by (dx,dy) relative to its current position
# (broken into <=20px steps -- QEMU's PS/2 emulation does not
# guarantee a single large mouse_move is delivered as one relative
# packet) then clicks and releases the left button. SOCK is the QEMU
# monitor unix socket, e.g. from tools/screenshot.sh's own $SOCK.
set -u
SOCK="$1"; DX="$2"; DY="$3"
STEP=20
{
  n=0
  rx=$DX; ry=$DY
  while [ "${rx#-}" -gt 0 ] || [ "${ry#-}" -gt 0 ]; do
    sx=$STEP; [ "${rx#-}" -lt $STEP ] && sx=${rx#-}
    sy=$STEP; [ "${ry#-}" -lt $STEP ] && sy=${ry#-}
    [ "$rx" -lt 0 ] && sx=$((-sx))
    [ "$ry" -lt 0 ] && sy=$((-sy))
    printf 'mouse_move %d %d\n' "$sx" "$sy"
    rx=$((rx - sx)); ry=$((ry - sy))
    n=$((n + 1))
    [ $n -gt 200 ] && break   # safety valve against a logic error above
  done
  printf 'mouse_button 1\n'
  printf 'mouse_button 0\n'
} | nc -q 1 -U "$SOCK" >/dev/null 2>&1
```

- [ ] **Step 3: Build the disk image and confirm the install step ran**

```bash
cd /home/neo/projects/personal/NeoOS
rm -f build/disk.img
make WM_DIR=../neoos-wm disk-image
mdir -i build/disk.img ::usr/local/bin
```

Expected: console output includes `disk: taskbar.nex and startmenu.nex
installed`, and the `mdir` listing shows both `taskbar.nex` and
`startmenu.nex` alongside `wm.nex`.

- [ ] **Step 4: Add the `wm-taskbar` end-to-end target**

Add near `wm-cursor`/`wm-cursor-fallback` (which already established
the short-local-`timeout` pattern -- reused here for the same reason:
`wm.nex`'s frame budget does not advance once idle, so a short local
bound, not `$(BOOT_TIMEOUT)`, is what actually ends this run):

```makefile
# `make wm-taskbar` boots the compositor with the taskbar (which is
# what actually owns the screen this time, not wmdemo), confirms the
# taskbar mapped, drives a synthetic click at the Start button to open
# the menu, confirms it opened, drives a second click on bare desktop,
# confirms the menu closed on focus loss (real WM_FOCUS delivery, Task
# 2), and screenshots throughout. Smoke-tests
# docs/superpowers/specs/2026-09-22-wm-taskbar-start-menu-design.md
# end to end.
TASKBAR_ELF   := $(WM_DIR)/build/TASKBAR.ELF
STARTMENU_ELF := $(WM_DIR)/build/STARTMENU.ELF

.PHONY: wm-taskbar
wm-taskbar: iso disk-image
	@test -f $(WM_ELF) || { echo "wm-taskbar: $(WM_ELF) missing -- build neoos-wm first (WM_DIR=$(WM_DIR))"; exit 1; }
	@test -f $(TASKBAR_ELF) || { echo "wm-taskbar: $(TASKBAR_ELF) missing -- build neoos-wm first (WM_DIR=$(WM_DIR))"; exit 1; }
	@test -f $(STARTMENU_ELF) || { echo "wm-taskbar: $(STARTMENU_ELF) missing -- build neoos-wm first (WM_DIR=$(WM_DIR))"; exit 1; }
	./tools/nexify.sh $(WM_ELF) $(BUILD_DIR)/wm.nex
	./tools/nexify.sh $(TASKBAR_ELF) $(BUILD_DIR)/taskbar.nex
	./tools/nexify.sh $(STARTMENU_ELF) $(BUILD_DIR)/startmenu.nex
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/wm.nex ::wm.nex
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/taskbar.nex ::taskbar.nex
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/startmenu.nex ::startmenu.nex
	@printf '%s\n' \
	  '# generated by `make wm-taskbar`' \
	  'spawn /wm.nex' \
	  'spawn /taskbar.nex' \
	  > $(BUILD_DIR)/disk-src/INITTAB.solo
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/INITTAB.solo ::etc/inittab
	@SOCK=$$(mktemp -u /tmp/neoos-qmon-XXXX.sock); \
	qemu-system-x86_64 -cpu Nehalem -smp 4 -boot order=d \
	  -cdrom build/neoos.iso -drive file=$(DISK_IMG),format=raw \
	  -drive file=$(DISK2_IMG),format=raw -vga std \
	  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	  -audiodev none,id=a0 -device AC97,audiodev=a0,addr=0x6 \
	  -no-reboot -display none -serial file:$(BUILD_DIR)/wm-taskbar.log \
	  -monitor "unix:$$SOCK,server,nowait" & \
	QPID=$$!; \
	sleep 3; \
	tools/inject_click.sh "$$SOCK" -620 380; \
	sleep 1; \
	printf 'screendump $(BUILD_DIR)/wm-taskbar-menu-open.ppm\n' | nc -q 1 -U "$$SOCK" >/dev/null 2>&1; \
	tools/inject_click.sh "$$SOCK" 880 -680; \
	sleep 1; \
	printf 'screendump $(BUILD_DIR)/wm-taskbar-menu-closed.ppm\n' | nc -q 1 -U "$$SOCK" >/dev/null 2>&1; \
	sleep 1; \
	kill $$QPID 2>/dev/null; wait $$QPID 2>/dev/null; \
	rm -f "$$SOCK"
	@grep -E '^\[wm\]|^\[taskbar\]|^\[startmenu\]|\[fault-audit\]' $(BUILD_DIR)/wm-taskbar.log || true
	@if grep -qE 'PANIC|\[exception\]' $(BUILD_DIR)/wm-taskbar.log; then \
		echo "WM-TASKBAR: the KERNEL did not survive"; \
		grep -E 'PANIC|\[exception\]' $(BUILD_DIR)/wm-taskbar.log; exit 1; fi
	@if ! grep -q '\[taskbar\] ready' $(BUILD_DIR)/wm-taskbar.log; then \
		echo "WM-TASKBAR FAILED: taskbar never became ready"; tail -20 $(BUILD_DIR)/wm-taskbar.log; exit 1; fi
	@if ! grep -q '\[startmenu\] mapped' $(BUILD_DIR)/wm-taskbar.log; then \
		echo "WM-TASKBAR FAILED: start menu never opened -- Start click missed or spawn failed"; tail -20 $(BUILD_DIR)/wm-taskbar.log; exit 1; fi
	@if ! grep -q '\[startmenu\] closing on focus loss' $(BUILD_DIR)/wm-taskbar.log; then \
		echo "WM-TASKBAR FAILED: start menu never closed on outside click"; tail -20 $(BUILD_DIR)/wm-taskbar.log; exit 1; fi
	@echo "WM-TASKBAR: screenshots at $(BUILD_DIR)/wm-taskbar-menu-open.ppm and $(BUILD_DIR)/wm-taskbar-menu-closed.ppm"
```

The click coordinates assume the cursor starts at screen center
(`cur_x=fb_w/2, cur_y=fb_h/2`, `wm.c`'s existing startup behavior) and
a 1280x800 screen (confirmed the actual resolution this project boots
at in every other target's serial log): center (640,400) to the Start
button (~20,780) is `(-620, +380)`; from there to a bare-desktop spot
(~900,100) is `(+880, -680)`. **These are concrete starting values,
not guesses to leave unverified** -- Step 5 below runs the target and
requires confirming the log lines actually appear; if the click misses
(most likely cause: PS/2 relative-motion scaling in QEMU's emulation
does not map 1:1 to `wm.c`'s pixel-accumulation, since nothing in this
codebase has exercised monitor-injected mouse motion before this
task), adjust the two delta pairs and re-run rather than treating a
miss as a `wm.c` bug.

- [ ] **Step 5: Run it and confirm it passes**

```bash
cd /home/neo/projects/personal/NeoOS
chmod +x tools/inject_click.sh
make WM_DIR=../neoos-wm wm-taskbar
```

Expected: exits 0, with `[taskbar] ready`, `[startmenu] mapped`, and
`[startmenu] closing on focus loss` all present in the log. Open both
PPM screenshots: the first should show the start menu's glass panel
open above the taskbar; the second should show it gone, taskbar alone.
If the log shows `[taskbar] ready` but never `[startmenu] mapped`,
recalibrate the first click's coordinates (see Step 4's note) and
re-run before concluding anything else is broken.

- [ ] **Step 6: Commit**

```bash
cd /home/neo/projects/personal/NeoOS
git add Makefile tools/inject_click.sh
git commit -m "$(cat <<'EOF'
build: install taskbar.nex/startmenu.nex; add wm-taskbar end-to-end test

wm-taskbar boots the compositor with the real taskbar, drives two
synthetic clicks via a new reusable QEMU-monitor click-injection
helper (tools/inject_click.sh) to open and then close the start menu,
and confirms both transitions and a taskbar-only boot via the serial
log plus before/after screenshots.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Document the protocol extensions in `docs/stdlib.md`

**Files:**
- Modify: `NeoOS/docs/stdlib.md`

**Interfaces:** none -- documentation only.

- [ ] **Step 1: Update the window-system section**

The window-system section (`## The window system: neoos-wm and
wmclient (GUI stack G4-G6)`) currently ends its `wm_create_glass_window`
paragraph with a sentence that is no longer accurate: "the wire
protocol carries no extra message or field for it" -- it now does
(`glass_intensity`, and implicitly the pixel format). Replace that
paragraph and the sentence right after it:

```
`wm_create_glass_window(c, w, h, title)` is identical to
`wm_create_window` in every way a client can observe -- same
`memfd`/`SCM_RIGHTS` setup, same `wm_pixels`/`wm_damage`/`wm_commit`
draw cycle -- except the compositor composites the resulting surface
through its built-in Liquid-Glass shader (`WM_SURFACE_GLASS` in
`wmproto.h`): it snapshots whatever is behind the surface's screen
rect, refracts and tints it, and draws the client's own content on top
of that lensed backdrop instead of a flat opaque blit. This is purely
a compositor-side rendering choice; the wire protocol carries no extra
message or field for it.
```
with:
```
`wm_create_glass_window(c, w, h, title)` is identical to
`wm_create_window` in every way a client can observe -- same
`memfd`/`SCM_RIGHTS` setup, same `wm_pixels`/`wm_damage`/`wm_commit`
draw cycle -- except the compositor composites the resulting surface
through its built-in Liquid-Glass shader (`WM_SURFACE_GLASS` in
`wmproto.h`): it snapshots whatever is behind the surface's screen
rect, refracts and tints it, and draws the client's own content on top
of that lensed backdrop instead of a flat opaque blit.

`wm_create_surface_ex(c, x, y, w, h, flags, glass_intensity, title)` is
the general entry point `wm_create_window`/`wm_create_glass_window`/
`wm_create_shell` are thin wrappers over:

- `x, y` are honored only when `flags` includes `WM_SURFACE_FIXED_POS`
  -- the compositor positions the surface exactly there instead of its
  usual cascade placement. Meant for docked desktop chrome (a taskbar,
  a popup menu), not ordinary application windows.
- `glass_intensity` (0-100) is honored only when `flags` includes
  `WM_SURFACE_GLASS`: 100 (the default every existing caller gets) is
  the shader's full, unmodified refraction+tint effect; lower values
  blend the shaded result back toward the original unshaded backdrop,
  for a lighter frost that leaves more of what is behind the surface
  recognizable.
- Any `WM_SURFACE_GLASS` surface's pixel buffer is automatically
  `WM_FORMAT_ARGB8888` (real, straight, per-pixel alpha in the top
  byte) rather than the opaque `WM_FORMAT_XRGB8888` every other
  surface uses -- a client never requests this directly. A fully
  transparent pixel (alpha 0) leaves the shaded backdrop showing
  through; alpha 255 is fully opaque; anything between blends. This is
  what makes a mostly-see-through glass panel with a few fully-opaque
  UI elements on it possible -- `glass_intensity` alone only controls
  how strong the lensing looks *underneath* whatever is opaque, not
  how much of the surface is opaque in the first place.
```

- [ ] **Step 2: Commit**

```bash
cd /home/neo/projects/personal/NeoOS
git add docs/stdlib.md
git commit -m "$(cat <<'EOF'
docs: document wm_create_surface_ex, fixed-position surfaces, and glass alpha

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Plan Self-Review Notes

- **Spec coverage**: Goals (taskbar docked+glass+logo+clock, start
  menu open/close both directions, 50% intensity both surfaces,
  general per-surface intensity) -> Tasks 1, 3, 5, 6. Design section
  1 (protocol) -> Task 1. Section 2 (WM_FOCUS) -> Task 2. Section 3
  (intensity blend) -> Task 3. Section 3b (alpha compositing, found
  during planning) -> Task 4. Sections 4-5 (taskbar) -> Task 5.
  Section 6 (logo asset) -> Task 5, Steps 1-3. Section 7 (start menu)
  -> Task 6. Cross-repo scope -> Tasks 1-8 collectively (every listed
  file appears in some task's Files list). Error handling (spawn
  failure, connect failure, intensity clamping, asset build failure,
  format rejection) -> Tasks 3-6 inline in the relevant code. Testing
  (intensity-blend bound check, alpha regression, end-to-end click
  test, clock correctness) -> Tasks 3-4 build/regression steps + Task
  7. The spec's one remaining Testing bullet not directly covered by a
  task -- "Clock correctness... sampled via screenshot at a known
  injected time (QEMU's `-rtc base=...` flag)" -- is a refinement of
  Task 7's existing screenshot capture, not a separate mechanism; a
  future pass can add an explicit `-rtc base=` override to `wm-taskbar`
  if clock-accuracy regressions become a real concern, without needing
  a new task structure.
- **Placeholder scan**: no TBD/TODO; the one place a "verify and
  adjust" instruction appears (Task 7's click coordinates) carries
  concrete starting numbers and a concrete reason a miss might happen,
  not a deferred decision.
- **Type consistency**: `wm_create_surface_ex`'s signature is
  identical everywhere it appears (Task 1's wmclient.h declaration,
  Task 1's wmclient.c definition, Task 5/6's taskbar.c/startmenu.c call
  sites). `WM_SURFACE_FIXED_POS`/`WM_FORMAT_ARGB8888`'s values (4, 2)
  are defined once (Tasks 1 and 4 respectively) and never redefined.
  `struct client`'s three new fields (`fixed_pos` in Task 3,
  `glass_intensity` in Task 3, `format` in Task 4) are each added
  exactly once and referenced consistently by name in every later use.
