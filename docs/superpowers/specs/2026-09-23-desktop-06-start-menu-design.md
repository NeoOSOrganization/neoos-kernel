# Start menu

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M5. Lives in `neoos-shell` (spec 04) — it is part of the shell process,
not an executable of its own. Depends on spec 01 (SQLite + schema),
02 (UI kit), 05 (popup role), K3 (power).

## Goals

- Config-driven entries, each with **category, name, executable path,
  command-line arguments** (plus icon and sort order), grouped and shown
  **by category**.
- A static **power section** at the bottom: **Shut down** and **Restart**.
- Backed by **SQLite** (`desktop.db`, schema = spec 01 migration 1).
- Search.
- Add/edit/remove entries and categories.
- Packages (ports built by os-builder) can register their apps without
  anyone editing a database — DOOM is the first (spec 11).

## Non-goals

- Recently-used / most-used ranking in v1 (the settings key is reserved;
  the table comes in M11).
- Nested categories, per-user menus, `.desktop` (freedesktop) compatibility.
- A "Run…" box (Terminal covers it in v1).

## UI layout

A `POPUP` surface (spec 05), 560 × 520 px, docked above the taskbar's
left edge, glass 50% (today's start-menu look kept):

```
┌──────────────────────────────────────────────┐
│ [🔍 Search apps…                           ] │  TextInput, focused on open
├───────────────┬──────────────────────────────┤
│ All apps      │  DOOM                        │  categories pane (List)  |  apps pane (List)
│ Accessories   │  …                           │  - "All apps" first: every app,
│ Games      ◀  │                              │    alphabetical, category shown as
│ System        │                              │    a muted suffix
│ …             │                              │
├───────────────┴──────────────────────────────┤
│ ⚙ Edit menu…            ⏻ Shut down  ⟳ Restart │  power section (static)
└──────────────────────────────────────────────┘
```

- Left: categories (`nui_list`), sorted by `sort_order, name`. Selecting
  one filters the right pane.
- Right: apps of the selected category (`nui_list`, icon + name),
  sorted by `sort_order, name`. Enter / double-click / single click on
  an app launches it and closes the menu.
- Bottom: "Edit menu…" (opens the editor dialog, below) and the power
  buttons (confirmation dialog before either power action).
- Keyboard: typing anywhere goes to search; ↑/↓ move in the apps pane;
  ←/→ switch panes; Enter launches; Escape closes. The menu closes on
  focus loss (spec 05 popup semantics — today's behaviour, kept).
- Opening: Start button click, and later a `WM_SHELL_KEY` Win-key event.

## Search

Decided: **case-insensitive substring match on the app name, ranked
prefix-first**, across **all categories** regardless of the selected
one (while the box is non-empty the category pane greys out and the
apps pane shows "Results").

Ranking: (1) name starts with the query, (2) a word in the name starts
with it, (3) substring anywhere; ties by `sort_order, name`. Matching is
done in memory over the loaded model (dozens to hundreds of apps), not
with SQL `LIKE`, so each keystroke costs no I/O.

Not fuzzy in v1: with this few entries, fuzzy matching mostly adds
surprising results; recorded, easy to add behind the same function.

## Data flow

```
manifests (/usr/share/neoos/apps/*.app) ──import on start──┐
                                                           v
        user edits (editor dialog) ──────────────> desktop.db (SQLite)
                                                           │ load at start + after every edit
                                                           v
                                          in-memory model (arrays of categories/apps)
                                                           │ render on open / on filter
                                                           v
                                                  nui_list widgets (LVGL)
```

- The model is loaded once at shell start and re-read after the shell
  itself changes the database (it is the only writer, spec 01), so the
  menu opens with no database access at all.

## Entries: manifests + database

Packages cannot be expected to run SQL at image-build time, and
editing a binary database from os-builder's host scripts is fragile.
So packages ship a **manifest**, and the shell imports manifests into
SQLite:

`/usr/share/neoos/apps/<key>.app` — a small `key=value` text file:

```
key=org.neoos.doom
name=DOOM
category=Games
category_key=org.neoos.category.games
exec=/usr/local/bin/doom.nex
args=-iwad /opt/doom/doom1.wad
icon=/usr/share/neoos/icons/doom.png
sort_order=0
```

Import rules (run at every shell start, cheap):

- Upsert by `key`: a manifest's app is inserted if missing; if present
  with `source='package'`, name/exec/args/icon/category are updated from
  the manifest (packages own their entries).
- A category named by a manifest is created on demand (by
  `category_key`, falling back to name).
- If the user **edited** a package entry, it becomes `source='user'` and
  manifests no longer overwrite it. If the user **removed** one, it is
  kept with `hidden=1` so a re-import does not resurrect it.
- A package entry whose manifest disappeared (package removed) is
  deleted unless the user edited it.

The system tools (spec 08–10) and the shell's own defaults ship
manifests the same way; the image build just copies files into
`/usr/share/neoos/apps/` — which is what os-builder already knows how
to do for any port.

## Arguments

`args` is one string, split by the shell with **POSIX-shell-like
quoting**: whitespace separates, `'…'` and `"…"` group, backslash escapes
the next character inside `"…"` and outside quotes. No variable
expansion, no globbing, no pipes — it is not a shell. The executable is
`exec_path` (absolute; never looked up in `PATH`), `argv[0]` is its
basename. Documented next to the manifest format so manifest authors do
not have to guess.

## Launching

`launch.c`:

1. Split args (above) → `argv[]`.
2. `spawnv(exec_path, argv)` (NeoOS's `SYS_SPAWNV` via `lib/`, the
   native spawn primitive; musl `posix_spawn` would also do — use
   whichever `lib/` exposes, the point is no `fork` of a large process).
3. Record `{pid, app_id, launch time}`; `SIGCHLD` handling reaps it
   (spec 04).
4. Nothing else: the app creates its own window, the WM tells the shell
   (`WM_WIN_ADDED` with the pid), and the taskbar button appears. The
   shell can associate that window with the app via the pid → app
   record (used for the button's fallback icon).

Failure → dialog + log (spec 04).

## Power actions

Decided: **signal init**, using BusyBox init's long-standing convention
(kernel prerequisite K3):

| action | shell does | init does |
|---|---|---|
| Shut down | `kill(1, SIGUSR2)` | stops respawn entries, sends `SIGTERM` to all processes, waits for them (bounded, then `SIGKILL`), syncs, `reboot(LINUX_REBOOT_CMD_POWER_OFF)` |
| Restart | `kill(1, SIGTERM)` | same, then `reboot(LINUX_REBOOT_CMD_RESTART)` |

Why this and not a new syscall: `reboot(2)` is PID-1-only by design
(docs/stdlib.md), init must stop services cleanly anyway, and this is
exactly how BusyBox's `poweroff`/`reboot` applets already talk to init —
so those applets work on NeoOS too, for free. Both actions go through a
`nui_dialog` confirmation ("Shut down NeoOS?"). K3 also needs
`LINUX_REBOOT_CMD_RESTART` to actually reset the machine (keyboard-
controller reset or ACPI reset register) — **assumption to verify**:
check what NeoOS's `reboot(RESTART)` does today.

## The editor ("Edit menu…")

Decided: **an in-shell editor dialog, with manifests for packages and
the `sqlite3` CLI for power users** — not "edit a config file and
reload".

Why: the brief makes SQLite the store; a config file that is *also*
editable would need a sync rule in both directions. Packages already
have the file path (manifests), so the only remaining editor is a human
one, and a dialog is the natural one for a desktop.

The dialog (UI kit `nui_dialog` + `nui_tabs`):

- **Categories** tab: list; Add, Rename, Delete (only if empty, or
  "move its apps to…"), Move up/down.
- **Apps** tab: list with category filter; Add / Edit (form: name,
  category dropdown, executable path with a file-picker button
  (`nui_file_dialog_open`), arguments, icon picker, sort order), Remove,
  Move up/down; "Pin to desktop".
- Saving validates: `exec_path` absolute and exists (`access(X_OK)` —
  NeoOS has no exec permission bits, so this checks existence), name
  non-empty. Each save is one transaction.

## Theming

Everything is UI-kit components styled by tokens (spec 02); the menu
has no styles of its own beyond layout. The glass intensity comes from
the `taskbar.glass` setting so the panel and menu match.

## Error handling and logging

- `[startmenu]` prefix (kept from today's binary so existing test greps
  keep meaning something).
- Malformed manifest: skipped with `[startmenu] bad manifest <file>:
  line N: <reason>`; the rest still import.
- Launch errors: dialog + log. Power: if `kill(1, …)` fails, dialog.
- DB write failure in the editor: the dialog stays open with the error;
  nothing is half-applied (one transaction).

## Testing

- Host: search ranking, argument splitting (quoting table), manifest
  parsing, import rules (insert / update / user-edited / hidden /
  removed-package) against an in-memory SQLite.
- Target (`make desktop-startmenu`): boot with a manifest for a test
  app; click Start → `[startmenu] open apps=N`; type a query via
  synthetic keys → `[startmenu] results=…`; Enter → the test app's
  window appears in the taskbar; Restart confirmed → the next boot's
  first marker appears (the test harness keeps QEMU running across the
  reset with `-no-reboot` removed for this target).

## Assumptions to verify

1. `reboot(LINUX_REBOOT_CMD_RESTART)` resets on NeoOS (QEMU and real
   hardware).
2. `SYS_SPAWNV` is reachable from a musl program via `lib/` (the brief's
   `lib/` rule: `spawn` is a NeoOS-native primitive with a `lib/` wrapper).

## Open questions

- Should uninstalled packages' entries be deleted even if pinned to the
  desktop? Draft: yes, and the pin goes with it.
- Win-key behaviour (open menu on key release, not press, like Windows)?

## Decision log

| decision | alternatives | why |
|---|---|---|
| Part of the shell process | separate `startmenu.nex` (today) | brief forbids separate desktop processes; shares model + DB handle |
| Manifests imported into SQLite | packages write SQL; SQLite only; files only | packages get a plain file (os-builder copies files already); SQLite remains the single store; user edits win |
| In-shell editor dialog | edit file + reload | one store, no two-way sync; a desktop edits through UI |
| Substring search, prefix-ranked | fuzzy; SQL `LIKE`; FTS5 | small data set; predictable results; no I/O per keystroke |
| Power via signals to init (BusyBox convention) | new syscall; relax `reboot(2)` to any process | init must stop services first; BusyBox applets keep working |
| POSIX-like quoting for `args`, no shell | pass through `/bin/sh -c` | predictable, no injection surface, no dependency on a shell |
