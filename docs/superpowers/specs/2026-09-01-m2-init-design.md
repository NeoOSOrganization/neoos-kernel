# M2 — init (PID 1) — design

**Date:** 2026-09-01
**Status:** design (brainstormed with user 2026-09-01). Second of the
three console/init milestones: M1a (console plumbing, **DONE** —
`627a113`..`34c17ad`) → **M2 (this)** → M1b (userspace terminal).

**Context:**
- `kernel/kernel.c` — `kmain` currently ends with ~30 hard-coded
  `spawn("/BIN/*.ELF")` calls, then starts the kernel-thread selftests
  (`waitq_selftest_start`, `signal_selftest_start`, `futex_selftest`,
  `smp_parallel_selftest_start`, `smp_steal_selftest_start`), then
  `sti` + `schedule()`.
- `kernel/sched/proc.c` — `process_exit` turns a process into a zombie
  and sends `SIGCHLD` to its parent; `proc_reap` frees the struct.
  `wait4(pid, status, options, rusage)` (`SYS_WAIT4` 32) already
  supports `pid == -1` (any child) and returns `-ECHILD` when a process
  has none. `wait_scan` matches on `p->parent_pid == self->pid`. There
  is **no orphan reparenting** today.
- `kernel/kernel.c:kernel_shutdown()` does the real ACPI S5 poweroff
  (fixed in `733db3e`). It is currently invoked by the
  `user_proc_count` hook in `proc.c` (`user_proc_started` /
  `user_proc_exited`) — a stand-in for exactly the init this milestone
  builds.
- `kernel/syscall/syscall_nr.h` — `SYS_MAX` is 69 (`SYS_POLL` 67,
  `SYS_SELECT` 68 from M1a).
- `lib/` — `spawnv(path, argv)`, `wait4`, `open`/`read`/`close`,
  `printf` all exist. `docs/porting-coreutils.md` notes `execve` with
  argv is still owed; init does not need it (`spawnv` is enough).
- `Makefile` — `$(DISK_IMG)` recipe lays out `::BIN/*.ELF` and a few
  `::DIR/` trees with `mmd`/`mcopy`. `REQUIRED_MARKERS` is a `grep -F`
  list; `make test` powers off in ~11s and treats a `BOOT_TIMEOUT`
  (60s) hit as a hang.

## Problem

The kernel is the process manager: `kmain` names every program to run,
in order, and the machine only stops because a `user_proc_count` hack
in `proc.c` notices the last one exited. There is no PID 1 — no process
that owns orphan reaping, no single place that decides "the workload is
done, shut down", nothing a shell (M3) or a `getty` can be a child of.

M2 makes PID 1 a real ring-3 process. It reads a manifest, launches the
workload, reaps every orphan for the life of the machine, and powers
off via a `reboot(2)` syscall when its manifest entries have all
exited. The kernel goes back to knowing nothing about the test list.

## Goals

- **`reboot(2)`** — a Linux-shaped syscall, `POWER_OFF` / `HALT` /
  `RESTART`, callable only by PID 1 (`-EPERM` otherwise).
- **`/sbin/init.nex`** — an ordinary libneoos ring-3 program that the
  kernel starts as PID 1: parse `/etc/inittab`, `spawnv` each entry,
  `wait4(-1)` in a loop forever (reaping its own children and every
  orphan), and `reboot(POWER_OFF)` once the manifest's entries are all
  gone.
- **Orphan reparenting** — when a process exits, its still-live and
  still-zombie children get `parent_pid = 1`, and PID 1's
  `child_waiters` is woken.
- **PID 1 exiting is a kernel panic.**
- `kmain` launches only `/sbin/init.nex`; the kernel-thread selftests stay.
- The `user_proc_count` poweroff hook is **removed** — `reboot(2)` from
  init is the clean path; `BOOT_TIMEOUT` is the hang backstop.
- `make test` behaviour unchanged: same ~11s, same per-suite marker
  scrape, plus one `[init]` summary marker.
- Every userland-visible shape (`reboot` cmd magic numbers, `wait4`
  semantics, the `SIGCHLD`-to-init contract) matches Linux. Divergences
  in `docs/stdlib.md`.

## Non-goals

- **A shell or `getty`.** M3. The `INITTAB` parser accepts a `respawn`
  mode and M2 ships no `respawn` entries; the machinery is there for M3
  to use.
