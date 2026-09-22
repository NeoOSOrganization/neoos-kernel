# Research: LVGL component libraries and design systems

Date: 2026-09-23. Status: research note feeding spec 02
(`2026-09-23-desktop-02-ui-kit-design.md`). Part of the desktop roadmap.

Question: before building `neoos-ui-kit`, what already exists on top of
LVGL that could be reused — themes, component sets, editors, C#
bindings — and what should be built instead?

Method: web search (text only; nothing downloaded, per the restricted-
bandwidth rule) plus what the vendored LVGL 9.2 in `neoos-lvgl/upstream`
already contains. Findings that could not be confirmed from a primary
source are marked *unverified*.

## Constraints that decide the answer

A candidate must fit **all** of these, or it is out regardless of quality:

1. **Runs on target as plain C on LVGL 9.2** (what `neoos-lvgl` vendors).
   Tools that only *generate* code on a host are acceptable only if the
   generated code has no runtime dependency beyond LVGL.
2. **Licence compatible with an open-source OS** — MIT/BSD/Apache for
   anything linked into NeoOS; a host tool may be anything we are allowed
   to run.
3. **Desktop-shaped**, not appliance-shaped: overlapping windows, dense
   lists, keyboard focus, text editing, hover. Most LVGL material targets
   480×272 touch panels.
4. **No big downloads** (restricted plan): an Electron IDE or a full
   SDK is a cost, not just a convenience.

## What LVGL 9.2 itself provides

- **~30 widgets**: button, label, checkbox, dropdown, list, menu,
  msgbox, tabview, textarea, table, roller, slider, switch, spinbox,
  keyboard, window, calendar, chart, image, canvas, … — no **radio
  button** widget (radio is a checkbox with a style and group logic you
  write), no **tooltip**, no **modal dialog** beyond `lv_msgbox`, no
  **grid widget** (grid is a *layout*, `LV_LAYOUT_GRID`, on any container).
- **Themes** (`lv_theme_t`): a theme is a set of styles plus an `apply`
  callback that attaches them to every newly created widget by class.
  Built in: `default` (Material-inspired, light/dark, primary/secondary
  colour), `simple`, `mono`. Themes can be **chained** (`lv_theme_set_parent`),
  so a custom theme can extend `default` and override only what it
  needs. Runtime switching = rebuild the theme and call
  `lv_obj_report_style_change(NULL)` to refresh every object. [LVGL
  themes docs]
- **Styles** are cascading, stateful (`LV_STATE_PRESSED/FOCUSED/
  CHECKED/DISABLED/HOVERED` …) and part-aware (`LV_PART_INDICATOR`,
  `LV_PART_SCROLLBAR`, …) — i.e. LVGL already has the mechanism a design-
  token system maps onto.
- **XML UI description** (9.3+, not in our 9.2): declarative components
  loaded at runtime. Its *specification* licence forbids third-party
  editors/code generators that read or write the format without written
  permission from LVGL LLC; using it inside an app is allowed. [LVGL XML
  licence]

## Candidates

### Visual editors

| tool | what | licence | fit |
|---|---|---|---|
| **LVGL Pro** (Editor) | LVGL's own editor, built around the XML format; Figma import; generates C | commercial, "royalty-free" tiers [LVGL Pro licence] | needs LVGL ≥ 9.3 XML; paid; ties the kit to a vendor tool. **No.** |
| **SquareLine Studio** | drag-and-drop editor exporting C for LVGL | proprietary, paid tiers; partnership with LVGL ended Feb 2024 [LVGL forum / comparison] | output is appliance-screen code, not a component library. **No.** |
| **EEZ Studio** | Electron drag-and-drop editor with flow programming, exports LVGL C | free and open source (GPL-3.0, *unverified*) | useful for *mock-ups*; its generated code is screen-shaped and the IDE is a large download. **Not as a dependency**; optional personal tool. |
| PicoPixel, GUI Guider (NXP) | more screen editors | mixed | same objection as SquareLine |

Editors solve "lay out one screen", which is not the problem: the
problem is a small, consistent, themeable **component set** used by a
handful of hand-written apps. None of them produces that.

### Component libraries / design systems on LVGL

