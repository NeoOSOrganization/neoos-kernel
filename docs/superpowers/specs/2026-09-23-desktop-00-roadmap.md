# Desktop environment and system tools — roadmap

Date: 2026-09-23. Status: **draft for review** — written while you were
away, from the brief "NeoOS Desktop Environment & System Tools
Specification". Every ambiguity was decided rather than asked; each
spec ends with a decision log, and the consolidated log is
`2026-09-23-desktop-99-decision-log.md`.

This is the master sequencing document (the brief's `PLAN.md`). It is
adapted to this repo's conventions rather than the brief's file layout:

| brief asked for | lives here instead | why |
|---|---|---|
| `docs/PLAN.md` | this file | specs live in `docs/superpowers/specs/`, dated |
| `docs/specs/NN-*.md` | `docs/superpowers/specs/2026-09-23-desktop-NN-*-design.md` | same directory and naming as every other milestone |
| `docs/research/*.md` | `2026-09-23-desktop-03-lvgl-ui-libraries-survey.md` | there is no `docs/research/`; a survey that feeds one spec sits beside it |
| `docs/decisions/DECISION-LOG.md` | `2026-09-23-desktop-99-decision-log.md` | ditto |

Implementation **plans** (`docs/superpowers/plans/`) are deliberately not
written yet: the repo's flow is brainstorm → spec → plan → implement, and
each milestone below gets its plan when it is picked up, against the code
as it is then.

## Relationship to the current project goal

`CLAUDE.md` makes Gallium3D the project goal and says graphics-stack work
is subordinate to it "unless the user directs otherwise". This roadmap
exists because you directed it. It does not compete with Gallium3D:
the compositor already renders through Mesa (sub-projects of
`2026-09-22-wm-mesa-everywhere-design.md` shipped), and everything here
is a *client* of that compositor that draws in software (LVGL) into
shared-memory surfaces. Nothing here touches the Mesa path.

It is the continuation of the "Windows-inspired desktop environment"
roadmap started in `2026-09-22-wm-taskbar-start-menu-design.md`
(sub-project 1: cursor theme, shipped; 2: taskbar + empty start menu,
shipped; 3: window chrome; 4: taskbar entries). Sub-projects 3 and 4
are absorbed into spec 05 below.

## The specs

| # | spec | repo(s) | depends on |
|---|---|---|---|
| 00 | this roadmap | — | — |
| 01 | [SQLite port](2026-09-23-desktop-01-sqlite-port-design.md) | **neoos-sqlite** (new), NeoOS (kernel prerequisites) | K1 |
| 02 | [UI toolkit `neoos-ui-kit`](2026-09-23-desktop-02-ui-kit-design.md) | **neoos-ui-kit** (new) | neoos-lvgl |
| 03 | [LVGL UI-library survey](2026-09-23-desktop-03-lvgl-ui-libraries-survey.md) (research) | — | — |
| 04 | [Unified desktop shell](2026-09-23-desktop-04-shell-design.md) | **neoos-shell** (new) | 01, 02, 05 |
| 05 | [Window chrome, window states, taskbar protocol](2026-09-23-desktop-05-window-chrome-taskbar-design.md) | neoos-wm | 02 (tokens only) |
| 06 | [Start menu](2026-09-23-desktop-06-start-menu-design.md) | neoos-shell | 01, 02, 04, K3 |
| 07 | [.NET NativeAOT toolchain and bindings](2026-09-23-desktop-07-dotnet-aot-toolchain-design.md) | **neoos-systools** (new), neoos-ui-kit (C# bindings) | 02 |
| 08 | [Task Manager](2026-09-23-desktop-08-task-manager-design.md) | neoos-systools, NeoOS (procfs) | 07, K2 |
| 09 | [Notepad](2026-09-23-desktop-09-notepad-design.md) | neoos-systools | 07, 01, K1 |
| 10 | [Terminal emulator](2026-09-23-desktop-10-terminal-design.md) | neoos-systools, NeoOS (`libvt` extraction) | 07 |
| 11 | [DOOM in a window](2026-09-23-desktop-11-doom-integration-design.md) | neoos-doom, neoos-shell (app manifest) | 05, 06 |
| 99 | [Decision log](2026-09-23-desktop-99-decision-log.md) | — | — |

### Kernel prerequisites (NeoOS repo)

Found while writing these specs by reading the kernel, not assumed. Each is
a Linux-shaped primitive per `CLAUDE.md` ("if the shim ever starts emulating
a primitive … add the primitive to the kernel instead"), each needs its shim
entry, a `docs/stdlib.md` entry and an `abi-compatibility.md` refresh.

| id | what | needed by | today |
|---|---|---|---|
| K1 | `fcntl(F_SETLK/F_SETLKW/F_GETLK)` POSIX advisory record locks; honour `O_EXCL` in `open`; `pwrite`/`pwrite64`; `rename`/`renameat`/`renameat2(flags=0)` | 01 (SQLite locking), 09 (atomic save) | no record locks; `O_EXCL` defined but never checked; no `pwrite`; no `rename` in the shim at all |
| K2 | procfs: real `utime`/`stime` in `/proc/<pid>/stat` (from the scheduler's ns accounting the hrtimer milestone made real), `/proc/<pid>/status` (`Name`, `State`, `VmRSS`, `Threads`), `/proc/meminfo`, `/proc/uptime` | 08 | only `/proc/<pid>/stat`, `/proc/<pid>/cmdline`, `/proc/stat` |
| K3 | init power control: `kill(1, SIGUSR2)` = power off, `kill(1, SIGTERM)` = reboot (BusyBox init's convention) | 06 | `reboot(2)` is PID-1-only; init has no signal-driven shutdown |

K1–K3 are small and independent; they can be done in any order, but each
must land before the first spec that needs it.

## Dependency graph and build order

```
            neoos-lvgl (exists)            NeoOS kernel: K1  K2  K3
                  |                                     |   |   |
             02 neoos-ui-kit ─────────┐                 |   |   |
            /     |       \           |                 |   |   |
   tokens  /      |        \          |        01 neoos-sqlite  |   |
          v       |         v         v           |      |      |   |
 05 neoos-wm      |    07 neoos-systools (SDK + bindings)  |   |
 (chrome, states, |       |        |         \           |   |
  shell channel)  |       v        v          v          |   |
          \       |    08 taskmgr 09 notepad 10 terminal  |   |
           v      v                   ^ (01)              |   |
          04 neoos-shell  <───────────────── 01 ──────────+   |
           |  (wallpaper, icons, taskbar)                     |
           v                                                  |
          06 start menu (in neoos-shell) <────────────────────+
           |
           v
          11 neoos-doom windowed backend + app manifest
```

Build order for `neoos-os-builder` (every new port repo must be built
after everything it links against — see "os-builder" below):

1. neoos-musl (exists) → neoos-lvgl (exists) → **neoos-sqlite** → **neoos-ui-kit**
2. neoos-wm (exists, updated by 05)
3. **neoos-shell** (links ui-kit, lvgl, sqlite, wmclient)
4. **neoos-systools** (host `dotnet publish`; links ui-kit, lvgl, sqlite, wmclient, libvt)
5. neoos-doom (exists, updated by 11)

## Milestones

Ordered by dependency. "∥" marks work that can proceed in parallel with
the milestone above it.

| M | content | exit criterion (headless QEMU + serial markers, as every milestone) |
|---|---|---|
| M0 | K1, K2, K3 kernel prerequisites | new userland test binaries pass (`locktest`, `renametest`, `proctest`, `powertest`) inside the gauntlet; `docs/stdlib.md` + `abi-compatibility.md` updated |
| M1 | 01 SQLite port | `sqlitetest.nex` passes a scripted create/insert/transaction/crash-recovery run on disk; two processes contend for the same DB without corruption |
| M2 ∥ M1 | 02 UI kit v0.1 (tokens, theme, Button, List, Checkbox, Radio, Grid, TextInput, Dialog) + gallery app | `make ui-gallery` screenshot matches the reviewed golden in light and dark |
| M3 | 05 WM protocol v2: window states, chrome buttons, shell channel, clipboard | `make wm-chrome` drives min/max/close/restore by synthetic clicks; serial log shows the state events |
| M4 | 04 Shell v1: one process for wallpaper, desktop icons, taskbar (running windows), replacing `taskbar.nex`/`startmenu.nex` | `make desktop-shell`: kill -9 the shell, it respawns and re-adopts every window |
| M5 | 06 Start menu v1 in the shell, SQLite-backed, app manifests, power | click Start → DOOM entry visible under Games; Restart reboots via K3 |
| M6 ∥ M3–M5 | 07 NeoOS.Sdk + bindings; C# gallery port | the C# gallery renders identically to the C one |
| M7 | 08 Task Manager | lists every `/proc` pid with CPU%; End task kills a spinner |
| M8 | 09 Notepad | open/edit/save round-trip; atomic save survives power-off mid-write |
| M9 | 10 Terminal | runs BusyBox `ash` on a PTY; `vi` renders; scrollback + copy/paste |
| M10 | 11 DOOM in a window | DOOM launches from the start menu into a window; F11 toggles fullscreen |
| M11 | Remaining ui-kit components (Dropdown, Tabs, Tooltip, Menu, Scrollbar styling), polish, docs | gallery complete |

M2 (UI kit) must be **complete** before M7–M9 start (the brief's ordering
rule), where "complete" means every component those three apps use —
Button, List, TextInput, Dialog, Menu, Tabs, Scrollbar — is in the kit
with its gallery page. M11 is what remains after that.

## Cross-cutting conventions (apply to every spec)

- **Language split.** Everything that must start instantly or survive
  being the last thing running — compositor, shell, start menu — is C
  (musl, hosted toolchain). The system tools are C# NativeAOT, per the
  brief. Both sides share one C ABI: `neoos-ui-kit` (C) with C# bindings
  generated over it.
- **Stable C ABI first, C# layered on top.** No C# component defines
  behaviour the C API cannot express.
- **Static linking** for every binary this roadmap ships (the NativeAOT
  path is static already, and `DirectPInvoke` is what makes it work — see
  `neoos-lvgl/README.md`). Dynamic linking works on NeoOS now
  (`2026-09-09-dynamic-linking-design.md`) and is the obvious later
  optimisation (one `libnui.so` shared by five processes); it is recorded
  as future work, not a v1 dependency.
- **Logging.** Every process logs to stderr with a `[component]` prefix,
  matching the existing `[wm]`, `[startmenu]` lines; stderr reaches the
  serial log, which is what every test reads. Errors are logged with the
  errno name, never swallowed.
- **Testing.** The repo rule holds: behaviour is verified by booting
  headless QEMU and reading serial markers (`tools/boot_until.sh`, never
  a fixed sleep), plus framebuffer screenshots for visual checks
  (`make wm-shot` pattern). Host-side unit tests are added where the code
  is portable userland C or C# (SQLite queries, token generator, VT
  parser, text buffer) — these are libraries, not bare-metal code, so the
  "no host-runnable tests" rule for the kernel does not apply to them.
- **Timing.** No test waits on a fixed delay; every wait is on a real
  condition (a serial marker, a WM event, a file appearing). Standing
  rule.
- **Documentation.** Each new repo gets `README.md` (build, layout),
  `ARCHITECTURE.md` (the design in brief, pointing back at its spec
  here), and API docs in its public headers. NeoOS-native protocol and
  kernel additions go into `docs/stdlib.md`.

## os-builder

`CLAUDE.md`'s "Keeping neoos-os-builder in sync" applies to every new
repo here. Concretely, when each lands:

- `scripts/build.sh` learns the dependency chain: `neoos-ui-kit` needs
  `LVGL_DIR`; `neoos-shell` needs `UIKIT_DIR LVGL_DIR SQLITE_DIR WM_DIR`;
  `neoos-systools` needs all of those plus a host .NET SDK (10.0.x, which
  the reference host has: 10.0.112). The per-port loop today passes only
  `MUSL_DIR` and has no build order — this roadmap is the forcing function
  for fixing that bug properly (a topological order from a `deps:` field
  in each port's `*.test.json`/manifest), not port by port.
- `docs/BUILD_ORDER.md` gains the chain above.
- A new `config/desktop.yaml` builds the full desktop: wm + shell +
  systools + doom.

## Notes for the user — review these first

1. **Five new repos are proposed** under `NeoOSOrganization`:
   `neoos-sqlite`, `neoos-ui-kit`, `neoos-shell`, `neoos-systools`
   (plus an optional future `neoos-icons` for assets — not needed for
   v1). None has been created; that is an outward-facing action and was
   left for you.
2. **The start menu is part of the shell process**, not its own
   executable (spec 04/06, D-04-2). The brief calls it an "application"
   but also forbids separate processes for desktop pieces; one process
   satisfies both. This also retires today's `taskbar.nex` /
   `startmenu.nex` pair.
3. **Window chrome stays server-side in `neoos-wm`** (spec 05), themed
   from the UI kit's design tokens through a generated config file, so
   one token change restyles both the WM frame and every LVGL app.
4. **SQLite is the start menu's store, but packages register apps with
   plain-text manifests** (`/usr/share/neoos/apps/*.app`) that the shell
   imports. DOOM registers itself this way. Hand edits go through an
   in-shell editor; `sqlite3` is shipped for power users.
5. **Three kernel prerequisites (K1–K3)** were found by reading the code:
   no record locks / `O_EXCL` / `pwrite` / `rename`, thin procfs, no way
   for a non-PID-1 process to power off. They are small but they are
   kernel ABI work and they come first.

Assumptions about NeoOS that must be verified before each milestone's
plan are listed per spec under "Assumptions to verify"; the ones that
could change a design (not just a detail) are gathered in the decision log.