- **Runlevels, service dependencies, socket activation, cgroups.** This
  is not systemd. One flat manifest, three modes.
- **`execve(path, argv, envp)`.** init uses `spawnv`; a real `execve`
  is still owed (tracked in `docs/abi-compatibility.md`) and unrelated.
- **Sessions / controlling-terminal / job control.** `reboot`'s
  PID-1-only check is the entire permission model; there is no uid, no
  `CAP_SYS_BOOT`. Session hang-up on `reboot` is not done.
- **`/proc`, `kill(1, …)` semantics, `SIGTERM` broadcast on shutdown.**
  init just `reboot()`s; the kernel's poweroff does not first signal
  every process. (A ported service manager would notice; recorded.)
- **A writable `/ETC`.** The manifest is on the read-only FAT image,
  placed there by the Makefile. Editing it needs M3's shell + a
  writable mount.

## Design

### 1. `reboot(2)` — `kernel/syscall/sys_proc.c` (or `sys_misc.c`)

```c
// syscall_nr.h
#define SYS_REBOOT 69
#define SYS_MAX    70

// uapi (lib/include/sys/reboot.h)
#define LINUX_REBOOT_CMD_RESTART   0x01234567
#define LINUX_REBOOT_CMD_HALT      0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc
int reboot(int cmd);
```

Handler:
```c
int64_t sys_reboot(struct syscall_args *a) {
    struct process *p = current_proc();
    if (!p || p->pid != 1) { return -EPERM; }
    switch ((uint32_t)a->a1) {
    case LINUX_REBOOT_CMD_POWER_OFF: kernel_shutdown();          break; // no return
    case LINUX_REBOOT_CMD_HALT:      __asm__("cli"); for(;;) __asm__("hlt");
    case LINUX_REBOOT_CMD_RESTART:
        __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
        for (volatile int i=0;i<100000;i++){}
        // fall through to triple fault if the 8042 pulse did nothing:
        { struct { uint16_t l; uint64_t b; } __attribute__((packed)) z = {0,0};
          __asm__ volatile ("lidt %0; int3" :: "m"(z)); }
        break;
    default: return -EINVAL;
    }
    return 0;  // unreachable for the three real commands
}
```
Table row in `syscall.c`: `[SYS_REBOOT] = { sys_reboot, "reboot" }`.
The table selftest's "implemented == SYS_MAX" check passes (real
handler). The musl shim maps Linux `SYS_reboot` (169) onto it (init is
a libneoos program, but a ported service manager would come through the
shim).

### 2. Orphan reparenting — `kernel/sched/proc.c`

In `process_exit`, once `p->exiting` is set and before/after the
sibling-kill (a point where `p` is committed to dying), walk the
process table for children of `p`:

```c
struct reparent_ctx { int dead_pid; };
static void reparent_one(struct process *c, void *v) {
    struct reparent_ctx *r = v;
    if (c->parent_pid == r->dead_pid) { c->parent_pid = 1; }
}
// in process_exit, after the exit line is printed:
if (p->pid != 1) {
    struct reparent_ctx r = { p->pid };
    proc_table_for_each(reparent_one, &r);   // whatever the existing iterator is called
    struct process *init = proc_find(1);
    if (init) { waitq_wake_all(&init->child_waiters); proc_put(init); }
}
```

- A **zombie** child reparented to 1 is now reapable by init's
  `wait4(-1)` — `wait_scan` matches `parent_pid == 1` and finds it in
  `PROC_ZOMBIE`. Good.
- A **live** child reparented to 1 will `SIGCHLD` init and land in
  init's `wait4` when *it* exits — because `process_exit`'s existing
  `proc_find(p->parent_pid)` + `SIGCHLD` now resolves to init.
- The `proc_table_for_each` iterator: check `proc_table.c` for the
  existing callback-walk (`proc_table_for_each` / `kill_scan` style —
  `wait_scan` is already driven by one). Reuse it; do **not** add a
  new locking scheme.
- Ranks: the walk takes the proc-table lock (rank `PROCTABLE` 0), which
  `process_exit` already touches; no new lock, no new order.

### 3. PID 1 dies → panic — `kernel/sched/proc.c`

