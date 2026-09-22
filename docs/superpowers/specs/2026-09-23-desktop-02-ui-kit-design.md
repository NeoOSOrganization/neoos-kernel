# UI toolkit: `neoos-ui-kit`

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestones
M2 (core) and M11 (remaining components). Research behind the
build-vs-reuse choices: `2026-09-23-desktop-03-lvgl-ui-libraries-survey.md`.

## Goals

- One themed, consistent look for every NeoOS GUI program: the shell,
  the start menu, Task Manager, Notepad, Terminal and anything later.
- A small C library, **`libnui.a`**, on top of LVGL 9.2 as vendored by
  `neoos-lvgl`, in its own repository **`neoos-ui-kit`**.
- **Design tokens as the single source of truth** for colour, spacing,
  radii, typography and elevation — consumed by the C theme *and* by the
  compositor's window chrome (spec 05), so the two can never disagree.
- Light and dark themes, switchable at runtime without restarting apps.
- Components: Button, List, Radio, Checkbox, Grid (the brief's five),
  plus TextInput, Dropdown, Dialog/Modal, Tabs, Scrollbar, Tooltip, Menu,
  and the two composites the tools need: FileDialog and Toolbar.
- A **gallery app** (`nui-gallery`) that shows every component in every
  state — the Storybook equivalent and the visual regression target.
- The glue every app otherwise re-writes: an LVGL display bound to a
  `neoos-wm` surface, input from WM events, one event loop.
- C# bindings generated over the same header (spec 07).

## Non-goals

- A layout language, XML/declarative UI, or a visual editor (survey:
  none fits; LVGL XML is 9.3+ and licence-restricted for tooling).
- Replacing LVGL widgets that already work. The kit is a *consistency
  layer*: tokens, a theme, variants, and a few composites.
- Accessibility APIs (screen readers). NeoOS has none to talk to. The
  kit commits to keyboard operability and contrast targets instead (see
  "Accessibility").
- Internationalisation beyond UTF-8 text in Latin scripts: fonts are
  limited to the glyph ranges baked in (open question).
- Animations beyond LVGL's defaults for pressed/checked transitions.

## Repository

`NeoOSOrganization/neoos-ui-kit`:

```
neoos-ui-kit/
  README.md, ARCHITECTURE.md, CHANGELOG.md
  Makefile                  # -> build/libnui.a, build/nui-gallery.elf
  tokens/tokens.json        # the design tokens (source of truth)
  tools/gen_tokens.py       # host: tokens.json -> generated/{nui_tokens.h, wm-theme.conf}
  include/nui.h             # the whole public C API (one header)
  include/nui_tokens.h      # generated, committed (so a build needs no Python)
  src/theme.c               # lv_theme_t implementation, light/dark
  src/app.c                 # nui_app: wm surface <-> lv_display, event loop
  src/button.c list.c check.c radio.c grid.c textinput.c dropdown.c
  src/dialog.c tabs.c tooltip.c menu.c scrollbar.c filedialog.c toolbar.c
  src/fonts/                # generated LVGL fonts (see Typography)
  gallery/gallery.c         # nui-gallery
  bindings/csharp/          # NeoOS.UIKit (spec 07): generated + hand-written layer
  test/                     # host tests (token generator, pure logic)
```

Build: a plain `Makefile` (every NeoOS repo uses Make; no CMake/Meson),
with `LVGL_DIR` and `WM_DIR` overridable exactly like `neoos-lvgl`'s
`WMCLIENT`/`NEOOS_TOOLCHAIN` knobs:

```
make LVGL_DIR=../neoos-lvgl WM_DIR=../neoos-wm   # -> build/libnui.a, build/nui-gallery.elf
```

Flags follow `neoos-lvgl`'s (`-mcmodel=large -fno-pic` while LVGL is
built that way, so objects link together) — whatever `neoos-lvgl`
chooses, the kit matches; it never diverges from the library it
extends. The kit uses LVGL's `lv_conf.h` from `LVGL_DIR`; it requires
`LV_USE_THEME_DEFAULT 1` (parent theme), `LV_USE_GRID 1`,
`LV_USE_FLEX 1`, `LV_COLOR_DEPTH 32`, and the fonts listed below. A
compile-time check in `nui.h` fails the build with a message if a
required `LV_USE_*` is off.

## Design tokens

`tokens/tokens.json` — a flat, typed token file (W3C design-tokens
format, reduced to what is used):

