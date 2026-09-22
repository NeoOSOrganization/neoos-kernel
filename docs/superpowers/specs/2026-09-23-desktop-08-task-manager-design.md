# Task Manager

Date: 2026-09-23. Status: draft for review. Desktop roadmap milestone
M7. C# NativeAOT in `neoos-systools/apps/TaskManager` (spec 07), UI
from `neoos-ui-kit` (spec 02). Needs kernel prerequisite K2.

## Goals

- Process list: **PID, name, CPU %, memory, state**, plus threads and
  parent PID; sortable by any column; refreshes while visible.
- **End task**: `SIGTERM`, and `SIGKILL` if it does not exit
  ("End task" → after the process ignores it, "Kill").
- System stats: CPU usage (total and per CPU), RAM used/total, uptime.
- A second tab, **Applications**: windows (from the WM) with "Switch to"
  and "End task" — the familiar Windows split between apps and processes.

## Non-goals

- Performance graphs history beyond the last 60 samples, services,
  startup-app management, per-process I/O or network, priority editing
  (possible later via `setpriority`, which exists).

## The kernel interface (K2)

Linux-shaped files in the existing synthetic `procfs`, so the same code
reads a real Linux `/proc` unchanged — and `top`/`ps` from BusyBox get
better for free. Today (read from `kernel/fs/procfs.c`) `/proc/<pid>/stat`
renders only pid, comm, state (R or Z), ppid, pgrp, session and then
**zeros** for every later field; `/proc/stat` exists; nothing else.

| file | fields NeoOS must fill (Linux semantics) |
|---|---|
| `/proc/<pid>/stat` | field 3 `state` from real thread states (`R` running/ready, `S` sleeping, `T` stopped, `Z` zombie); 14 `utime` and 15 `stime` in clock ticks (`sysconf(_SC_CLK_TCK)` = 100) from the scheduler's per-thread `sum_exec_runtime` summed over live threads + `cpu_ns_exited` (made real by the hrtimer milestone) — `stime` 0 until user/kernel time is split, documented; 18 `priority`, 19 `nice`; 20 `num_threads`; 22 `starttime` (ticks since boot); 23 `vsize` (bytes, sum of VMAs); 24 `rss` (pages resident) |
| `/proc/<pid>/status` | `Name`, `State` (letter + word), `Pid`, `PPid`, `Threads`, `VmSize`, `VmRSS` (kB) — the human-readable file most tools and `top` read |
| `/proc/meminfo` | `MemTotal`, `MemFree`, `MemAvailable` (= free + reclaimable cache, or = free if NeoOS has no reclaimable cache), `Cached`, `SwapTotal: 0` (kB) |
| `/proc/uptime` | `<uptime seconds>.<centis> <idle seconds>.<centis>` from `ktime` and the scheduler's idle accounting |
| `/proc/stat` | per-CPU `cpuN` lines in addition to the aggregate (check what exists) |
| `/proc/self` | symlink to the caller's pid dir (cheap, commonly used) |

Rendering stays per-open (the documented divergence: a file reflects
the process table at open time, not per read). Each field that NeoOS
still cannot supply is written as 0 and listed in `docs/stdlib.md`.

`sysinfo(2)` already exists (uptime, RAM) and is used as a cross-check,
not as the primary source — procfs is what ports expect.

## Data model and sampling

- A background task (not the UI thread, spec 07 threading rule) samples
  every **1 s while the window is visible** (a `PeriodicTimer`; stops
  when minimized — `WM_STATE_CHANGED`, spec 05 — so a minimized Task
  Manager costs nothing).
- Per process: CPU % = Δ(utime+stime) ticks / (Δ wall ticks × online
  CPUs) × 100, from two consecutive samples (the first sample shows
  "—"). Memory = `VmRSS`. Name = `comm` (stat) or the first `cmdline`
  element if longer. State from the stat letter.
- Totals: `/proc/stat` aggregate and per-CPU deltas (user+nice+system vs
  idle), `/proc/meminfo`, `/proc/uptime`.
- Results are posted to the UI thread (`app.Post`) as one immutable
  snapshot; the UI diffs by pid to keep selection and scroll position
  stable across refreshes.

## UI

`nui_window` 720×520, three tabs (`nui_tabs`):

1. **Processes**: `nui_list_virtual` in table mode — columns PID, Name,
   CPU %, Memory, Threads, State; header click sorts (again reverses);
   selection follows the pid, not the row. Bottom toolbar: filter box
   (substring on name), **End task** (danger button).
2. **Applications**: windows from the WM with their titles; "Switch to"
   (activate) and "End task". This needs window information a normal
   client does not get: Task Manager asks the **shell** over a small
   control socket (abstract AF_UNIX name `neoos-shell`, spec 04
   "Control socket") for its window model — the shell already has it (spec
   04), and only one process may hold the WM's shell channel. v1 command
   set: `list-windows`, `activate <surface>`, `close <surface>`.
   If the shell is not running, the tab says so.
3. **Performance**: CPU total and per-CPU bars, a 60-sample line chart
   (`lv_chart`), RAM used/total, uptime, process and thread counts.

End task flow: confirm dialog ("End <name> (pid N)?") → `SIGTERM` → watch
the next samples; if the pid is still there after 3 samples the button
becomes **Kill** (`SIGKILL`). Killing PID 1 or the compositor is refused
with an explanation; killing the shell is allowed (it respawns, spec 04).

## Error handling

- A pid vanishing between `readdir(/proc)` and reading its files is
  normal: skip it silently.
- `kill` errors (`ESRCH`: already gone → refresh; `EPERM`: shown).
- Unparseable procfs line: logged once per field per run
  (`[taskmgr] /proc/12/stat: bad field 14`), the value shown as "?".

## Testing

- Host: procfs parser against fixture files captured from NeoOS *and*
  from Linux (same code must read both); CPU % arithmetic.
- K2 target test: `proctest.nex` (gauntlet) checks that a spinner's
  `utime` grows, a sleeper's does not, `num_threads` matches, `status`
  and `meminfo` parse.
- App target (`make taskmgr`): boot wm + shell + Task Manager + a
  spinner process; serial markers: `[taskmgr] sample pids=N`, the
  spinner's CPU % > 50 on one CPU; synthetic click End task → the
  spinner's exit appears in the init log; screenshot of each tab.

## Assumptions to verify

1. The scheduler's per-thread runtime is readable consistently from
   procfs context (it is read with `p->lock` in `clock_gettime` today).
2. RSS: NeoOS can count resident pages per address space (VMA walk of
   present PTEs, or a maintained counter). If only a walk exists, it is
   O(pages) per read — acceptable at 1 Hz for tens of processes.
3. Thread states map onto Linux letters (blocked-on-waitq → `S`).

## Decision log

| decision | alternatives | why |
|---|---|---|
| Linux-shaped procfs (K2) as the data source | a NeoOS-native "process info" syscall | ports (`ps`, `top`) and Linux itself use procfs; the same parser runs on both |
| Sample only while visible | always | zero cost when minimized; tickless idle stays idle |
| Applications tab via the shell's local socket | a second WM shell channel; WM exposes a public window list | one privileged WM client by design; the shell already has the model |
| SIGTERM then SIGKILL | SIGKILL at once | gives apps (Notepad) the chance to save |
