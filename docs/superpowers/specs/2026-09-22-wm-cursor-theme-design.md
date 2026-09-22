# Xcursor-themed mouse pointer in neoos-wm

## Context

This is sub-project 1 of the "Windows-inspired desktop environment"
roadmap (a new roadmap, separate from the Liquid-Glass sub-projects
1-2 already shipped -- see
`docs/superpowers/specs/2026-09-20-wm-glass-lensing-design.md`). The
staged plan agreed with the user:

1. **Cursor theme** (this spec) -- swap the compositor's hardcoded
   1-bit cursor bitmap for a real themed pointer.
2. Desktop shell v1 -- wallpaper + a static taskbar.
3. Window chrome -- draggable title bars, close/minimize buttons.
4. Taskbar interactivity -- running-window list, start menu.

Sub-projects 2-4 are out of scope here.

Today, `neoos-wm/wm.c` draws the pointer from a hardcoded 10x14 1-bit
arrow (`cursor_bits[]`, OR'd into `back`, no alpha). This spec replaces
it with a cursor decoded from a real **Xcursor binary theme** --
specifically the `gm_cursors` theme
(https://git.gianmarco.gg/gianmarco/gm-cursors, GPLv3), which the user
supplied as `gm_cursors.tar.xz`.

## Goals

- `neoos-wm` loads and displays the `default` cursor from the
  `gm_cursors` Xcursor theme at startup, alpha-blended against the
  desktop, replacing the hardcoded bitmap.
- The underlying loader parses the **general Xcursor binary format**
  (not a `gm_cursors`-specific hack), so any standard Xcursor theme
  dropped into the same install path works, and loading additional
  named cursors later (`pointer`, resize cursors, etc., for
  sub-projects 3-4) is a one-line call, not new parsing code.
- The theme is installed at a general-purpose path
  (`/usr/share/icons/<theme>/cursors/<name>`) mirroring the real-world
  Xcursor theme layout, not hardcoded to a single file.
- `gm_cursors`' compiled Xcursor binaries are committed into
  `neoos-wm` as-is (no SVG rasterization needed -- the binaries already
  carry pre-rendered ARGB32 pixels + hotspot metadata at multiple
  sizes), with attribution to
  https://git.gianmarco.gg/gianmarco/gm-cursors and its GPLv3 license
  carried alongside.

## Non-goals

- Cursor switching by context (hand cursor over a button, resize
  cursor over a window edge, etc.). Nothing in `neoos-wm` today knows
  "what's under the pointer" in a way that would make this
  meaningful -- that lands with sub-projects 3-4 (chrome/taskbar).
  This milestone loads and shows exactly one cursor: `default`.
  Wiring more cursors in later is meant to be an easy follow-up given
  this loader, but is not done here.
- Animated cursors (the theme's `progress` spinner frames). Not
  attempted.
- User-configurable / runtime-swappable themes (a settings UI, reading
  a theme name from config). Only the fixed path
  `/usr/share/icons/gm_cursors/cursors/default` is read; changing
  theme means changing that hardcoded path and rebuilding, for now.
- SVG rasterization or any on-target/build-time SVG tooling. The
  `cursors_scalable/` half of the archive is not used at all -- only
  the pre-compiled `cursors/` binaries.
- DPI/scale-aware cursor size selection. One fixed target size is
  picked (see Design); no notion of display scale exists in `neoos-wm`
  today to make this meaningful.

## Design

### 1. Asset layout and licensing

`neoos-wm/assets/icons/gm_cursors/` gains:

```
assets/icons/gm_cursors/
  cursors/
    default              # compiled Xcursor binary, copied verbatim from the archive
  LICENSE                # GPLv3, verbatim from the archive
  CREDITS                # one line: theme name + https://git.gianmarco.gg/gianmarco/gm-cursors
```

Only `default` is committed. The full theme is 38MB (the `progress`/
`wait` animated cursors alone are 10MB each -- many size variants times
many animation frames); committing all ~30 unused files now for a
one-cursor milestone is a real, permanent repo-size cost for zero
present benefit, not "free" as an earlier draft of this spec assumed.
Sub-projects 3-4 commit whichever additional named cursors they
actually wire up, when they wire them up.

### 2. Xcursor binary parser (`neoos-wm/xcursor.h` / `xcursor.c`)

New, wholly-ours module. No OS dependency beyond `open`/`read`/`malloc`
(already available via musl), so it is written and tested as portable
C, matching the tinygl-spec precedent of host-testable format parsers.

Public surface:

```c
typedef struct {
    uint32_t width, height;
    int32_t  hot_x, hot_y;
    uint32_t *argb;   // width*height, straight (non-premultiplied) ARGB32, malloc'd
} xcursor_image_t;

// Reads `path`, picks the image chunk whose declared nominal size is
// closest to `target_size`, decodes it. Returns 0 and fills `out` on
// success; returns -1 and logs the reason on any failure (bad magic,
// empty/corrupt TOC, chunk length that doesn't fit the file, OOM).
// Never partially fills `out` on failure.
int xcursor_load(const char *path, uint32_t target_size, xcursor_image_t *out);

void xcursor_free(xcursor_image_t *img);
```

Format handled (per the Xcursor binary spec):
- File header: magic `Xcur`, header size, version, TOC count.
- TOC: `(type, subtype, offset)` triples; only
  `type == 0xfffd0002` (image chunks) are consulted -- `subtype` is
  the chunk's nominal pixel size.
- Image chunk: `(header, type, subtype, version, width, height,
  xhot, yhot, delay, pixels[width*height] ARGB32)`.

Only single-frame selection is implemented (pick one TOC entry by
nominal size); the `delay` field (animation) is read but unused, per
Non-goals.

**Target size**: 24px, matching the theme's own documented nominal
size and sitting proportionally against the existing 18px title bar /
2px border metrics `wm.c` already uses elsewhere.

### 3. `wm.c` integration

- At startup (alongside the existing `ZB_open`/`glInit` one-time
  setup), call `xcursor_load("/usr/share/icons/gm_cursors/cursors/default", 24, &g_cursor)`.
  On failure, log the reason and leave a `g_cursor.argb == NULL`
  sentinel -- `draw_cursor()` checks this and falls back to the
  existing hardcoded `cursor_bits[]` path unchanged. **Never a hard
  boot failure over a missing/corrupt cursor asset.**
- `CURSOR_W`/`CURSOR_H` (currently compile-time `#define`s used for
  damage-rect sizing) become runtime reads of `g_cursor.width` /
  `g_cursor.height` when a themed cursor is loaded, falling back to
  the old `10`/`14` constants otherwise -- damage tracking must cover
  the real footprint either way.
- `draw_cursor()` gains a themed path: for each pixel of `g_cursor.argb`,
  alpha-blend into `back` at
  `(cur_x - g_cursor.hot_x + col, cur_y - g_cursor.hot_y + row)`, with
  the same on-screen bounds clipping `fill()`/the existing cursor path
  already do. **Xcursor pixel data is premultiplied alpha** (confirmed
  against the real `gm_cursors/cursors/default` file: every fully
  transparent pixel has RGB bytes of zero) -- the blend is therefore
  `dst.rgb = src.rgb + dst.rgb * (255 - src.a) / 255` per channel, NOT
  the straight-alpha `src.rgb * src.a + ...` formula, since the file's
  `src.rgb` is already scaled by its own alpha. Using the straight-alpha
  formula against premultiplied data would double-darken every
  partially-transparent pixel (the antialiased edges and the drop
  shadow). The old 1-bit path stays as the fallback branch, not
  deleted.

### 4. Install path (`NeoOS/Makefile`)

`neoos-wm/Makefile` only ever builds artifacts (`WM.ELF`,
`libwmclient.a`, `WMDEMO.ELF`) -- disk-image assembly is NeoOS's job,
the same division already documented at the top of
`neoos-wm/Makefile`. So the new install step belongs in
`NeoOS/Makefile`'s `$(DISK_IMG)` recipe, inside the existing
`if [ -f "$(WM_DIR)/build/WM.ELF" ]` block that conditionally installs
`wm.nex` (this block is the "you get what you built" signal -- no
`WM.ELF`, no compositor, no cursor asset either):

```
mmd -i $(DISK_IMG) ::usr/share/icons 2>/dev/null || true
mmd -i $(DISK_IMG) ::usr/share/icons/gm_cursors 2>/dev/null || true
mmd -i $(DISK_IMG) ::usr/share/icons/gm_cursors/cursors 2>/dev/null || true
mcopy -o -i $(DISK_IMG) $(WM_DIR)/assets/icons/gm_cursors/cursors/default \
  ::usr/share/icons/gm_cursors/cursors/default
```

This follows the existing `mmd`/`mcopy` convention already used for
`::usr/share/test/...`. The general `<theme>/cursors/<name>` shape
(not a flat `default` file at the top level) is deliberate: it is the
real Xcursor theme layout, so a later milestone that installs more
cursor names, or a second theme, needs no path redesign.

## Cross-repo scope

Implementation touches only **neoos-wm**:
- `assets/icons/gm_cursors/` (new, committed binary assets + license/credits)
- `xcursor.h`, `xcursor.c` (new)
- `wm.c` (`draw_cursor()` themed path, startup load, runtime
  `CURSOR_W`/`CURSOR_H`)

**NeoOS**: `Makefile`'s `$(DISK_IMG)` recipe, inside the existing
conditional `WM.ELF`-found block, gains the `mmd`/`mcopy` steps from
section 4 that install the cursor asset alongside `wm.nex`. Unlike the
glass-lensing milestone's `TINYGL_DIR` (an internal build dependency
of `wm` itself, invisible to NeoOS), a disk-image *data file* has to
be installed by whichever Makefile actually assembles the disk image
-- that is NeoOS's, not neoos-wm's.

**docs/stdlib.md**: none -- this changes no client-visible protocol or
syscall-adjacent surface. `wmproto.h` is untouched.

## Error handling

- Missing file / bad magic / empty TOC / a TOC entry claiming an
  offset or length past EOF: `xcursor_load` returns -1 with a logged
  reason; `wm.c` falls back to the hardcoded bitmap and boots normally.
- `malloc` failure during decode: same fallback, logged.
- A TOC with image chunks but none matching `type == 0xfffd0002`
  (theoretically possible per the format, though not expected in a
  well-formed theme): treated the same as an empty TOC -- fallback,
  logged, not a crash.

## Testing

Matches the tinygl/glass precedent's two-tier split:

- **Host-native (`gcc`, no QEMU)**: `xcursor.c` unit tests --
  well-formed multi-size TOC selects the entry closest to a given
  target size; malformed magic/TOC/chunk-length is rejected without
  reading past the buffer; a real extracted `gm_cursors` `default`
  file (checked into the test fixtures) decodes to the expected
  width/height/hotspot at target size 24.
- **Target (headless QEMU + `wm-shot` framebuffer screenshot)**:
  confirm the on-screen cursor is the themed arrow (not the old 1-bit
  bitmap) with visible alpha-blended antialiasing/shadow, and that its
  hotspot aligns correctly (e.g. clicking at a known cursor position
  still hits the window `wm`'s existing click-routing test already
  exercises -- hotspot offset errors would show up as an off-by-N
  click miss).
- **Fallback path**: boot with the asset path deliberately missing
  (rename it in a test fixture image) and confirm `wm` logs the
  fallback reason and still boots with the old hardcoded cursor
  rather than crashing or hanging.

## Open questions for planning

- Exact byte-for-byte TOC/image-chunk field layout should be verified
  against the real extracted `gm_cursors/cursors/default` file (26 TOC
  entries, sizes 12-96px, confirmed present) during implementation,
  not re-derived from the format spec alone -- use that file as the
  primary fixture.
- Whether `wm-shot`/`wm-run` local-iteration Makefile targets need any
  passthrough for the new asset path, or whether the existing `WM_DIR`
  plumbing already covers it -- a convenience question for the plan,
  not a design blocker.