```json
{
  "color": {
    "light": { "bg": "#F3F4F6", "surface": "#FFFFFF", "surface-alt": "#E9EBEF",
               "text": "#1B1F24", "text-muted": "#5B6470", "border": "#C9CED6",
               "accent": "#2F6FEB", "accent-text": "#FFFFFF", "danger": "#C62828",
               "focus-ring": "#2F6FEB", "selection": "#CFE0FF" },
    "dark":  { "bg": "#1E2832", "surface": "#26313C", "surface-alt": "#2F3B47",
               "text": "#E6EAEE", "text-muted": "#9AA5B1", "border": "#3C4855",
               "accent": "#58A6FF", "accent-text": "#0B1016", "danger": "#FF6B6B",
               "focus-ring": "#58A6FF", "selection": "#1F3B61" }
  },
  "space":  { "xxs": 2, "xs": 4, "sm": 8, "md": 12, "lg": 16, "xl": 24 },
  "radius": { "sm": 4, "md": 6, "lg": 10, "pill": 999 },
  "font":   { "body": 14, "small": 12, "title": 18, "mono": 14 },
  "elevation": { "0": [0,0,0], "1": [0,2,6], "2": [0,4,12], "3": [0,8,24] },
  "chrome": { "title-height": 28, "border": 1, "button-size": 28 },
  "motion": { "press-ms": 80 }
}
```

(`#1E2832` is the existing hello-world background — kept deliberately so
today's screenshots stay recognisable.) Elevation is `[x, y, blur]` of
a shadow, mapped to LVGL `shadow_ofs_x/_y/_width`.

`tools/gen_tokens.py` (host, standard library only) emits:

1. `include/nui_tokens.h` — `#define NUI_SPACE_MD 12`, and colours as
   `static const uint32_t nui_light[NUI_COLOR_COUNT]` /
   `nui_dark[...]` indexed by an `enum nui_color`.
2. `generated/wm-theme.conf` — the subset the compositor needs to draw
   window chrome (spec 05): title height, border width, button size, and
   the per-theme colours for frame, title text, button glyphs and hover.
   A plain `key=value` file, installed to `/etc/neoos/wm-theme.conf`,
   parsed by `neoos-wm` at startup. The WM stays independent of LVGL.

Both generated files are **committed**, so building the kit or the WM
needs no Python; the generator is re-run by hand when tokens change,
and a `make check-tokens` target fails CI if they are stale.

## Theming

- `src/theme.c` implements an `lv_theme_t` whose **parent is LVGL's
  `default` theme** (`lv_theme_set_parent`). The NeoOS theme overrides
  styles per widget class in its `apply` callback using token values;
  anything it does not override falls through to `default`. This is the
  cheapest route to "every LVGL widget looks right" — the kit only has
  to style what it cares about.
- Style objects are built once per theme (light, dark) at startup:
  ~40 `lv_style_t` per theme. Switching theme swaps which set `apply`
  uses and calls `lv_obj_report_style_change(NULL)`, which refreshes
  every object in the display without recreating widgets.
- **Runtime switching**: `nui_set_theme(NUI_THEME_DARK)`. The shell owns
  the user's choice (stored in `desktop.db` settings, spec 04) and
  broadcasts it; apps receive `WM_THEME_CHANGED` (spec 05) and call
  `nui_set_theme` from their event loop. An app therefore follows the
  system theme with zero code.
- **States** map to LVGL states: normal, `LV_STATE_HOVERED` (hover —
  delivered because WM pointer motion reaches LVGL as an indev),
  `LV_STATE_PRESSED`, `LV_STATE_FOCUSED`/`FOCUS_KEY` (keyboard focus
  shows the focus ring; mouse focus does not), `LV_STATE_DISABLED`,
  `LV_STATE_CHECKED`.

## Typography

- Body/UI font: **Montserrat** (already compiled into `neoos-lvgl` at 14
  and 28) extended with 12 and 18 via `LV_FONT_MONTSERRAT_12/18` —
  built-in LVGL fonts, no download.
- Monospace (Terminal, Notepad): **Spleen 12×24** — already in the tree
  (`third_party/spleen`, used by the kernel terminal and `neoos-wm`'s
  font), converted once to an LVGL font with `lv_font_conv` on the host
  (a small npm tool; if unavailable, a ~100-line converter from the
  existing `tools/bdf2c.py` output — the BDF is already parsed there).
- Glyph range: ASCII + Latin-1 in v1. See open questions.

## Public C API

One header, `nui.h`, prefix `nui_`, LVGL objects passed as `lv_obj_t *`
so kit components interoperate with raw LVGL freely.

### Application glue

```c
typedef struct nui_app nui_app;
typedef struct nui_window nui_window;

// Connects to neoos-wm, initialises LVGL, installs the NeoOS theme.
// Returns NULL (and logs) if there is no compositor.
nui_app    *nui_app_create(const char *app_name);
// One WM surface <-> one lv_display. Title bar is drawn by the WM.
nui_window *nui_window_create(nui_app *a, int w, int h, const char *title,
                              unsigned flags /* NUI_WIN_* -> WM_SURFACE_* */);
lv_obj_t   *nui_window_root(nui_window *w);      // the display's active screen
void        nui_window_set_title(nui_window *w, const char *title);
void        nui_window_destroy(nui_window *w);
// Blocks on the WM socket and any registered fds with poll(), runs
// lv_timer_handler() when it is due -- no fixed-period sleep loop.
// Returns the exit code passed to nui_app_quit().
int         nui_app_run(nui_app *a);
void        nui_app_quit(nui_app *a, int code);
// Extra fds (a PTY, an inotify fd) serviced by the same loop.
void        nui_app_watch_fd(nui_app *a, int fd, void (*ready)(int fd, void *ud), void *ud);
// Hook for WM_CLOSE: return 0 to veto (e.g. "unsaved changes").
void        nui_window_on_close(nui_window *w, int (*cb)(nui_window *, void *), void *ud);
```

The loop computes its `poll()` timeout from `lv_timer_handler()`'s
return value (LVGL reports ms until its next timer), so an idle app
sleeps in the kernel until the next real event — with tickless idle
(hrtimer milestone), an idle desktop really is idle.

Rendering: each `nui_window` uses LVGL's **DIRECT** render mode into the
WM surface's shared buffer (as `neoos-lvgl`'s Hello does today), and the
flush callback turns LVGL's invalidated areas into `wm_damage` +
`wm_commit`. Input: WM pointer events feed an `LV_INDEV_TYPE_POINTER`,
WM key events feed an `LV_INDEV_TYPE_KEYPAD` with the display's default
group — both per window. Multiple windows per process = multiple
`lv_display`s (LVGL 9 supports this natively), which is exactly what
the shell needs (spec 04).

