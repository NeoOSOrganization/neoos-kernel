# Desktop environment — consolidated decision log

Date: 2026-09-23. Every decision taken on your behalf while writing the
desktop specs (`2026-09-23-desktop-00-roadmap.md` and siblings), in one
place, with the alternative and the reason. Each spec repeats its own
decisions in context; this is the review list. IDs are `D-<spec>-<n>`.

## Structure and repos

| id | decision | alternatives | why |
|---|---|---|---|
| D-00-1 | Specs follow this repo's layout (`docs/superpowers/specs/2026-09-23-desktop-NN-*`), not the brief's `docs/PLAN.md` + `docs/specs/` | brief's layout | you asked for "this repo's way"; every milestone lives there |
| D-00-2 | Five new repos: `neoos-sqlite`, `neoos-ui-kit`, `neoos-shell`, `neoos-systools` (+ bindings inside `neoos-ui-kit`) | fewer, bigger repos; put the shell in `neoos-wm` | matches the one-component-one-repo pattern (`neoos-wm`, `neoos-lvgl`, ports); lets images choose |
| D-00-3 | Shell/start menu in C; system tools in C# NativeAOT | all C#; all C | the brief mandates C# for tools; the shell is on the boot/crash path |
| D-00-4 | Static linking for everything in v1 | shared `libnui.so` | NativeAOT `DirectPInvoke` is static; `.so` is a later optimisation |
| D-00-5 | Kernel prerequisites K1–K3 are Linux-shaped kernel features, not shims | per-app workarounds | CLAUDE.md "translation, never emulation" |

## SQLite (spec 01)

| id | decision | alternatives | why |
|---|---|---|---|
| D-01-1 | Stock `unix` VFS; kernel gains POSIX record locks (K1) | custom VFS; `unix-none`; `unix-dotfile` | no emulation in the port; unlocked DBs corrupt under a second writer |
| D-01-2 | Rollback journal, no WAL | WAL | WAL needs shared file mappings + shm locks |
| D-01-3 | One DB per owning app in `/var/lib/neoos/` | one system DB | one writer per DB, small blast radius |
| D-01-4 | `PRAGMA user_version` migrations in a shared `nsql` helper, refuse downgrades (read-only) | ad-hoc per app | one implementation for C and C# |
| D-01-5 | Amalgamation vendored | autoconf tarball | smallest download, no configure |

## UI kit (specs 02, 03)

| id | decision | alternatives | why |
|---|---|---|---|
| D-02-1 | Custom `lv_theme_t` (parent: LVGL default) + thin `nui_*` constructors | adopt an existing library/editor output | survey found nothing desktop-shaped, permissive and on LVGL 9.2 |
| D-02-2 | `tokens.json` → generated C header **and** WM theme file | tokens in C only | one source for app widgets and window chrome |
| D-02-3 | Generated files committed; `make check-tokens` | generate at build | no Python needed to build |
| D-02-4 | `nui_app` owns WM↔LVGL glue and a poll-based loop | every app hand-rolls it | correct once; no sleep loops; multi-window |
| D-02-5 | Stay on LVGL 9.2; XML (9.3+) not used | upgrade now | XML licence restricts tooling; not needed |
| D-02-6 | Latin-1 glyphs in v1 | full Unicode fonts | size; open question for you |

## Window management (spec 05)

| id | decision | alternatives | why |
|---|---|---|---|
| D-05-1 | Chrome drawn by the WM (server-side), themed by tokens | client-side decorations by the UI kit | works for hung clients; WM cannot link LVGL |
| D-05-2 | Protocol v2 with version negotiation; surfaces are objects on a connection | one connection per surface (today) | a single-process shell needs several surfaces; v1 clients keep working |
| D-05-3 | Roles: background / normal / panel / popup; work area excludes panels | flags only | z-order layers are what the shell needs; fixes the taskbar-overlap gap |
| D-05-4 | Shell channel = first client to `WM_SHELL_BIND` | uid-based privilege; shell inside WM | NeoOS has no uids; a shell crash must not kill the WM |
| D-05-5 | No window grouping in v1 | group by app | nothing needs it yet |
| D-05-6 | Taskbar click on the focused window minimizes it | always activate | Windows semantics |
| D-05-7 | Clipboard (text) brokered by the WM via memfds | clipboard daemon | the WM already brokers memfds |
| D-05-8 | `SO_PEERCRED` for window→pid attribution (add to kernel if missing) | client-reported pid | cannot be spoofed; Linux-shaped |

## Shell and start menu (specs 04, 06)

| id | decision | alternatives | why |
|---|---|---|---|
| D-04-1 | Shell is its own process; WM stays separate | shell hosts the WM | isolation of crashes; WM stays small |
| D-04-2 | Start menu is inside the shell process | separate `startmenu.nex` | brief: no separate desktop processes |
| D-04-3 | Background + taskbar + popups = separate surfaces of one process | one full-screen surface | z-order makes one surface impossible |
| D-04-4 | Crash recovery via init `respawn` + new exponential backoff; channel replay rebuilds state | watchdog process | init already respawns |
| D-04-5 | Settings in `desktop.db`, not an INI | text config | one store, one writer, one backup path |
| D-04-6 | Desktop icons are pins of start-menu apps | files on a Desktop directory | one launch path |
| D-04-7 | Shell control socket (`neoos-shell`, text protocol) for other tools | second WM shell channel | only one privileged WM client |
| D-06-1 | Packages register apps with `.app` manifests imported into SQLite; user edits win | packages write SQL; files only | os-builder copies files already; SQLite stays the store |
| D-06-2 | In-shell editor dialog (+ `sqlite3` CLI) | config file + reload | no two-way sync between file and DB |
| D-06-3 | Case-insensitive substring search, prefix-ranked, in memory | fuzzy; SQL LIKE; FTS5 | tiny data set, predictable |
| D-06-4 | Power: `kill(1, SIGUSR2)` off / `kill(1, SIGTERM)` reboot (BusyBox init convention, K3) | new syscall; open `reboot(2)` to all | init must stop services; BusyBox applets work too |
| D-06-5 | `args` split with POSIX-like quoting, no shell | `/bin/sh -c` | no injection surface, no dependency |

