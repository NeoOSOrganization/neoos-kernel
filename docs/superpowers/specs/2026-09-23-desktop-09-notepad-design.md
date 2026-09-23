# Notepad

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M8. C# NativeAOT in `neoos-systools/apps/Notepad` (spec 07). Needs the
UI kit's TextInput (multi-line), FileDialog, Dialog, Menu (spec 02),
SQLite (spec 01) and K1 (`rename`, `O_EXCL`).

## Goals

- Open and save plain-text files through the UI kit's file dialog;
  New, Open, Save, Save As; "unsaved changes" prompt on close.
- Plain-text editing: selection, cut/copy/paste (system clipboard, spec
  05), **undo/redo**, **Find** and Find next/previous, Replace, Go to
  line, word wrap toggle.
- **UTF-8** throughout.
- **Recent files**, stored in SQLite.
- Saves are atomic: a crash or power-off mid-save never loses the old
  file.

## Non-goals

- Rich text, syntax highlighting, multiple documents per window (open a
  second Notepad), printing, encodings other than UTF-8 on save.
- Files larger than **4 MiB** in v1 (refused with a message) — the
  editor widget is not built for huge buffers; recorded.

## Architecture

Model and view are separate so the model is host-testable:

- **Model** (`TextDocument`, pure C#): the text as a **piece table**
  (original buffer + add buffer + piece list). Edits are O(pieces), undo
  is natural (each edit is an inverse operation), and the original file
  bytes are never copied until saved.
- **Undo/redo**: a stack of edit records; consecutive single-character
  inserts/deletes in the same direction within one "typing run" are
  coalesced into one undo step; a cursor jump or a pause starts a new
  run. Unlimited depth bounded by memory (cap 10 000 steps). Undo past a
  save point is allowed; the title's `*` dirty marker follows the save
  point, not "has any edit happened".
- **View**: the UI kit's multi-line `nui_textinput` (LVGL `lv_textarea`)
  holds the displayed text. The model is the source of truth; the view
  is updated from model edits, and user keystrokes are intercepted
  (`LV_EVENT_INSERT` / key events) and applied to the model first.
  **Assumption to verify** in M8's plan: that `lv_textarea` keeps up at
  4 MiB (it stores one string). If it does not, the fallback is a
  line-virtualised view (`nui_list_virtual` of lines + a caret layer) —
  a larger job, and the reason for the 4 MiB cap.
- Find: plain substring and whole-word/case options, over the model
  (not the widget), highlighting via the textarea's selection.

## Encoding

- Read: bytes → validate UTF-8. Valid → open. Invalid → open anyway,
  decoding invalid sequences as U+FFFD, with a warning bar ("This file
  is not valid UTF-8; saving will replace N invalid bytes"). A UTF-8 BOM
  is detected, hidden and preserved on save.
- Line endings: detect LF vs CRLF on open (majority), preserve on save,
  show in the status bar.
- Fonts and Persian: the UI kit's monospace font (DejaVu Sans Mono) with
  LVGL shaping + BiDi (spec 02 "Persian support"): Persian text is shaped
  and displayed right-to-left, mixed Persian/Latin lines are reordered by
  the BiDi algorithm, and the Persian keyboard layout (Alt+Shift) types
  it. The *model* stays in logical order (what is saved is what was
  typed); caret movement follows logical order in v1 — visual-order
  cursor movement in mixed lines is a known v1 limitation, recorded.
  Glyphs outside Latin + Persian render as a box.

## Files

- **Open**: `nui_file_dialog_open` (spec 02). Also `notepad <path>` on
  the command line (the start menu can pass a file).
- **Save (atomic)**: write to `<dir>/.<name>.notepad-<pid>.tmp` opened
  with `O_CREAT|O_EXCL`, `fsync`, then `rename(tmp, path)`, then `fsync`
  the directory if NeoOS supports it (on FAT the directory entry update
  is what `rename` changes; **assumption to verify**: fatfs `rename` is
  atomic enough — at worst both names exist, never neither). Needs K1's
  `rename` and `O_EXCL`; without them, Notepad falls back to direct
  overwrite and logs that saves are not atomic.
- Save As uses the file dialog in save mode; overwriting asks first.

## Recent files

- `/var/lib/neoos/notepad.db` via `nsql` (spec 01), migration 1:

  ```sql
  CREATE TABLE recent_files (
      path       TEXT PRIMARY KEY,
      opened_at  INTEGER NOT NULL,     -- unix seconds
      cursor     INTEGER NOT NULL DEFAULT 0  -- caret offset to restore
  );
  ```
- Updated on open and save; File → Recent shows the last 10; a path
  that no longer exists is removed when chosen (with a message).
- Own database, not `desktop.db`: the shell is `desktop.db`'s single
  writer (spec 01).

## UI

Window 800×600, title `<name>[*] — Notepad`:

- Menu bar (`nui_toolbar` + `nui_menu`): File (New, Open…, Recent ▸,
  Save, Save As…, Exit), Edit (Undo, Redo, Cut, Copy, Paste, Select all,
  Find…, Find next, Replace…, Go to line…), View (Word wrap, Theme follows
  system).
- Editor fills the window; status bar: `Ln, Col`, line endings, UTF-8.
- Shortcuts: Ctrl+N/O/S/Shift+S/Z/Y/X/C/V/A/F/H/G, F3/Shift+F3.
  Keys arrive as evdev codes (spec 05) and are mapped by the UI kit's
  keypad indev; modifiers tracked in the kit.
- Close (× or Exit) with unsaved changes → Save / Don't save / Cancel
  dialog; the WM's `WM_CLOSE` is vetoable through `nui_window_on_close`
  (spec 02).

## Error handling

- Open errors: dialog naming path + `strerror`.
- Save errors (`ENOSPC`, `EIO`): the temp file is removed, the document
  stays dirty, the dialog says the original is untouched (true, because
  of the atomic save).
- Database errors: recent files are best-effort — logged, never block
  opening or saving.

## Testing

- Host (xUnit): piece table (insert/delete/undo/redo sequences with a
  reference string model, property-style), coalescing rules, UTF-8
  validation/replacement, line-ending detection, find.
- Target (`make notepad`): open a known file from the image, synthetic
  keys type + undo + save; the harness extracts the file from the disk
  image afterwards (`mcopy` — the FAT-bug lesson in the project memory:
  check the bytes on disk, not the app's claim) and compares.
  Crash-atomicity: kill the VM (QEMU quit) during a large save loop;
  the file on disk is either the old or the new content.

## Decision log

| decision | alternatives | why |
|---|---|---|
| Piece table model separate from the LVGL view | edit the textarea string directly | host-testable, cheap undo, large-file headroom |
| Temp + `O_EXCL` + `fsync` + `rename` | overwrite in place | never lose the old file; needs K1, which is Linux-shaped anyway |
| Own `notepad.db` | a table in `desktop.db` | one writer per database (spec 01) |
| Invalid UTF-8 opens with U+FFFD + warning | refuse to open | a text editor that cannot open a slightly broken file is worse |
| 4 MiB cap in v1 | unlimited | `lv_textarea` holds one string; the cap is lifted when the view is virtualised |