- There is **no established, maintained, desktop-oriented component
  library for LVGL** comparable to what Qt or GTK have. Searches for
  Material/Fluent LVGL component sets return LVGL's own built-in
  Material-ish default theme and forum discussion (Metro was dropped in
  favour of Fluent in an old LVGL themes issue), not a library.
  [lvgl/lvgl#804]
- `awesome-lvgl` lists drivers, ports, fonts and demos; its "UI" section
  is editors and demo apps, not reusable desktop widget sets.
  [awesome-lvgl]
- LVGL's own `lv_demos` (widgets, benchmark, music) are demonstrations,
  not an API.

### C# bindings

| project | approach | fit |
|---|---|---|
| **LVGLSharp** (IoTSharp) | WinForms-compatible API over LVGL, ClangSharp-generated P/Invoke, NativeAOT, multi-RID [LVGLSharp] | impressive, but a **WinForms emulation layer** — the opposite of a thin binding over *our* C kit; brings its own abstractions and native-library loading model (runtime `DllImport`), which NeoOS's static/`DirectPInvoke` build must replace anyway |
| imxcstar/LVGLSharp, joelmartinez/LVGLSharp | ClangSharp-generated raw bindings | the *technique* (ClangSharp PInvokeGenerator over headers) is exactly right; the output targets a different LVGL build/config and would need regenerating against ours regardless |
| `neoos-lvgl/csharp/Hello` (ours) | hand-written `Lvgl.cs` / `Wm.cs`, `DirectPInvoke` | proven on NeoOS; the seed for spec 07 |

## Recommendation

**Build `neoos-ui-kit` as a custom LVGL *theme* plus thin component
constructors, reusing LVGL's widgets for everything they cover.** In
detail:

| need | reuse | build |
|---|---|---|
| Theming mechanism | `lv_theme_t` with `default` as parent; LVGL style states/parts | the NeoOS theme (token → style mapping), runtime light/dark switch |
| Button, Checkbox, List, TextInput (textarea), Dropdown, Tabs (tabview), Menu, Scrollbar (style part), Table | LVGL widgets as-is | `nui_*` constructors that create the widget, apply variant styles, and wire events |
| Radio | checkbox widget | radio *group* logic (exclusive `CHECKED` state), round indicator style |
| Grid | `LV_LAYOUT_GRID` | `nui_grid` container helper (track templates from tokens) |
| Dialog / modal | `lv_msgbox` for simple cases | `nui_dialog`: modal layer (`lv_layer_top`), focus trap, Enter/Escape |
| Tooltip | nothing | `nui_tooltip`: hover timer + popup on the top layer |
| Design tokens | nothing | `tokens.json` → generated C header + WM theme file (spec 02, 05) |
| C# bindings | ClangSharp PInvokeGenerator (host tool, the technique LVGLSharp uses) | generate over `nui.h` + the subset of `lvgl.h` the tools call; `DirectPInvoke` |
| Editors | EEZ Studio optionally, for mock-ups only | nothing depends on it |

Why not adopt any library wholesale: none exists that is (a) desktop-
shaped, (b) permissively licensed, (c) on LVGL 9.2, and (d) small. The
build cost of the kit is modest precisely because LVGL already supplies
the widgets and the styling engine; what is missing is the *consistency
layer* (tokens, variants, a handful of composite components), which is
also the part that must be NeoOS's own.

Why not upgrade LVGL to 9.3+ for XML now: the XML format's licence
restricts tooling around it, the kit does not need declarative UI, and
an LVGL bump is a separate, testable change to `neoos-lvgl` that can
happen later without touching the kit's C API. Recorded as future work.

## Sources

- [Themes | LVGL docs](https://lvgl.io/docs/open/common-widget-features/styles/themes)
- [revisit themes · lvgl/lvgl#804](https://github.com/lvgl/lvgl/issues/804)
- [LVGL XML licence (9.5 docs)](https://lvgl.io/docs/open/9.5/xml/xml/license)
- [LVGL Pro Editor licence (9.5 docs)](https://lvgl.io/docs/open/9.5/xml/editor/license)
- [Comparison with other LVGL UI editors — LVGL forum](https://forum.lvgl.io/t/comparison-with-other-lvgl-ui-editors/24330)
- [PicoPixel vs SquareLine Studio vs EEZ Studio](https://picopixel.io/compare/)
- [Free and Open Source EEZ Studio — LVGL forum](https://forum.lvgl.io/t/free-and-open-source-eez-studio/12954)
- [awesome-lvgl](https://github.com/aptumfr/awesome-lvgl)
- [IoTSharp/LVGLSharp](https://github.com/IoTSharp/LVGLSharp), [lvglsharp.net](https://lvglsharp.net/en/)
- [imxcstar/LVGLSharp](https://github.com/imxcstar/LVGLSharp), [joelmartinez/LVGLSharp](https://github.com/joelmartinez/LVGLSharp)
- [LVGL bindings docs](https://lvgl.io/docs/open/9.0/integration/bindings/)