### Components

Every constructor returns the underlying `lv_obj_t *`; variants and
sizes are flags; events are ordinary LVGL events (`LV_EVENT_CLICKED`,
`LV_EVENT_VALUE_CHANGED`) so C# and C handle them the same way.

| component | constructor | built on | notes |
|---|---|---|---|
| Button | `nui_button(parent, text, NUI_BTN_PRIMARY\|SECONDARY\|GHOST\|DANGER)` | `lv_button` + label | optional icon: `nui_button_set_icon(b, LV_SYMBOL_*)` |
| Checkbox | `nui_checkbox(parent, text, checked)` | `lv_checkbox` | tri-state not in v1 |
| Radio | `nui_radio_group(parent)` → `nui_radio(group, text)` | `lv_checkbox` + round indicator | the group enforces exactly-one CHECKED; `VALUE_CHANGED` on the group with the selected index |
| List | `nui_list(parent)`, `nui_list_add(list, icon, text, user_data)` | `lv_list` | single selection with `selection` token colour, keyboard up/down, `nui_list_selected()`; virtualised variant `nui_list_virtual(parent, count, render_cb)` for thousands of rows (Task Manager, file dialog) |
| Grid | `nui_grid(parent, cols, rows)` | container + `LV_LAYOUT_GRID` | tracks as `NUI_FR(n)`/px from tokens; `nui_grid_place(child, col, row, colspan, rowspan)` |
| TextInput | `nui_textinput(parent, placeholder, NUI_TI_SINGLE\|MULTI\|PASSWORD)` | `lv_textarea` | multi-line mode is Notepad's editor surface; selection + clipboard via spec 05 |
| Dropdown | `nui_dropdown(parent, options_newline_separated)` | `lv_dropdown` | |
| Dialog | `nui_dialog(app_window, title)` + `nui_dialog_add_button(d, text, variant, response_id)`; `nui_dialog_run_async(d, cb)` | `lv_layer_top` modal backdrop + panel | focus trapped inside; Enter = default, Escape = cancel |
| Tabs | `nui_tabs(parent)`, `nui_tabs_add(t, title)` → page | `lv_tabview` | closable tabs (Terminal) via `NUI_TAB_CLOSABLE` |
| Scrollbar | not a widget: `nui_theme` styles `LV_PART_SCROLLBAR` everywhere | style | always-visible on desktop-sized content (`LV_SCROLLBAR_MODE_AUTO`) |
| Tooltip | `nui_tooltip_attach(obj, text)` | `lv_layer_top` label | appears after 600 ms hover (an `lv_timer`, event-driven), hides on leave/press |
| Menu | `nui_menu(parent_window)`, `nui_menu_add(m, text, shortcut, id)`, `nui_menu_popup(m, x, y)` | top-layer panel + `nui_list` | used for app menus and context menus; menubar = `nui_toolbar` of menu buttons |
| Toolbar | `nui_toolbar(parent)` | flex row | |
| FileDialog | `nui_file_dialog_open(win, start_dir, filters, cb)` / `_save(...)` | Dialog + virtual List + TextInput | reads the directory with `opendir`/`readdir`; hidden-file toggle; spec 09 |

Every component documents, in `nui.h` next to its constructor: purpose,
states it styles, events it emits, keyboard behaviour.