At the top of `process_exit`, inside the `if (!p->exiting)` guard:
```c
if (p->pid == 1) {
    serial_write_string("[init] PANIC: init exited, code=");
    serial_write_hex64((uint64_t)(int64_t)code);
    serial_write_string("\n");
    __asm__ volatile ("cli"); for (;;) __asm__ volatile ("hlt");
}
```
A crashed init is unrecoverable; halting loudly beats a silent
`schedule()` with no runnable user threads.

### 4. `kmain` — `kernel/kernel.c`

Replace the block of ~30 `spawn("/BIN/*.ELF")` calls with:
```c
struct process *init = spawn("/sbin/init.nex.ELF");
if (!init || init->pid != 1) {
    serial_write_string("[init] PANIC: could not start /sbin/init.nex (pid=");
    serial_write_hex64(init ? (uint64_t)init->pid : 0);
    serial_write_string(")\n");
    for (;;) __asm__ volatile ("hlt");
}
```
`spawn` allocates pids from `proc_table_alloc_pid()`; init must get 1.
Today PARENT.ELF is the first `spawn` and gets a low pid — check
`pid_alloc.c` / `proc_table_alloc_pid` starts at 1 (it does: the first
allocated pid in a fresh table). If it starts at 0 or reserves 0,
init is pid 1 anyway. Assert it.

Keep, immediately after, the kernel-thread selftests
(`waitq_selftest_start` … `smp_steal_selftest_start`) and then
`sti` + `schedule()`. Remove the `user_proc_started()` seed call and
`user_proc_exited()` drop.

### 5. Remove the `user_proc_count` hook — `kernel/sched/proc.c`, `kernel/kernel.h`

Delete `user_proc_count`, `user_proc_started`, `user_proc_exited`, the
`user_proc_started()` call in `spawn_argv` and `fork_task`, the
`user_proc_exited()` call in `process_exit`, and the two declarations
in `kernel.h`. `kernel_shutdown()` stays and is now called only from
`sys_reboot`. `BOOT_TIMEOUT` in the Makefile stays as the hang
detector (a test that hangs → init never reaches `reboot` → timeout).

### 6. `/sbin/init.nex` — `userland/init.c`

```c
// /etc/inittab: one entry per line. '#' to end-of-line is a comment.
//   <mode> <path>
//   mode: spawn   -- launch, keep going
//         wait    -- launch, block until it exits, then continue
//         respawn -- launch; relaunch whenever it exits (M3's getty)
#define MAX_ENTRIES 64

struct entry { char path[64]; int mode; };  // 0=spawn 1=wait 2=respawn
static struct entry ents[MAX_ENTRIES];
static int nents;

static void parse_inittab(void);   // open("/etc/inittab"), read whole file, split lines

int main(void) {
    parse_inittab();
    if (nents == 0) {
        printf("[init] no /etc/inittab entries -- powering off\n");
        reboot(LINUX_REBOOT_CMD_POWER_OFF);
    }

    int live = 0;
    for (int i = 0; i < nents; i++) {
        if (ents[i].mode == 1 /*wait*/) {
            int pid = spawnv(ents[i].path, (char *[]){ ents[i].path, 0 });
            if (pid > 0) { int st; wait4(pid, &st, 0, 0); }
        } else {
            int pid = spawnv(ents[i].path, (char *[]){ ents[i].path, 0 });
            if (pid > 0) { live++; }
            if (ents[i].mode == 2 /*respawn*/) { /* record pid -> entry for restart */ }
        }
    }

    printf("[init] %d entries launched, reaping\n", live);
    for (;;) {
        int st;
        int pid = wait4(-1, &st, 0, 0);
        if (pid < 0) { break; }               // -ECHILD: nothing left
        // if pid was a respawn entry: spawnv it again, continue
    }
    printf("[init] all entries exited -- powering off\n");
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) { }   // unreachable
}
```

- **Parser**: read the whole file into a fixed buffer (the testtab is
  < 2 KB), walk it line by line, skip blank / `#` lines, `sscanf`-free
  hand split on the first space. Unknown mode word → skip the line and
  `printf("[init] bad inittab line: ...")` (not fatal).
- **`spawnv` argv**: `{ path, NULL }` — `spawnv` already gives argv[0]
  = path when passed a bare path, but init passes it explicitly so the
  contract is visible.
- **respawn bookkeeping**: a small `pid -> entry index` array; on a
  `wait4` return matching a respawn pid, relaunch. M2 ships no respawn
  entries, so this path is exercised only by a future `getty`; keep it
  minimal but present.