## System tools (specs 07–10)

| id | decision | alternatives | why |
|---|---|---|---|
| D-07-1 | Stock `linux-musl-x64` static NativeAOT, packaged as `NeoOS.Sdk` props | custom RID / runtime pack | already works (Hello: 1.7 MB) |
| D-07-2 | C# UI bindings live in `neoos-ui-kit` | in `neoos-systools` | versioned with the C API |
| D-07-3 | ClangSharp-generated bindings, committed; hand-written fallback | runtime binding | no generator at build time |
| D-07-4 | Own SQLite binding over `sqlite3.h` + `nsql` | Microsoft.Data.Sqlite | runtime library loading doesn't fit static linking |
| D-07-5 | LVGL only on the UI thread; background work posts back through a watched pipe | locks around LVGL | LVGL is single-threaded |
| D-08-1 | Task Manager reads Linux-shaped procfs (K2) | NeoOS-native process syscall | same code works on Linux; improves BusyBox `ps`/`top` |
| D-08-2 | Sample only while visible | always | idle stays idle |
| D-09-1 | Notepad: piece table + undo coalescing, host-tested model | edit the widget string | testable, cheap undo |
| D-09-2 | Atomic save (temp + `O_EXCL` + `fsync` + `rename`, K1) | in-place overwrite | never lose the old file |
| D-09-3 | 4 MiB file cap in v1 | unlimited | `lv_textarea` limits; lifted with a virtualised view |
| D-10-1 | Terminal reuses `userland/term/vt.c` as `libvt`, extended to UTF-8 and runtime sizes | new parser; libvterm | debugged against BusyBox, pure, host-testable, no download |
| D-10-2 | PTY fd serviced on the UI loop | reader thread | no cross-thread LVGL |
| D-10-3 | Copy/paste on Ctrl+Shift+C/V | Ctrl+C/V | Ctrl+C is SIGINT |

## DOOM (spec 11)

| id | decision | alternatives | why |
|---|---|---|---|
| D-11-1 | DOOM becomes an ordinary WM client (shared buffer), fb0 backend kept as fallback | WM fb passthrough mode | no mode-switch protocol; crash-safe |
| D-11-2 | Integer scaling, 640×400 default window | arbitrary scaling | crisp, trivial |
| D-11-3 | Absolute-deadline tic pacing with `clock_nanosleep(TIMER_ABSTIME)` | relative sleeps | exact since hrtimers; no drift |

## Assumptions that could change a design (verify first)

These are not details — if one is false, the named decision changes:

| assumption | if false | affects |
|---|---|---|
| NeoOS `fsync` reaches the device through the block cache | SQLite durability needs a kernel fix first | D-01-1, D-09-2 |
| `/var/lib/neoos` persists on desktop images | persistence only on specific images; shell must reseed every boot | D-04-5 |
| `SO_PEERCRED` exists on AF_UNIX | add it (small, Linux-shaped) to M0 | D-05-8 |
| init's `respawn` restarts after signal-terminated exits | fix init as part of M4 | D-04-4 |
| `reboot(LINUX_REBOOT_CMD_RESTART)` resets the machine | implement reset in K3 | D-06-4 |
| NeoOS `fork` is copy-on-write | tools must use `spawn`, never `Process.Start` | D-07-1 |
| NeoOS pty implements `TIOCSWINSZ` + `SIGWINCH` | add to the pty driver in M9 | spec 10 |
| LVGL 9.2 handles several DIRECT-mode displays in one process | shell needs one connection/display trick | D-04-3 |
| `lv_textarea` is usable at 4 MiB | lower the cap or virtualise earlier | D-09-3 |

## Notes for the user

**Biggest assumptions:** the nine above; plus that you are happy for the
desktop to be a *Windows-shaped* design (taskbar semantics, Start menu,
Ctrl+Shift+C in the terminal) — every behaviour choice followed the
brief's Windows vocabulary.

**Top 5 things to review:**

1. **The repo split** (D-00-2) — creating repos is left to you; nothing
   was pushed or created.
2. **Start menu inside the shell process** (D-04-2) — the brief both
   calls it an "application" and forbids separate desktop processes.
3. **WM protocol v2** (D-05-2) — the largest single change; it breaks no
   existing client but it is a real compositor rewrite of the client
   bookkeeping (8-slot array → dynamic connections with surface lists).
4. **Kernel prerequisites K1–K3** — record locks, `rename`, `pwrite`,
   `O_EXCL`, procfs, init power signals: kernel ABI work that comes
   before any desktop code.
5. **Fonts / scripts** (D-02-6) — Latin-1 only in v1 keeps things small;
   if you need Persian or other scripts in Notepad/Terminal from day one,
   that changes the UI kit's font pipeline and LVGL config (BiDi).