## Accessibility (what "accessible" means here)

- Every interactive component is reachable and operable by keyboard
  (Tab/Shift+Tab through the window's `lv_group`, Space/Enter to
  activate, arrows in lists/radios/menus, Escape closes popups).
- Focus is always visible when it came from the keyboard.
- Token colours meet WCAG AA contrast (4.5:1 text, 3:1 UI) in both
  themes; `gen_tokens.py` computes and **fails** on violations, so a
  token edit cannot silently break contrast.
- Hit targets ≥ 24×24 px.

## Versioning and release

- Semantic versioning of the **C API** (`NUI_VERSION_MAJOR/MINOR/PATCH`
  in `nui.h`, git tag `vX.Y.Z`). Tokens changing *values* is a patch;
  adding a token or component is minor; removing/renaming anything in
  `nui.h` is major.
- Consumers pin a tag in `neoos-os-builder` configs, like every port.
- The C# binding package version equals the C version (spec 07).
- `CHANGELOG.md` per release; the gallery screenshot set is regenerated
  and reviewed for every minor release.

## Gallery (`nui-gallery`)

- A C app (`gallery/gallery.c`) with a left `nui_list` of component
  pages; each page shows the component in every state and variant,
  both themes via a toggle in the toolbar.
- Arguments `--page=<name> --theme=dark` make it deterministic for
  screenshots; NeoOS gets `make ui-gallery UIKIT_DIR=...` that boots
  `wm.nex` + the gallery on each page and captures framebuffer shots
  (the existing `wm-shot` machinery), compared against committed golden
  images with a small per-pixel tolerance. Golden updates are
  deliberate, reviewed commits.

## Testing

- Host: `gen_tokens.py` unit tests (parsing, contrast check, generated
  output stable); radio-group exclusivity and list selection logic
  compiled against LVGL on the host with LVGL's headless (no-display)
  driver, which is portable C.
- Target: the gallery screenshot suite (above) is the regression gate,
  booted headless in QEMU; plus a scripted keyboard walk (synthetic
  `WM_KEY` via the existing test-injection path) asserting focus order
  from serial markers the gallery prints (`[gallery] focus button:primary`).

## Error handling and logging

- Construction failures (LVGL out of memory) return NULL and log
  `[nui] <component>: allocation failed`; callers check.
- `nui_app_create` without a compositor returns NULL with
  `[nui] no compositor`, the same line the Hello app prints today.
- Lost compositor connection: `nui_app_run` returns `-EPIPE`; apps exit.

## Assumptions to verify

1. LVGL 9.2's `LV_STATE_HOVERED` is delivered for a pointer indev fed
   from WM motion events (it is in upstream LVGL; confirm with our config).
2. Several `lv_display`s in one process with DIRECT mode into separate
   shared buffers work in LVGL 9.2 (they should; the shell depends on it).
3. `lv_font_conv` (npm) is small enough to fetch on the restricted plan;
   fallback is extending `tools/bdf2c.py`.

## Risks

| risk | mitigation |
|---|---|
| LVGL's `default` theme leaks Material look into places we did not override | the gallery shows every widget class; anything not restyled is visible there |
| Two sources of chrome styling (WM, kit) drift | one `tokens.json`, both generated, `make check-tokens` |
| Font coverage (non-Latin text in Notepad) | open question; fallback glyph shown, never a crash |

## Open questions

- Which extra scripts must Notepad/Terminal render in v1 (e.g.
  Persian/Arabic, CJK)? Each adds font size and, for RTL, LVGL's BiDi
  support (`LV_USE_BIDI`). Draft: Latin-1 only in v1.
- Should the accent colour be user-configurable (a token override stored
  by the shell)? Draft: yes in M11, fixed in M2.

## Decision log

| decision | alternatives | why |
|---|---|---|
| Custom `lv_theme_t` over LVGL `default` + thin `nui_*` constructors | adopt LVGLSharp/SquareLine/EEZ output; write widgets from scratch | survey: nothing desktop-shaped exists; LVGL already has widgets + styling engine; only the consistency layer is missing |
| Tokens in JSON → generated C header + WM config | tokens hand-written in C | one source for the kit *and* the compositor chrome |
| Generated files committed | generate at build | builds need no Python; staleness caught by `make check-tokens` |
| Make, not CMake/Meson | CMake | every NeoOS repo uses Make; os-builder drives Make |
| `nui_app` owns WM↔LVGL glue and the poll loop | each app writes it (as Hello does) | written once, correct once (no sleep loops, multi-window) |
| Chrome drawn by the WM, not the kit | client-side decorations | spec 05: the WM must own window-management gestures and survive a hung client |
| Radio/Tooltip/Dialog built; everything else wraps LVGL | wrap nothing / rebuild everything | smallest kit that meets the brief |