- init never exits the `for(;;)` except via `reboot`. If `wait4`
  returns `-ECHILD` while a respawn entry is still meant to be alive,
  relaunch it rather than powering off. With no respawn entries, the
  first `-ECHILD` means the workload is done → poweroff.

### 7. `/etc/inittab` for `make test` — `Makefile`

A new `$(BUILD_DIR)/disk-src/etc/inittab` written by the `$(DISK_IMG)`
recipe, `mmd ::ETC` + `mcopy` it in, plus `mmd ::SBIN` + `mcopy
INIT.ELF ::SBIN/INIT.ELF`. Contents = every binary `kmain` spawns
today, one `spawn` line each, `PARENT.ELF` first (it forks CHILD.ELF —
order only matters for pid-stability of *its* children, and the marker
scrape is pid-independent, so any order is fine; keep PARENT first for
minimal diff to the boot log).

```
# NeoOS test suite -- launched by /sbin/init.nex
spawn /BIN/PARENT.ELF
spawn /BIN/LOOPER.ELF
spawn /BIN/LOOPER.ELF
spawn /BIN/YIELDER.ELF
spawn /BIN/VFSTEST.ELF
... (the full current list, including FBTEST/POLLTEST/PTYTEST)
```

`INIT.ELF` builds from `userland/init.c` with the standard
`$(USER_CFLAGS)` recipe and is added to the `$(DISK_IMG)` prereq list.

`REQUIRED_MARKERS` gains `"[init] all entries exited -- powering off"`.
Everything else in the list is still printed by the individual suites,
unchanged.

**A non-test build** (`make iso`, what the gauntlet runs) ships the
same `INITTAB` — the suite runs there too, exactly as it does now, and
the gauntlet's marker extraction already reads `REQUIRED_MARKERS` from
the Makefile.

### 8. `lib/` — `reboot` wrapper

`lib/syscall.c`:
```c
int reboot(int cmd) { return (int)syscall1(SYS_REBOOT_NR, cmd); }
```
`lib/include/sys/reboot.h` with the three `LINUX_REBOOT_CMD_*` and the
prototype. Add `#include <sys/reboot.h>` where `init.c` needs it.

## Data flow

```
kmain:  spawn("/sbin/init.nex.ELF")  -> pid 1
          |
          v
INIT:   parse /etc/inittab
        spawnv each entry  ---------------------> ~30 test processes
        for(;;) wait4(-1):                         |  each forks children,
          reap own children + every orphan  <------+  exits when done;
          (kernel reparents orphans to pid 1)         orphans' parent_pid := 1
        wait4 == -ECHILD  ->  reboot(POWER_OFF)
          |
          v
kernel: sys_reboot (pid==1 ok) -> kernel_shutdown() -> ACPI S5 -> QEMU exits
```

## Testing

No host unit tests. `make test` (headless QEMU, 4 CPUs) + the parallel
gauntlet.

- **`[init] all entries exited -- powering off`** is the new
  `REQUIRED_MARKER`: it is printed only if init launched the manifest,
  every entry (and every orphan) was reaped, and `reboot` was reached.
  A hang in any suite means init never gets there → `BOOT_TIMEOUT` →
  the marker is missing → `make test` fails, same as a missing suite
  marker today.
- **Every existing suite marker still required and still printed** —
  the suites are unchanged; only their launcher moved.
- **`inittest` (userland, optional)** — a tiny binary in the manifest
  (`wait` mode) that: `reboot(0x4321fedc)` returns `-EPERM` (it is not
  pid 1); `getppid()` (if it exists; else skip) is 1 after its real
  parent — actually its parent IS init, so check `getppid() == 1`;
  `wait4(-1, ...)` with no children returns `-ECHILD`. Marker
  `[inittest] ALL PASSED`.
- **Orphan reaping** — `orphantest` (userland): fork a child; the child
  forks a grandchild that `spawn`s nothing and sleeps 200ms; the child
  exits immediately (orphaning the grandchild); `orphantest` (the
  original) also exits. The grandchild is now init's; it exits after
  its sleep and init's `wait4` reaps it. Verified indirectly: `make
  test` still powers off (if init could not reap the orphan, `wait4`
  would spin returning the same un-reapable pid, or the machine would
  never see `-ECHILD` and never `reboot` → timeout). Add an explicit
  `[init] reaped orphan pid=` debug line for the first orphan reaped so
  the log shows it happened.
- **Gauntlet**: `PGAUNTLET PASSED: 15/15`, ×3 (init is now on the boot
  critical path for *every* userland test — highest-blast-radius
  change in the milestone).
- **Panic paths** (manual, revert after): a deliberately-crashing
  `/sbin/init.nex` → `[init] PANIC: init exited`; an `INITTAB` naming a
  missing binary → init logs `spawnv` failure, keeps going, still
  powers off.

## Risks

- **init not getting pid 1.** `proc_table_alloc_pid()` must return 1
  first. Mitigation: assert `init->pid == 1` in `kmain` and panic
  otherwise; the fix if it does not is a one-line reserve in
  `pid_alloc.c`.
- **Reparenting races the reaper.** A child exiting on CPU B while
  `process_exit` on CPU A reparents it: both take the proc-table lock,
  so the walk sees a consistent `parent_pid`; worst case the child's
  own `SIGCHLD` goes to the old (dying) parent and is lost, but init's
  `wait4` still finds the zombie by the new `parent_pid`. Mitigation:
  reparent *before* the sibling-kill in `process_exit`, and wake
  `init->child_waiters` after.
- **`user_proc_count` removal leaves a gap** if init's `reboot` is
  never reached (bug in the wait loop). Mitigation: `BOOT_TIMEOUT` (the
  Makefile already treats a timeout as failure) and the explicit
  `[init]` markers make the failure legible rather than silent.
- **The manifest is a new file the disk image must carry.** A missing
  `/etc/inittab` → init powers off immediately → every suite marker
  missing → obvious `make test` failure. Low risk, loud symptom.
- **`RESTART`'s triple-fault fallback** is only exercised by hand.
  `POWER_OFF` is the only command `make test` uses; `HALT` and
  `RESTART` are for a human at a console and are simple enough to
  eyeball.

## Documentation at milestone close

- **`docs/stdlib.md`:** a `reboot(2)` entry (the three commands, the
  PID-1-only rule as a divergence — Linux uses `CAP_SYS_BOOT`); an
  `init` / `/etc/inittab` section (format, the three modes, the
  "powers off when the workload is done" behaviour, the note that
  `reboot` does not first `SIGTERM` every process). Update the evdev /
  poll notes only if they reference the old boot flow.
- **`docs/abi-compatibility.md`:** `reboot` implemented (subset,
  PID-1-only). Orphan reparenting now matches Linux. Note that a
  ported service manager still hits: no `SIGTERM`-on-shutdown, no
  sessions, no `/proc`.
- **`README.md`:** "how it boots" — kernel starts `/sbin/init.nex`, which
  runs `/etc/inittab`. Move the milestone count.
- **`docs/superpowers/specs/2026-08-31-post-smp-roadmap.md`:** mark M2
  done, M1b next.
- **This spec** committed under `docs/superpowers/specs/`.

## Self-Review

**Placeholder scan:** the `respawn` bookkeeping in §6 is "a small
`pid -> entry index` array; relaunch on match" — concrete enough, and
M2 ships no respawn entries so it is a written-but-dormant path, not a
TODO. `proc_table_for_each` in §2 is "reuse the existing callback walk
that `wait_scan` already uses" — a real function to locate, not a gap.
No "add error handling".

**Internal consistency:** `SYS_REBOOT` = 69, `SYS_MAX` = 70 in §1 and
the testing section. `LINUX_REBOOT_CMD_POWER_OFF` = `0x4321fedc`
everywhere. `parent_pid = 1` and "init is pid 1" consistent across §2,
§3, §4. The manifest modes (`spawn`/`wait`/`respawn` = 0/1/2) match
between §6 and §7.

**Scope check:** one syscall, one ~150-line userland program, three
small kernel edits (reparent, panic-on-pid-1, kmain), one removal
(`user_proc_count`), Makefile + lib. A single implementation plan.

**Ambiguity check:** "init powers off when its manifest entries have
all exited" — precisely: `wait4(-1)` returns `-ECHILD` and no `respawn`
entry is outstanding (§6). "Orphan reparenting" — both live and zombie
children of the exiting process, to pid 1, under the proc-table lock,
before the sibling-kill (§2, Risks). "PID 1 exiting is a panic" —
`process_exit` with `p->pid == 1`, halt with `cli;hlt` (§3).
