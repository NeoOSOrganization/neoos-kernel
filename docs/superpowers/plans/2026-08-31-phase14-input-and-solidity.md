# Phase 14: Raw Keyboard Input and Correctness-Hardening — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the uncommitted tree as Phase 13, fix five catalogued
correctness bugs, and add `/dev/input/event0` (Linux evdev ABI) so an
application can read raw key events without the TTY line discipline —
with `EVIOCGRAB` as the only thing that severs keyboard → TTY.

**Architecture:** `struct file_ops` gains `ioctl` and `poll`; devfs
devices become `file_ops` implementations with per-open private state,
so `sys_ioctl` stops special-casing the TTY. A reworked keyboard
decoder feeds a new input core (`kernel/dev/input.c`) that fans every
key event out to open evdev clients and — unless a grab is held — to
`tty_input_char()`. A `-DNEOOS_TEST_HOOKS` key-injection syscall makes
all of this deterministically testable under headless QEMU.

**Tech Stack:** C (freestanding, `-ffreestanding -mcmodel=large`),
NASM, x86-64, GNU Make, headless QEMU + serial-log assertions. No host
test runner — every test is a kernel selftest (`serial_write_string`)
or a userland suite whose `[name] ALL PASSED` line is a
`REQUIRED_MARKER` in the `Makefile`.

**Spec:** `docs/superpowers/specs/2026-08-31-phase14-input-and-solidity-design.md`

## Global Constraints

- **No host unit tests.** Every task's "test" is a kernel selftest or a
  userland test binary, verified by `make test` (headless QEMU, 4
  CPUs, COM1 → `build/serial.log`). "Run it to see it fail" means:
  apply only the test, run `make test`, and confirm a `FAILED` line or
  a missing `REQUIRED_MARKER`.
- **A milestone item is not done until a test fails on the pre-fix
  kernel.** (Project rule, `docs/optimization-summary.md` Phase 5c.)
- **The ABI is not ours; internals are.** Struct layouts, flag values,
  and constants that cross into userland must match Linux x86-64
  exactly. Syscall *numbers* stay NeoOS's own. (`CLAUDE.md`.)
- **Every user-facing kernel feature needs a musl-visible path or a
  `lib/` wrapper, plus a `docs/stdlib.md` entry** (or a divergence
  note). (`CLAUDE.md`.)
- **Kernel includes are relative to `kernel/`** (`#include
  "dev/foo.h"`), made to work by `-Ikernel`.
- **Lock rank order is enforced at runtime.** A new lock gets a rank in
  the `LOCK_RANK_*` enum (`kernel/sync/lock.h`); acquiring out of rank
  order panics the boot. Never change an existing rank or acquisition
  order without saying so.
- **Work happens directly on `main`.** Commit per task. End commit
  messages with the `Co-Authored-By:` / `Claude-Session:` trailer used
  by the existing history.
- **Regenerate disk images between runs:** `rm -f build/disk.img
  build/disk2.img && make disk-image` (a write selftest mutates the
  image).
- **US keyboard layout only.** Non-US layouts are out of scope and a
  recorded divergence.

---

## File Structure

**Phase 13 (no new files — commit the existing tree).**

**Solidity sweep:**
- `kernel/syscall/syscall_internal.h` — `O_*` values (fix `O_CREAT`)
- `lib/include/fcntl.h` — `O_*` values (fix `O_CREAT`)
- `kernel/syscall/sys_misc.c` — `sys_nanosleep` blocks instead of spinning
- `kernel/dev/timer.c` / `kernel/sync/waitq.c` — (only if a shared "sleep" waitq helper is cleaner than a file-local one)
- `kernel/sched/proc.c`, `kernel/sched/sched.h`, `kernel/sched/thread.c`, `kernel/sched/sched.c`, `kernel/ipc/signal.c` — remove `proc_list`, rename `proc_lock`
- `kernel/lib/rand.c` + `kernel/lib/rand.h` (new) — the CSPRNG
- `kernel/sched/proc.c` — draw `AT_RANDOM` from the CSPRNG
- `kernel/ipc/signal.c` — `NEOOS_DEBUG_STOP_WINDOW` yield
- `userland/sigtest.c`, `userland/stattest.c` — regression assertions

**Input subsystem:**
- `kernel/fs/file.h`, `kernel/fs/file.c` — `file_ops` gains `ioctl`, `poll`; dispatch helpers
- `kernel/syscall/sys_file.c` — `sys_ioctl` calls `f->ops->ioctl`
- `kernel/fs/devfs.c`, `kernel/fs/devfs.h` — device entries carry a `file_ops *` + open constructor; per-open `priv`
- `kernel/dev/tty.c`, `kernel/dev/tty.h` — a `struct file_ops tty_file_ops`
- `kernel/dev/keyboard.c`, `kernel/dev/keyboard.h` — the Set-1 decoder, modifier state, `struct key_event`
- `kernel/dev/input.c` + `kernel/dev/input.h` (new) — the input core, fan-out, grab, `struct evdev_client`, ring buffer, `input_selftest`, `input_inject_key`
- `kernel/dev/evdev.c` + `kernel/dev/evdev.h` (new) — the `event0` `file_ops` and the `EVIOC*` handlers
- `lib/include/linux/input.h`, `lib/include/linux/input-event-codes.h` (new) — userland UAPI subset
- `kernel/syscall/syscall_nr.h`, `kernel/syscall/syscall.c`, `kernel/syscall/sys_misc.c` — `SYS_TEST_HOOK` (compiled under `-DNEOOS_TEST_HOOKS`)
- `lib/syscall.c`, `lib/include/*.h` — the test-hook wrapper (test builds only)
- `userland/evtest.c` (new), `userland/ttytest.c` (extend)
- `Makefile` — build rules, `mcopy` lines, `REQUIRED_MARKERS`, `-DNEOOS_TEST_HOOKS` for `make test`
- `kernel/kernel.c` — `input_init`, `input_selftest`, `spawn("/BIN/EVTEST.ELF")`
- `docs/stdlib.md`, `docs/abi-compatibility.md`, `README.md`

---

## Task 1: Land the existing tree as Phase 13

**Files:**
- Modify: the entire uncommitted working tree (already staged renames + unstaged content + untracked files)
- Modify: `docs/abi-compatibility.md` (header date)

**Interfaces:**
- Consumes: nothing.
- Produces: a clean `git status` (only `.idea/vcs.xml` may remain dirty). Every later task starts from committed HEAD.

- [ ] **Step 1: Review the `tlb.c` fix**

Run: `git diff kernel/smp/tlb.c`
Read it against the hazard it fixes: the old fixed 64-entry deferred-free
array called `tlb_shootdown(0)` when full, which is illegal because
`vma_munmap` unmaps under `mm_lock` and `tlb_shootdown` asserts
`lock_held_depth() == 0`. The fix makes the queue unbounded (256-entry
fast path + a `kmalloc`ed overflow list allocated *before*
`deferred_lock`; on OOM the frame is leaked with a log line). Confirm
the overflow node is allocated with `deferred_lock` NOT held.

- [ ] **Step 2: Run the stability gauntlet**

Run:
```bash
rm -f build/disk.img build/disk2.img
for i in $(seq 1 15); do
  rm -f build/disk.img build/disk2.img
  make test 2>&1 | grep -E "ALL PASSED|PASS:|FAILED|BOOT DID NOT|MISSING|PANIC" | tail -3
  echo "--- run $i done ---"
done
```
Expected: 15/15 runs end with all `REQUIRED_MARKERS` present and no
`FAILED`/`PANIC`/`BOOT DID NOT COMPLETE`. The bug this guards against
(`[lock] PANIC: tlb_shootdown with a lock held`) hits ~1 in 10, so
fewer than 15 clean runs is a Phase 13 blocker — debug it before
continuing (see the handoff doc §4: a run with zero `[timer] tick=`
lines means a CPU is spinning with interrupts off).

- [ ] **Step 3: Commit the tree in dependency-ordered slices**

The index already holds the `kernel/` renames. Commit in this order, one
`git commit` per slice, staging only that slice's paths
(`git add -- <paths>` / `git restore --staged` as needed). Use the
`Phase 13:` message prefix and the standard trailer.

1. `kernel/` source reorg (`arch/ dev/ fs/ ipc/ net/ smp/ sync/ syscall/`) + the `-Ikernel` switch in the `Makefile` + every `#include` rewrite.
2. Syscall dispatch table split (`kernel/syscall/sys_*.c`, `syscall.c`, `syscall_internal.h`).
3. `struct stat` + the `stat`/`lstat`/`fstat`/`newfstatat` family (`kernel/fs/stat.h`, `sys_file.c`, `lib/include/sys/stat.h`).
4. The working directory (`chdir`/`getcwd`, `..` textual resolution) across `kernel/fs/vfs.c`, `sched/proc.c`, `syscall/sys_file.c`, `lib/`.
5. VFAT long names (`kernel/fs/fatfs.c`, `fatfs.h`, `vfs.h` `VFS_NAME_MAX`).
6. RTC + `CLOCK_REALTIME` anchoring (`kernel/dev/rtc.c`, `rtc.h`, `sys_misc.c`).
7. The TTY line discipline (`kernel/dev/tty.c`, `tty.h`, `keyboard.c`, `fs/devfs.c`).
8. `struct vnode` timestamps + FAT decode + `vfs_stat_vnode` wiring (spec §4.3 — already implemented here).
9. Tier-0 syscalls (`writev`/`readv`, `ioctl`, `clock_gettime`, `nanosleep`, `set_tid_address`, `exit_group`) + `lib/include/sys/uio.h`, `termios.h`.
10. The test binaries and `Makefile`/`kernel.c` wiring for `stattest`, `cwdtest`, `direnttest`, `lfntest`, `tier0test`, `ttytest` and the `third_party/shim/` tree + `userland/musl/`.
11. The doc refresh (`README.md`, `docs/abi-compatibility.md`, `docs/stdlib.md`, `docs/porting-coreutils.md`, `docs/optimization-summary.md`, `third_party/musl-README.md`) and the `sys_misc.c` clock-comment fix.

If a slice does not build in isolation, widen it until it does — a
bisectable history matters more than the exact count.

- [ ] **Step 4: Set the ABI report date**

Edit `docs/abi-compatibility.md`'s header line to read "close of Phase
13" and commit it as slice 11 or a final `Phase 13:` commit.

- [ ] **Step 5: Verify a clean tree and a green build**

Run: `git status` → only `.idea/vcs.xml` dirty (ignore it).
Run: `rm -f build/disk.img build/disk2.img && make test`
Expected: all `REQUIRED_MARKERS` present, no `FAILED`.

---

## Task 2: `O_CREAT` = 0x40, and the `O_*` / `AT_*` audit

**Files:**
- Modify: `kernel/syscall/syscall_internal.h:38-53` (the `O_*` block)
- Modify: `lib/include/fcntl.h:4-14`
- Test: `userland/fileio.c` (add a Linux-value create case)
- Modify: `docs/stdlib.md` (the corrected table)

**Interfaces:**
- Consumes: nothing.
- Produces: `O_CREAT == 0x40` everywhere. No new symbols.

- [ ] **Step 1: Write the failing test**

In `userland/fileio.c`, add (near the other open cases):
```c
// O_CREAT must be Linux's 0x40: a program compiled against Linux
// headers passes exactly this.
#define LINUX_O_CREAT 0x40
static void check_linux_o_creat(void) {
    int fd = open("/OCREAT.TMP", 1 /*O_WRONLY*/ | LINUX_O_CREAT, 0);
    if (fd < 0) { printf("[fileio] FAILED: O_CREAT 0x40 did not create, rc=%d\n", fd); failures++; return; }
    close(fd);
    struct stat st;
    if (stat("/OCREAT.TMP", &st) != 0) { printf("[fileio] FAILED: created file not stat-able\n"); failures++; return; }
    unlink("/OCREAT.TMP");
    printf("[fileio] O_CREAT=0x40 passed\n");
}
```
Call it from `fileio`'s `main` and include `<sys/stat.h>`.

- [ ] **Step 2: Run it to see it fail**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'fileio|FAILED'`
Expected: `[fileio] FAILED: O_CREAT 0x40 did not create` (the current
`O_CREAT` is `0x100`, so `0x40` is an unknown flag and no file is made).

- [ ] **Step 3: Fix the constant and audit the rest**

In `kernel/syscall/syscall_internal.h` and `lib/include/fcntl.h`:
```c
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040   /* was 0x0100 */
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000
```
Grep the kernel for every use of the old `0x100` value and for
`O_CREAT` to be sure nothing hard-codes it. Check `third_party/shim/`
does not redefine `O_*` (it should not — these are generic). Add
`O_EXCL`/`O_DIRECTORY` handling to `sys_open` only if trivial;
otherwise leave them defined-but-unimplemented and note it.

- [ ] **Step 4: Run tests to verify they pass**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'fileio|ALL PASSED|FAILED'`
Expected: `[fileio] O_CREAT=0x40 passed`, `[fileio] ALL PASSED`, and
every other `REQUIRED_MARKER` still present (nothing else used `0x100`).

- [ ] **Step 5: Document**

In `docs/stdlib.md`, update the `O_*` list: `O_CREAT` now `0x40`
(matches Linux), note `O_EXCL`/`O_DIRECTORY` are defined and whether
they are honoured. In `docs/abi-compatibility.md` §5, change "**`O_CREAT`
STILL DIVERGES**" to resolved.

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/syscall_internal.h lib/include/fcntl.h userland/fileio.c docs/stdlib.md docs/abi-compatibility.md
git commit -m "Phase 14: O_CREAT is Linux's 0x40, not 0x100

<trailer>"
```

---

## Task 3: `nanosleep` blocks instead of busy-spinning

**Files:**
- Modify: `kernel/syscall/sys_misc.c:101-117` (`sys_nanosleep`)
- Test: a new kernel selftest `sleep_selftest` in `kernel/dev/timer.c` (or `sys_misc.c`), called from `kernel/kernel.c`
- Modify: `kernel/dev/timer.h` (declare the selftest)
- Modify: `docs/stdlib.md`

**Interfaces:**
- Consumes: `waitq_sleep_timeout(struct waitq *q, struct spinlock *release, uint64_t deadline)` and `waitq_init` from `kernel/sync/waitq.h`; `timer_ticks()` from `kernel/dev/timer.h`.
- Produces: `sys_nanosleep` unchanged signature; new `void sleep_selftest(void);`.

- [ ] **Step 1: Write the failing test**

Add to `kernel/dev/timer.c` (declare in `timer.h`):
```c
// A thread that sleeps must not spin the scheduler. Before the fix,
// sys_nanosleep looped calling schedule() for the whole sleep; this
// counts idle-thread activations on the current CPU across a 100ms
// sleep issued from a kernel thread and asserts it stays small.
static volatile uint64_t sleep_test_passes;
static void sleep_test_thread(void *arg) {
    (void)arg;
    struct k_timespec req = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
    uint64_t before = sched_idle_passes_this_cpu();   // add this accessor to sched.c
    /* issue the sleep through the same path userland uses */
    struct syscall_args a = { .a1 = (uint64_t)&req };
    sys_nanosleep(&a);
    sleep_test_passes = sched_idle_passes_this_cpu() - before;
    thread_exit_self(0);
}
void sleep_selftest(void) {
    /* spawn sleep_test_thread pinned to a known CPU, join it */
    /* ... use the existing kernel-thread spawn+join helper ... */
    if (sleep_test_passes > 100) {
        serial_write_string("[sleep] selftest FAILED: nanosleep spun, passes=");
        serial_write_hex64(sleep_test_passes);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[sleep] selftest passed\n");
}
```
If a per-CPU idle-pass counter is awkward, use a simpler proxy: a
second kernel thread on the same CPU increments a counter in a tight
`schedule()` loop for the sleep's duration; post-fix it gets far *more*
turns than pre-fix because the sleeper is off the run queue. Assert the
direction.

Add `"[sleep] selftest passed"` to `REQUIRED_MARKERS`.

- [ ] **Step 2: Run it to see it fail**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'sleep|FAILED'`
Expected: `[sleep] selftest FAILED: nanosleep spun`.

- [ ] **Step 3: Implement the blocking sleep**

Replace the body of `sys_nanosleep` after the argument validation:
```c
    uint64_t ns    = (uint64_t)req->tv_sec * 1000000000ULL + (uint64_t)req->tv_nsec;
    uint64_t ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
    if (ticks == 0 && ns > 0) { ticks = 1; }
    if (ticks == 0) { return 0; }

    uint64_t deadline = timer_ticks() + ticks;
    struct waitq q;
    waitq_init(&q);
    // Nothing ever wakes this queue; waitq_timeout_tick() dequeues the
    // sleeper when timer_ticks() reaches `deadline`. -EINTR if the
    // thread is killed while blocked, matching the interrupted-sleep
    // contract (rem is still ignored — documented).
    int rc = waitq_sleep_timeout(&q, NULL, deadline);
    if (rc == -EINTR) { return -EINTR; }
    return 0;
```
Confirm `waitq_timeout_tick()` is already called from the timer IRQ on
every CPU (it is — futex timeouts depend on it).

- [ ] **Step 4: Run tests to verify they pass**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'sleep|tier0|ipctest|ALL PASSED|FAILED'`
Expected: `[sleep] selftest passed`; `tier0test` / `ipctest` (which use
timed waits) still `ALL PASSED`.

- [ ] **Step 5: Document**

`docs/stdlib.md`: `nanosleep` now genuinely blocks; still relative,
still rounds up to a 10ms tick, still ignores `rem`, now returns
`-EINTR` if the thread is killed mid-sleep.

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/sys_misc.c kernel/dev/timer.c kernel/dev/timer.h kernel/kernel.c Makefile kernel/sched/sched.c docs/stdlib.md
git commit -m "Phase 14: nanosleep blocks on a timer waitq instead of spinning

<trailer>"
```

---

## Task 4: `stattest` timestamp regression assertion

**Files:**
- Test: `userland/stattest.c` (add a case)

**Interfaces:**
- Consumes: `stat`, `clock_gettime(CLOCK_REALTIME, …)` from userland.
- Produces: nothing.

The `struct vnode` timestamp fields, the FAT decode, and
`vfs_stat_vnode` already exist (spec §4.3). This task adds cover so a
regression is caught.

- [ ] **Step 1: Write the test**

In `userland/stattest.c`:
```c
static void check_mtime_is_real(void) {
    int fd = open("/MTIME.TMP", 1 | 0x40, 0);   // O_WRONLY|O_CREAT (Linux value; needs Task 2)
    if (fd < 0) { fail("open for mtime", fd); return; }
    write(fd, "x", 1);
    close(fd);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    struct stat st;
    if (stat("/MTIME.TMP", &st) != 0) { fail("stat MTIME.TMP", -1); return; }
    unlink("/MTIME.TMP");

    long long skew = (long long)now.tv_sec - (long long)st.st_mtime;
    if (skew < 0) skew = -skew;
    // FAT time resolution is 2s; the RTC epoch may be a few seconds
    // stale. Allow a generous window; the point is "not zero, and not
    // 1980".
    if (st.st_mtime == 0 || skew > 120) {
        fail("st_mtime not close to CLOCK_REALTIME", (int)st.st_mtime);
        return;
    }
    printf("[stattest] mtime is real passed\n");
}
```

- [ ] **Step 2: Run it — expect PASS (this is regression cover, not a bug fix)**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'stattest'`
Expected: `[stattest] mtime is real passed`, `[stattest] ALL PASSED`.
If it *fails*, the Phase 13 timestamp code regressed — stop and fix.

- [ ] **Step 3: Commit**

```bash
git add userland/stattest.c
git commit -m "Phase 14: assert stat timestamps track the wall clock

<trailer>"
```

---

## Task 5: A deterministic SIGSTOP/SIGCONT reproducer

**Files:**
- Modify: `kernel/ipc/signal.c` (`signal_do_stop` / `signal_do_continue`)
- Modify: `Makefile` (thread `NEOOS_DEBUG_STOP_WINDOW` through `CFLAGS` when set)
- Test: `userland/sigtest.c` (`check_stop_continue_race`, bump rounds; add a note)
- Modify: `docs/superpowers/specs/2026-08-31-handoff.md` or a new handoff note

**Interfaces:**
- Consumes: `schedule()` from `kernel/sched/sched.h`.
- Produces: no new symbols; a build-time switch only.

- [ ] **Step 1: Add the debug window**

In `kernel/ipc/signal.c`, at the exact point between the stop-state
transition and the wake decision that the fix made atomic (find it via
`git log -p` for commit `230cdcf`), insert:
```c
#ifdef NEOOS_DEBUG_STOP_WINDOW
    // Widen the stop/continue race window so sigtest can hit it. A
    // continue that arrives now, pre-fix, is lost. Test builds only.
    schedule();
#endif
```
Place it so that with the fix reverted it reproduces the "continued
thread never runs" symptom, and with the fix in place it is harmless.

- [ ] **Step 2: Thread the switch through the build**

In the `Makefile`, where kernel `CFLAGS` are assembled:
```make
ifdef NEOOS_DEBUG_STOP_WINDOW
CFLAGS += -DNEOOS_DEBUG_STOP_WINDOW
endif
```

- [ ] **Step 3: Strengthen the test**

In `userland/sigtest.c`, raise `STOP_RACE_ROUNDS` from 10 to e.g. 200
and add a comment: this loop is a *reliable* reproducer only when the
kernel is built `NEOOS_DEBUG_STOP_WINDOW=1`; the normal build keeps it
as cheap smoke cover.

- [ ] **Step 4: Verify the reproducer both ways**

Run (fix present, switch on):
```bash
rm -f build/disk.img build/disk2.img
make test NEOOS_DEBUG_STOP_WINDOW=1 2>&1 | grep -E 'sigtest|FAILED'
```
Expected: `[sigtest] SIGSTOP/SIGCONT race passed`, `[sigtest] ALL PASSED`.

Then temporarily revert the atomicity fix (stash it), rebuild with the
switch, and confirm `[sigtest] FAILED: stop/cont race`. Restore the fix.

Run (normal build): `make test 2>&1 | grep sigtest` → still `ALL PASSED`.

- [ ] **Step 5: Record it**

Add a short section to the handoff notes: `NEOOS_DEBUG_STOP_WINDOW=1` is
a manual regression check, not part of CI, and what it proves.

- [ ] **Step 6: Commit**

```bash
git add kernel/ipc/signal.c Makefile userland/sigtest.c docs/superpowers/
git commit -m "Phase 14: a deterministic SIGSTOP/SIGCONT race reproducer

<trailer>"
```

---

## Task 6: Remove the `proc_list` shadow and its linear scans

**Files:**
- Modify: `kernel/sched/proc.c` (drop `proc_list`; iterate via `proc_table`)
- Modify: `kernel/sched/sched.h:13-14` (drop `proc_list` extern; rename lock)
- Modify: `kernel/sched/thread.c:181-188` (rename lock use)
- Modify: `kernel/sched/sched.c:228-231` (rename lock use)
- Modify: `kernel/ipc/signal.c` (rename lock use; comments at `proc.c:654`)
- Read: `kernel/sched/proc_table.h` (the iterator API)
- Test: `userland/vfstest.c` or `userland/fork_test.c` (a lookup/reap assertion)

**Interfaces:**
- Consumes: `proc_table` lookup + an iteration primitive from `kernel/sched/proc_table.h`. If no "for each process" iterator exists, add `void proc_table_for_each(void (*fn)(struct process *, void *), void *ctx);` that walks every bucket under the bucket locks in rank order.
- Produces: `proc_list` and its `extern` gone. `proc_lock` renamed to `proc_global_lock` (guards `kzombies` + process-group signal ordering), same rank `LOCK_RANK_PROCTABLE`, same acquisition order.

- [ ] **Step 1: Write the failing/holding test**

In `userland/fork_test.c` add:
```c
// A child must be findable by PID the instant fork returns, and gone
// the instant it is reaped — with no separate proc_list to drift out
// of sync with the hash table.
static void check_pid_visibility(void) {
    int pid = fork();
    if (pid == 0) { _exit(7); }
    // parent: kill(pid, 0) probes existence without sending a signal
    if (kill(pid, 0) != 0) { printf("[forktest] FAILED: child not visible by pid\n"); failures++; }
    int st = 0;
    waitpid(pid, &st, 0);
    if (kill(pid, 0) == 0) { printf("[forktest] FAILED: reaped child still visible\n"); failures++; }
    if (!failures) printf("[forktest] pid visibility passed\n");
}
```
(If `fork_test.c`'s marker is not required, use `vfstest.c` which is.)

- [ ] **Step 2: Run it — expect PASS today, it is a guard for the refactor**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'forktest|FAILED'`
Expected: `[forktest] pid visibility passed`. Keep this green through
every step below.

- [ ] **Step 3: Add a `proc_table` iterator if missing**

In `kernel/sched/proc_table.c`/`.h`, add `proc_table_for_each` (or an
open-coded bucket walk). It must take the bucket locks in ascending
index order and never call back into code that re-enters the table.

- [ ] **Step 4: Replace the scans**

At `proc.c:654`, `:672`, `:826` and any `wait4` scan, replace
`for (struct process *p = proc_list; p; p = p->next)` with the
iterator. Preserve the existing lock discipline: where `proc_lock` was
held across `sig_lock` for group-signal ordering, keep holding the
renamed `proc_global_lock` across `sig_lock` — the ordering constraint
is real.

- [ ] **Step 5: Delete `proc_list`, rename the lock**

Remove `proc_list` (decl, the add at `proc.c:87-91`, the remove at
`proc.c:808-813`, the `extern` in `sched.h`). Rename `proc_lock` →
`proc_global_lock` everywhere (`proc.c`, `sched.h`, `thread.c`,
`sched.c`, `signal.c`), keeping `spin_init(&proc_global_lock,
LOCK_RANK_PROCTABLE, "proc_global")`. Update the "`DEPRECATED`" and
"`proc_list`" comments to describe what the lock now guards.

- [ ] **Step 6: Run the full suite**

Run: `rm -f build/disk.img build/disk2.img && make test`
Expected: every `REQUIRED_MARKER` present; **no** `[lock] PANIC`
(a rank inversion from the rename would panic here); `sigtest`,
`fork_test`, `smptest` all `ALL PASSED`. Run it 3× for the
concurrency-sensitive paths.

- [ ] **Step 7: Commit**

```bash
git add kernel/sched/ kernel/ipc/signal.c userland/fork_test.c
git commit -m "Phase 14: proc_table is the only process store; drop proc_list

<trailer>"
```

Note: this is the scoped version — the lock stays (renamed). Full
removal of `proc_global_lock` is Phase 15. If the `kzombies` /
`sig_lock` entanglement turns out shallow while doing this, finishing
the removal here is fine; if not, stop at the scoped version.

---

## Task 7: `AT_RANDOM` from a seeded CSPRNG

**Files:**
- Create: `kernel/lib/rand.c`, `kernel/lib/rand.h`
- Modify: `kernel/kernel.c` (call `rand_init()` early, after RTC init)
- Modify: `kernel/sched/proc.c:288-299` (draw `AT_RANDOM` from `rand_bytes`)
- Modify: `Makefile` (compile `kernel/lib/rand.c`)
- Test: kernel `rand_selftest` called from `kernel.c`
- Modify: `docs/stdlib.md`, `docs/abi-compatibility.md` §8

**Interfaces:**
- Consumes: `rtc_boot_epoch()` (`kernel/dev/rtc.h`); a `RDTSC` helper (add `static inline uint64_t rdtsc(void)` to `kernel/arch/cpu.h` if absent); `cpuid` to test `RDRAND` (`ECX bit 30` of leaf 1).
- Produces:
  ```c
  void     rand_init(void);                 // seed once, at boot
  void     rand_bytes(void *buf, uint64_t n);
  uint64_t rand_u64(void);
  void     rand_selftest(void);
  ```

- [ ] **Step 1: Write the failing test**

`kernel/lib/rand.c`:
```c
void rand_selftest(void) {
    uint8_t a[16], b[16];
    rand_bytes(a, 16);
    rand_bytes(b, 16);

    int all_zero = 1, sequential = 1, same = 1;
    for (int i = 0; i < 16; i++) {
        if (a[i] != 0) all_zero = 0;
        if (i && a[i] != (uint8_t)(a[i-1] + 1)) sequential = 0;
        if (a[i] != b[i]) same = 0;
    }
    if (all_zero || sequential || same) {
        serial_write_string("[rand] selftest FAILED\n");
        return;
    }
    serial_write_string("[rand] selftest passed\n");
}
```
Wire `rand_selftest()` into `kernel.c` and add `"[rand] selftest
passed"` to `REQUIRED_MARKERS`.

- [ ] **Step 2: Run it to see it fail**

Build with a stub `rand_bytes` that `memset`s zero. Run: `make test 2>&1
| grep rand` → `[rand] selftest FAILED`.

- [ ] **Step 3: Implement the CSPRNG**

```c
// splitmix64 to expand the seed, xoshiro256** as the stream. NOT a
// real entropy pool: no reseeding, no /dev/random. It exists so
// AT_RANDOM — which musl turns into the stack-guard canary — is not
// the tick counter. docs/stdlib.md records the limitation.
static uint64_t s[4];
static uint64_t sm_next(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void rand_init(void) {
    uint64_t seed = (uint64_t)rtc_boot_epoch();
    seed ^= rdtsc();
    seed ^= (uint64_t)(uintptr_t)&seed;
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (c & (1u << 30)) {           // RDRAND available
        uint64_t r;
        if (rdrand64(&r)) { seed ^= r; }
    }
    uint64_t sm = seed;
    for (int i = 0; i < 4; i++) { s[i] = sm_next(&sm); }
}

uint64_t rand_u64(void) {
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 45);
    return result;
}

void rand_bytes(void *buf, uint64_t n) {
    uint8_t *p = buf;
    while (n) {
        uint64_t r = rand_u64();
        uint64_t k = n < 8 ? n : 8;
        for (uint64_t i = 0; i < k; i++) { p[i] = (uint8_t)(r >> (i * 8)); }
        p += k; n -= k;
    }
}
```
Add `rdrand64` and `cpuid` helpers to `kernel/arch/cpu.h`/`cpu.c` if not
present. `rand_init()` needs a lock only if called after other CPUs are
up — call it on the BSP before `smp_start`, so no lock is needed;
`rand_u64` afterwards needs a spinlock (rank near `LOCK_RANK_SERIAL`,
i.e. a leaf) since `AT_RANDOM` is drawn during `spawn` on any CPU.

- [ ] **Step 4: Draw `AT_RANDOM` from it**

In `kernel/sched/proc.c` `build_initial_stack`, replace the `mix`
computation with:
```c
    uint8_t at_random_bytes[16];
    rand_bytes(at_random_bytes, 16);
    if (!poke_user_bytes(pml4_phys, at_random, at_random_bytes, 16)) { return 0; }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'rand|tlstest|musltest|FAILED'`
Expected: `[rand] selftest passed`; `tlstest` and `musltest` still
`ALL PASSED` (musl consumes `AT_RANDOM` at startup).

- [ ] **Step 6: Document**

`docs/stdlib.md`: `AT_RANDOM` is now a seeded CSPRNG (splitmix64 +
xoshiro256\*\*), seeded from RTC ⊕ TSC ⊕ a stack address ⊕ `RDRAND`
when present. **Still not an entropy pool** — no reseeding, no
`getrandom`, no `/dev/random`. `docs/abi-compatibility.md` §8: change
"`AT_RANDOM` DIVERGES: not random" to "seeded PRNG; adequate for the
stack guard, not for keys".

- [ ] **Step 7: Commit**

```bash
git add kernel/lib/rand.c kernel/lib/rand.h kernel/arch/cpu.h kernel/arch/cpu.c kernel/sched/proc.c kernel/kernel.c Makefile docs/stdlib.md docs/abi-compatibility.md
git commit -m "Phase 14: seed AT_RANDOM from a CSPRNG, not the tick counter

<trailer>"
```

---

## Task 8: `file_ops` gains `ioctl` and `poll`; devfs devices become `file_ops`

**Files:**
- Modify: `kernel/fs/file.h` (extend `struct file_ops`; add dispatch helpers)
- Modify: `kernel/fs/file.c` (`file_ioctl`, `file_poll`; extend `vnode_file_ops` with truthful stubs)
- Modify: `kernel/ipc/pipe.c` (add `ioctl`/`poll` to the pipe ops — `ioctl` → `-ENOTTY`, `poll` → real readiness)
- Modify: `kernel/net/socket.c` (add `ioctl` → `-ENOTTY` for now, `poll` → real readiness)
- Modify: `kernel/syscall/sys_file.c:423-445` (`sys_ioctl` → `f->ops->ioctl`)
- Modify: `kernel/fs/devfs.c`, `kernel/fs/devfs.h` (device entries carry `const struct file_ops *` + an open constructor; per-open `priv`)
- Modify: `kernel/dev/tty.c`, `kernel/dev/tty.h` (expose `struct file_ops tty_file_ops`)
- Read: `kernel/sched/proc.h:36-56` (`struct file_descriptor`)
- Test: extend `userland/ttytest.c` `check_isatty` (already asserts `ioctl` on non-tty is `-ENOTTY`); add a kernel `file_selftest` case for the new ops being non-NULL.

**Interfaces:**
- Consumes: `struct file_descriptor` (`ops`, `priv`, `vn`, `nonblock`).
- Produces:
  ```c
  // in struct file_ops:
  int64_t (*ioctl)(struct file_descriptor *f, uint64_t request, void *arg);
  int     (*poll)(struct file_descriptor *f, int events);   // returns POLL* mask

  // dispatch helpers in file.h/.c:
  int64_t file_ioctl(struct file_descriptor *f, uint64_t request, void *arg);
  int     file_poll (struct file_descriptor *f, int events);

  // POLL* bits (kernel-internal, but must equal Linux's for Phase 15):
  #define POLLIN  0x001
  #define POLLOUT 0x004
  #define POLLERR 0x008
  #define POLLHUP 0x010
  #define POLLNVAL 0x020

  // devfs.h:
  struct devfs_dev {
      const char *name;
      enum vnode_type type;
      const struct file_ops *fops;
      int (*open)(struct file_descriptor *f);   // fills f->priv; may return -errno
  };

  // tty.h:
  extern const struct file_ops tty_file_ops;
  ```

- [ ] **Step 1: Write the failing test**

In `kernel/fs/file.c` `file_selftest`, add:
```c
    if (!vnode_file_ops.ioctl || !vnode_file_ops.poll) {
        serial_write_string("[file] selftest FAILED: ioctl/poll op is NULL\n");
        return;
    }
```
In `userland/ttytest.c`, `check_isatty` already opens a regular file and
asserts `isatty` is 0; add an explicit `ioctl(fd, TIOCGWINSZ, &ws)`
returns `-ENOTTY` (negative) on that file.

- [ ] **Step 2: Run it to see it fail**

Run: `make test 2>&1 | grep -E 'file\] selftest|ttytest'`
Expected: `[file] selftest FAILED: ioctl/poll op is NULL` (the fields
don't exist / are zero yet).

- [ ] **Step 3: Extend `struct file_ops` and the dispatch layer**

Add the two op pointers to `struct file_ops` in `file.h`, the `POLL*`
macros, and `file_ioctl`/`file_poll` helpers in `file.c` that handle a
NULL `ops` (vnode-backed default: `ioctl` → `-ENOTTY`, `poll` → "ready
for read and write"). Give `vnode_file_ops`, the pipe ops and the
socket ops real entries — no NULL pointers, per the existing rule.

- [ ] **Step 4: Generalise `sys_ioctl`**

`kernel/syscall/sys_file.c`:
```c
int64_t sys_ioctl(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_process(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_ioctl(f, (uint64_t)a->a2, (void *)(uintptr_t)a->a3);
}
```
Delete the `devfs_vnode_is_tty()` branch and the function if now unused
(keep it only if `isatty` elsewhere still needs it — it should not,
since `isatty` is a userland `ioctl(TIOCGWINSZ)` probe).

- [ ] **Step 5: Convert devfs**

Change `struct devfs_node` to `struct devfs_dev` (name, type, `fops`,
`open`). Provide:
- `null_file_ops` / `zero_file_ops` — `read`/`write` as today, `ioctl` → `-ENOTTY`, `poll` → always ready, `dup`/`close` no-ops, no `priv`.
- `CONSOLE`/`TTY` → `&tty_file_ops`, `open` sets `f->priv = &console_tty`.
The devfs `open` path (find where an fd is created for a devfs vnode —
`sys_open`) must call `dev->fops` into `f->ops` and `dev->open(f)`.

- [ ] **Step 6: Give the TTY a `file_ops`**

In `tty.c`:
```c
static int64_t tty_fop_read (struct file_descriptor *f, void *b, uint64_t n)        { (void)f; return tty_read(b, (uint32_t)n); }
static int64_t tty_fop_write(struct file_descriptor *f, const void *b, uint64_t n)  { (void)f; return tty_write(b, (uint32_t)n); }
static int64_t tty_fop_ioctl(struct file_descriptor *f, uint64_t req, void *arg)    { (void)f; return tty_ioctl(req, arg); }
static int     tty_fop_poll (struct file_descriptor *f, int events)                 { (void)f; return tty_poll(events); }
const struct file_ops tty_file_ops = { .name="tty", .read=tty_fop_read, .write=tty_fop_write,
    .lseek=..., .getdents=..., .ioctl=tty_fop_ioctl, .poll=tty_fop_poll, .dup=..., .close=... };
```
Add `int tty_poll(int events)` returning `POLLIN` when `console_tty`
has a complete line / satisfied `VMIN`, else 0; `POLLOUT` always.

- [ ] **Step 7: Run the full suite**

Run: `rm -f build/disk.img build/disk2.img && make test`
Expected: `[file] selftest passed`, `ttytest`/`vfstest`/`pipetest`/
`nettest` all `ALL PASSED`, no `FAILED`. The `isatty` path is now
"does `ioctl(TIOCGWINSZ)` succeed", so re-check `ttytest`'s isatty
cases pass.

- [ ] **Step 8: Commit**

```bash
git add kernel/fs/file.h kernel/fs/file.c kernel/ipc/pipe.c kernel/net/socket.c kernel/syscall/sys_file.c kernel/fs/devfs.c kernel/fs/devfs.h kernel/dev/tty.c kernel/dev/tty.h userland/ttytest.c
git commit -m "Phase 14: file_ops gains ioctl and poll; devfs devices use it

<trailer>"
```

---

## Task 9: Keyboard decoder — Set-1 + extended keys + modifiers → Linux keycodes

**Files:**
- Modify: `kernel/dev/keyboard.c` (the decoder; stop calling `tty_input_char` directly)
- Modify: `kernel/dev/keyboard.h` (`struct key_event`, the decode entry point)
- Create: `kernel/dev/keymap_us.h` (scancode → keycode + char tables)
- Test: a kernel `keyboard_decode_selftest` (pure function, no IRQ) called from `kernel.c`

**Interfaces:**
- Consumes: nothing (pure decode). The IRQ still enters at `keyboard_handler()` (`kernel/arch/isr.c:183`).
- Produces:
  ```c
  struct key_event {
      uint16_t keycode;   // Linux KEY_* value, 0 if the scancode is unmapped
      uint8_t  pressed;    // 1 = make, 0 = break
      uint8_t  raw_scan;   // the Set-1 byte (low 7 bits), for EV_MSC/MSC_SCAN
      int      ascii;      // -1 if none; else the character under current modifiers
  };
  // Feed one byte from port 0x60; returns 1 and fills *out when a
  // complete event is decoded, 0 while mid-sequence (0xE0 prefix).
  int keyboard_decode(uint8_t byte, struct key_event *out);
  ```

- [ ] **Step 1: Write the failing test**

`kernel/dev/keyboard.c` (declare in `keyboard.h`):
```c
void keyboard_decode_selftest(void) {
    struct key_event e;
    int rc;

    // plain 'a' make = 0x1E
    rc = keyboard_decode(0x1E, &e);
    if (!rc || e.keycode != KEY_A || !e.pressed || e.ascii != 'a') { FAIL("a make"); return; }
    // 'a' break = 0x9E
    rc = keyboard_decode(0x9E, &e);
    if (!rc || e.keycode != KEY_A || e.pressed || e.ascii != -1) { FAIL("a break"); return; }
    // left shift down (0x2A), then 'a' -> 'A'
    keyboard_decode(0x2A, &e);
    rc = keyboard_decode(0x1E, &e);
    if (!rc || e.ascii != 'A') { FAIL("shift+a"); return; }
    keyboard_decode(0xAA, &e);   // shift up
    // extended: right arrow = 0xE0 0x4D
    rc = keyboard_decode(0xE0, &e);
    if (rc) { FAIL("E0 should not complete"); return; }
    rc = keyboard_decode(0x4D, &e);
    if (!rc || e.keycode != KEY_RIGHT || e.ascii != -1) { FAIL("right arrow"); return; }
    // ctrl+c = control char 3
    keyboard_decode(0x1D, &e);   // ctrl down
    rc = keyboard_decode(0x2E, &e);
    if (!rc || e.ascii != 3) { FAIL("ctrl+c"); return; }
    keyboard_decode(0x9D, &e);   // ctrl up

    serial_write_string("[keyboard] decode selftest passed\n");
}
```
Add `"[keyboard] decode selftest passed"` to `REQUIRED_MARKERS`.

- [ ] **Step 2: Run it to see it fail**

Run: `make test 2>&1 | grep keyboard` → FAIL / missing marker (no
`keyboard_decode` yet).

- [ ] **Step 3: Implement the decoder**

`keymap_us.h`: two 128-entry tables — `scancode_keycode[]` (Set-1 base
→ `KEY_*`), `scancode_e0_keycode[]` (after `0xE0`), and `keychar[]` /
`keychar_shift[]` (`KEY_*` → char, indexed by keycode). Use the
canonical Set-1 map; `0xE0`-prefixed codes cover the arrows, Home/End,
PgUp/PgDn, Insert/Delete, right Ctrl/Alt, the keypad `/` and Enter.

`keyboard_decode` holds file-static state: an `e0_pending` flag and a
`uint32_t mods` bitmask (`MOD_LSHIFT`, `MOD_RSHIFT`, `MOD_LCTRL`,
`MOD_RCTRL`, `MOD_LALT`, `MOD_RALT`, `MOD_CAPS`, `MOD_NUM`). On a
modifier make/break, update `mods` and still emit the event (evdev
needs the modifier key events too). For a character key, pick
`keychar` vs `keychar_shift` by `(shift XOR caps)` for letters,
`shift` for others; if a ctrl modifier is set and the base char is
`@`..`_` or a letter, emit `char & 0x1F`.

- [ ] **Step 4: Rewire `keyboard_handler`**

```c
void keyboard_handler(void) {
    uint8_t sc = inb(KEYBOARD_DATA_PORT);
    struct key_event e;
    if (keyboard_decode(sc, &e) && e.keycode != 0) {
        input_key_event(&e);      // Task 10 — the fan-out
    }
}
```
It no longer references `tty_input_char` or `dev/tty.h`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'keyboard|FAILED'`
Expected: `[keyboard] decode selftest passed`. (`input_key_event` may
be a temporary stub that calls `tty_input_char(e.ascii)` until Task 10;
if so the existing `[tty] selftest` still passes.)

- [ ] **Step 6: Commit**

```bash
git add kernel/dev/keyboard.c kernel/dev/keyboard.h kernel/dev/keymap_us.h kernel/kernel.c Makefile
git commit -m "Phase 14: full Set-1 keyboard decoder with modifiers and keycodes

<trailer>"
```

---

## Task 10: The input core — fan-out, grab, evdev clients

**Files:**
- Create: `kernel/dev/input.c`, `kernel/dev/input.h`
- Modify: `kernel/dev/keyboard.c` (call `input_key_event` for real)
- Modify: `kernel/dev/tty.h` (no change — still `tty_input_char`)
- Modify: `kernel/sync/lock.h` (add `LOCK_RANK_INPUT`)
- Modify: `kernel/kernel.c` (`input_init()` before `keyboard` IRQ is unmasked)
- Test: `kernel/dev/input.c` `input_selftest` (fan-out + grab routing), called from `kernel.c`

**Interfaces:**
- Consumes: `struct key_event` (`keyboard.h`); `tty_input_char(char)` (`tty.h`); `waitq_*` (`sync/waitq.h`); `kmalloc`/`kfree` (`mm/heap.h`); `timer_ticks()` and the RTC epoch for event timestamps; `struct input_event` (from `input.h`, see below).
- Produces:
  ```c
  // input.h
  struct input_event {                 // Linux x86-64 layout, 24 bytes
      int64_t  tv_sec;
      int64_t  tv_usec;
      uint16_t type;
      uint16_t code;
      int32_t  value;
  };
  _Static_assert(sizeof(struct input_event) == 24, "input_event ABI");

  #define EV_SYN 0x00
  #define EV_KEY 0x01
  #define EV_MSC 0x04
  #define SYN_REPORT 0
  #define MSC_SCAN   4

  struct evdev_client;   // opaque

  void input_init(void);
  void input_key_event(const struct key_event *e);   // called from the IRQ

  // client lifecycle (called by the evdev file_ops, Task 11)
  struct evdev_client *evdev_client_open(void);
  void  evdev_client_close(struct evdev_client *c);
  int64_t evdev_client_read(struct evdev_client *c, void *buf, uint64_t len, int nonblock);
  int   evdev_client_poll(struct evdev_client *c);          // POLLIN or 0
  int   evdev_client_grab(struct evdev_client *c, int on);  // 0 / -EBUSY
  void  evdev_client_key_bitmap(uint8_t *out, uint64_t len);
  void  evdev_client_state_bitmap(uint8_t *out, uint64_t len);

  // test hook (Task 13)
  void input_inject_key(uint16_t keycode, int pressed);

  void input_selftest(void);
  ```

- [ ] **Step 1: Write the failing test**

```c
void input_selftest(void) {
    struct evdev_client *c = evdev_client_open();

    // Ungrabbed: an injected key reaches the client AND the tty.
    tty_selftest_reset();                 // small helper: clear the line buffer
    input_inject_key(KEY_A, 1);
    input_inject_key(KEY_A, 0);
    struct input_event ev[8];
    int64_t n = evdev_client_read(c, ev, sizeof(ev), 1 /*nonblock*/);
    // expect: MSC_SCAN, KEY down, SYN, MSC_SCAN, KEY up, SYN  -> 6 events
    if (n != 6 * (int64_t)sizeof(struct input_event)) { FAIL("ungrabbed event count"); return; }
    if (ev[1].type != EV_KEY || ev[1].code != KEY_A || ev[1].value != 1) { FAIL("event content"); return; }
    if (!tty_selftest_saw('a')) { FAIL("ungrabbed key missed the tty"); return; }

    // Grabbed: the tty sees nothing.
    if (evdev_client_grab(c, 1) != 0) { FAIL("grab"); return; }
    tty_selftest_reset();
    input_inject_key(KEY_B, 1); input_inject_key(KEY_B, 0);
    if (tty_selftest_saw('b')) { FAIL("grabbed key leaked to the tty"); return; }
    (void)evdev_client_read(c, ev, sizeof(ev), 1);

    // A second grab attempt fails.
    struct evdev_client *c2 = evdev_client_open();
    if (evdev_client_grab(c2, 1) != -EBUSY) { FAIL("double grab not refused"); return; }

    // Release: the tty sees keys again.
    evdev_client_grab(c, 0);
    tty_selftest_reset();
    input_inject_key(KEY_C, 1); input_inject_key(KEY_C, 0);
    if (!tty_selftest_saw('c')) { FAIL("tty did not recover after ungrab"); return; }

    // Closing a grab holder releases the grab.
    evdev_client_grab(c, 1);
    evdev_client_close(c);
    tty_selftest_reset();
    input_inject_key(KEY_A, 1); input_inject_key(KEY_A, 0);
    if (!tty_selftest_saw('a')) { FAIL("close did not release the grab"); return; }
    evdev_client_close(c2);

    serial_write_string("[input] selftest passed\n");
}
```
Add `tty_selftest_reset()` / `tty_selftest_saw(char)` helpers to
`tty.c` (guarded so they are compiled always but only used by tests —
they are tiny). Add `"[input] selftest passed"` to `REQUIRED_MARKERS`.

- [ ] **Step 2: Run it to see it fail**

Run: `make test 2>&1 | grep input` → missing marker / link error.

- [ ] **Step 3: Implement the input core**

- A global `static struct { struct spinlock lock; struct evdev_client *clients; struct evdev_client *grab; } input;` — `spin_init(&input.lock, LOCK_RANK_INPUT, "input")`. Rank `LOCK_RANK_INPUT` sits **above** `LOCK_RANK_WAITQ` is wrong — it must be a **leaf-ish** rank taken from the IRQ; place it just above `LOCK_RANK_RUNQUEUE`/below `LOCK_RANK_SERIAL`, and never call `tty_input_char` or `waitq_wake` with `input.lock` held if those take higher ranks. Safer: under `input.lock` only append to ring buffers and copy out the grab pointer; do the `tty_input_char` call and `waitq_wake_one` calls **after** dropping `input.lock`.
- `struct evdev_client` = `{ next; struct input_event ring[256]; uint32_t head, tail; int dropped; struct waitq readers; uint8_t keystate[KEY_CNT/8]; }`.
- `input_key_event(e)`:
  1. Take `input.lock`. For each client, push `EV_MSC/MSC_SCAN/raw_scan`, `EV_KEY/keycode/(pressed?1:0)` (use `2` for autorepeat if ever added), `EV_SYN/SYN_REPORT/0`; on ring-full advance `tail` (drop oldest) and set `dropped`. Update each client's `keystate` bit. Snapshot `grab != NULL` and, if no grab, `e->ascii`. Release `input.lock`.
  2. Wake each client's `readers` waitq.
  3. If no grab and `ascii >= 0`, call `tty_input_char((char)ascii)`.
- Timestamp each event with `rtc_boot_epoch() + ticks/HZ` seconds and `(ticks % HZ) * (1000000/HZ)` usec.
- `evdev_client_read`: under `input.lock`, if empty and `nonblock` → `-EAGAIN`; if empty and blocking → `waitq_sleep(&c->readers, &input.lock)` loop; copy whole events only, `-EINVAL` if `len < sizeof(struct input_event)`.
- `evdev_client_grab`: `on` → if `input.grab && input.grab != c` return `-EBUSY`, else `input.grab = c`; `!on` → if `input.grab == c` clear it.
- `evdev_client_close`: if it holds the grab, clear it; unlink; `kfree`.

- [ ] **Step 4: Wire the keyboard IRQ**

`keyboard.c` `keyboard_handler` calls `input_key_event(&e)` (replacing
the Task 9 temporary stub).

- [ ] **Step 5: Run tests to verify they pass**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'input|tty|keyboard|FAILED'`
Expected: `[input] selftest passed`, `[tty] selftest passed`,
`[keyboard] decode selftest passed`, no `[lock] PANIC`.

- [ ] **Step 6: Commit**

```bash
git add kernel/dev/input.c kernel/dev/input.h kernel/dev/keyboard.c kernel/sync/lock.h kernel/dev/tty.c kernel/dev/tty.h kernel/kernel.c Makefile
git commit -m "Phase 14: input core — key-event fan-out with an exclusive grab

<trailer>"
```

---

## Task 11: `/dev/input/event0` — the evdev device

**Files:**
- Create: `kernel/dev/evdev.c`, `kernel/dev/evdev.h`
- Modify: `kernel/fs/devfs.c` (add the `input/event0` entry; devfs must support a `/`-containing name or a nested dir)
- Modify: `kernel/fs/devfs.h`
- Test: extend `input_selftest` with ioctl checks, or a dedicated `evdev_selftest`

**Interfaces:**
- Consumes: `evdev_client_*` (`input.h`); `struct file_descriptor`; `file_ops` (`file.h`); the `EVIOC*` request numbers (define in `evdev.h`, values = Linux's).
- Produces:
  ```c
  // evdev.h
  extern const struct file_ops evdev_file_ops;
  int evdev_devfs_open(struct file_descriptor *f);   // f->priv = evdev_client_open()
  ```

- [ ] **Step 1: Decide the devfs namespace shape**

`devfs_lookup` currently matches a flat name. Two options — pick the
smaller: (a) allow the literal name `"input/event0"` as one entry and
teach `devfs_lookup`/`readdir` to present `input` as a synthetic
subdir; (b) give devfs one level of real nesting. Option (a) is fewer
lines: add an `"input"` `VNODE_DIR` entry and an `"input/event0"`
device entry, and special-case the one slash in lookup.

- [ ] **Step 2: Write the failing test**

Add to `input_selftest` (after opening a client directly) a path-based
check that goes through devfs + `file_ops`:
```c
    int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) { FAIL("open event0"); return; }
    int ver = 0;
    if (ioctl(fd, EVIOCGVERSION, &ver) != 0 || ver == 0) { FAIL("EVIOCGVERSION"); return; }
    char name[64] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof name), name) <= 0) { FAIL("EVIOCGNAME"); return; }
    struct input_event one;
    if (read(fd, &one, sizeof one) != -EAGAIN) { FAIL("empty nonblock read"); return; }
    char half[8];
    if (read(fd, half, 4) != -EINVAL) { FAIL("sub-event read"); return; }
    close(fd);
```
This runs in the kernel selftest via the in-kernel VFS API, or move it
to `evtest` (Task 15). Prefer `evtest` for the userland-facing assertions
and keep the kernel `input_selftest` focused on fan-out.

- [ ] **Step 3: Implement `evdev_file_ops`**

- `read` → `evdev_client_read(f->priv, buf, len, f->nonblock)`.
- `write` → `-EINVAL`.
- `lseek` → `-ESPIPE`.
- `poll` → `evdev_client_poll(f->priv)`.
- `dup` → increment a refcount on the client (a `dup`'d fd shares the
  queue — matches an fd inherited across `fork`).
- `close` → on last ref, `evdev_client_close(f->priv)`; release the
  grab if held.
- `ioctl` request handling (values from Linux `<linux/input.h>`):

  | Request | Action |
  |---|---|
  | `EVIOCGVERSION` | write `EV_VERSION` (0x010001) |
  | `EVIOCGID` | write `struct input_id { BUS_I8042, 0x0001, 0x0001, 0x0100 }` |
  | `EVIOCGNAME(len)` | copy `"NeoOS AT keyboard"` (≤ len), return byte count |
  | `EVIOCGPHYS(len)` / `EVIOCGUNIQ(len)` | `-ENOENT` |
  | `EVIOCGBIT(0, len)` | EV bitmap: bits `EV_SYN`, `EV_KEY`, `EV_MSC` |
  | `EVIOCGBIT(EV_KEY, len)` | `evdev_client_key_bitmap` |
  | `EVIOCGBIT(EV_MSC, len)` | bit `MSC_SCAN` |
  | `EVIOCGKEY(len)` | `evdev_client_state_bitmap` |
  | `EVIOCGRAB` | `evdev_client_grab(f->priv, arg != 0)` |
  | `EVIOCSCLOCKID` | `-EINVAL` |
  | anything else | `-EINVAL` |

  `EVIOCGBIT`/`EVIOCGNAME` encode the length in the request; decode with
  the Linux `_IOC_*` macros (add them to `evdev.h`).

- [ ] **Step 4: Register in devfs and export**

devfs `input/event0` → `fops = &evdev_file_ops`, `open =
evdev_devfs_open`. Ensure the `sys_open` devfs path (Task 8) calls the
constructor.

- [ ] **Step 5: Run tests to verify they pass**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'input|direnttest|FAILED'`
Expected: `[input] selftest passed`; `direnttest` still `ALL PASSED`
(it lists `/dev` — make sure the synthetic `input` dir does not break
its expectations; update `direnttest` if it hard-counts entries).

- [ ] **Step 6: Commit**

```bash
git add kernel/dev/evdev.c kernel/dev/evdev.h kernel/fs/devfs.c kernel/fs/devfs.h kernel/dev/input.h userland/direnttest.c
git commit -m "Phase 14: /dev/input/event0 with the evdev ioctl surface

<trailer>"
```

---

## Task 12: Userland `<linux/input.h>` UAPI headers

**Files:**
- Create: `lib/include/linux/input.h`
- Create: `lib/include/linux/input-event-codes.h`
- Modify: `lib/include/` install/copy logic if headers are staged somewhere for userland builds (check `Makefile` `USER_CFLAGS` `-I`)
- Modify: `docs/stdlib.md` (evdev section)

**Interfaces:**
- Consumes: nothing.
- Produces: `struct input_event`, `struct input_id`, `EV_*`, `KEY_*`,
  `MSC_*`, `SYN_*`, `BUS_I8042`, `EVIOC*` — all with Linux's values,
  matching `kernel/dev/input.h` and `kernel/dev/evdev.h` byte-for-byte.

- [ ] **Step 1: Write the headers**

`linux/input-event-codes.h`: the `EV_*`, `KEY_*` (the US-keyboard
subset the decoder can produce — copy the canonical values), `MSC_*`,
`SYN_*`, `BUS_*` constants.

`linux/input.h`:
```c
#ifndef _LINUX_INPUT_H
#define _LINUX_INPUT_H
#include <stdint.h>
#include <linux/input-event-codes.h>

struct input_event {
    int64_t  input_event_sec;
    int64_t  input_event_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
struct input_id { uint16_t bustype, vendor, product, version; };

#define EVIOCGVERSION   _IOR('E', 0x01, int)
#define EVIOCGID        _IOR('E', 0x02, struct input_id)
#define EVIOCGNAME(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGKEY(len)  _IOC(_IOC_READ, 'E', 0x18, len)
#define EVIOCGBIT(ev,len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGRAB       _IOW('E', 0x90, int)
/* ... _IOC/_IOR/_IOW/_IOC_READ ... matching Linux <asm-generic/ioctl.h> ... */
#endif
```
Add `asm-generic/ioctl.h`-equivalent macros (inline in `linux/input.h`
is acceptable for the subset used).

- [ ] **Step 2: Cross-check against the kernel**

Add a `_Static_assert` in `kernel/dev/evdev.c` that each `EVIOC*` value
it switches on equals the macro expansion from a copy of these
definitions, OR add a runtime `evdev_selftest` that asserts
`sizeof(struct input_event) == 24` and `offsetof(type) == 16`. The
userland `evtest` (Task 15) asserting the same is the real ABI guard.

- [ ] **Step 3: Ensure userland builds see the headers**

Confirm `USER_CFLAGS` includes `-Ilib/include` (or wherever). Build
`evtest` in Task 15 will fail here if not.

- [ ] **Step 4: Document**

`docs/stdlib.md` — a new "evdev / `/dev/input/event0`" section: the
supported `EVIOC*` subset (table), `struct input_event` is Linux's
24-byte layout, US-layout-only, one device, ring overflow drops
oldest, no `EVIOCSCLOCKID` (timestamps always `CLOCK_REALTIME`), the
grab semantics. `docs/abi-compatibility.md` — add evdev + `linux/input.h`
to the struct/constants tables.

- [ ] **Step 5: Commit**

```bash
git add lib/include/linux/ docs/stdlib.md docs/abi-compatibility.md
git commit -m "Phase 14: ship linux/input.h for evdev-using userland

<trailer>"
```

---

## Task 13: The `-DNEOOS_TEST_HOOKS` injection syscall

**Files:**
- Modify: `kernel/syscall/syscall_nr.h` (add `SYS_TEST_HOOK`, bump `SYS_MAX`)
- Modify: `kernel/syscall/syscall.c` (table entry — `#ifdef NEOOS_TEST_HOOKS`)
- Modify: `kernel/syscall/sys_misc.c` (the handler)
- Modify: `kernel/syscall/syscall_internal.h` (declare `sys_test_hook`)
- Modify: `kernel/smp/smp_selftest.c` or wherever the user-migration counter lives (expose a getter)
- Modify: `Makefile` (add `-DNEOOS_TEST_HOOKS` to the kernel `CFLAGS` used by `make test`, not by `make build`)
- Modify: `lib/syscall.c`, a `lib/include/neoos_test.h` (the wrapper)
- Modify: `docs/stdlib.md`

**Interfaces:**
- Consumes: `input_inject_key(uint16_t, int)` (`input.h`); the migration counter.
- Produces:
  ```c
  // request codes for SYS_TEST_HOOK(a1=op, a2, a3)
  #define TESTHOOK_INJECT_KEY   1   // a2 = keycode, a3 = pressed
  #define TESTHOOK_MIG_COUNT    2   // returns the user-thread migration count

  // lib/include/neoos_test.h
  int neoos_test_inject_key(unsigned keycode, int pressed);  // -ENOSYS in prod
  long neoos_test_migration_count(void);
  ```

- [ ] **Step 1: Write the failing test**

Fold into `evtest` (Task 15) and `smptest`. For now, a tiny kernel
check that in a `NEOOS_TEST_HOOKS` build the syscall table slot is
non-NULL and in a plain build it is NULL — via `syscall_table_selftest`
extended with a conditional.

- [ ] **Step 2: Add the syscall number and handler**

`syscall_nr.h`: `#define SYS_TEST_HOOK 66` and `#define SYS_MAX 67`.
`sys_misc.c`:
```c
#ifdef NEOOS_TEST_HOOKS
int64_t sys_test_hook(struct syscall_args *a) {
    switch ((int)a->a1) {
    case TESTHOOK_INJECT_KEY:
        input_inject_key((uint16_t)a->a2, (int)a->a3);
        return 0;
    case TESTHOOK_MIG_COUNT:
        return (int64_t)smp_user_migration_count();
    default:
        return -EINVAL;
    }
}
#endif
```
`syscall.c` table: `[SYS_TEST_HOOK] = {`
`#ifdef NEOOS_TEST_HOOKS sys_test_hook #else 0 #endif , "test_hook" }`.
A `0` handler already returns `-ENOSYS` via the dispatcher.

- [ ] **Step 3: Build flag**

In the `Makefile`, the `test` target (or the `CFLAGS` it uses) gets
`-DNEOOS_TEST_HOOKS`; `build`/`iso` do not. Keep the kernel object dir
separate or `make test` after `make build` will link stale objects —
add `-DNEOOS_TEST_HOOKS` unconditionally to the normal `CFLAGS` **only
if** a separate build dir is too invasive; document the choice. (Recommended:
`make test` does a clean kernel rebuild with the flag.)

- [ ] **Step 4: The userland wrapper**

`lib/include/neoos_test.h` + `lib/syscall.c`: thin wrappers over
`syscall(SYS_TEST_HOOK, …)`. They return whatever the kernel returns,
so `-ENOSYS` in a production kernel.

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test 2>&1 | grep -E 'syscall_table|FAILED'` → table selftest
passes; the slot is populated.

- [ ] **Step 6: Document**

`docs/stdlib.md`: `SYS_TEST_HOOK` exists **only in `NEOOS_TEST_HOOKS`
builds**, is **not part of the ABI**, and exists so headless tests can
inject keystrokes and read the migration counter. `docs/abi-compatibility.md`
§3: note number 66 is test-only.

- [ ] **Step 7: Commit**

```bash
git add kernel/syscall/ kernel/smp/smp_selftest.c Makefile lib/ docs/
git commit -m "Phase 14: a test-only key-injection syscall for headless tests

<trailer>"
```

---

## Task 14: Fold the migration counter into `smptest` (optional cleanup)

**Files:**
- Modify: `userland/smptest.c`

**Interfaces:**
- Consumes: `neoos_test_migration_count()` (`neoos_test.h`).
- Produces: nothing.

Per spec §4.0, `smptest` stays "reported, not asserted" for its own
threads. This task only *adds* a line: if
`neoos_test_migration_count()` is available (`>= 0`) and returns `0`
after the run, print a `FAILED` — because the kernel steal selftest
should have driven it above zero. In a non-test build the call returns
`-ENOSYS` and `smptest` keeps its current behaviour.

- [ ] **Step 1: Add the assertion**

```c
    long mig = neoos_test_migration_count();
    if (mig == 0) { printf("[smptest] FAILED: kernel reports zero user migrations\n"); return 1; }
    if (mig > 0)  { printf("[smptest] kernel user migrations: %ld\n", mig); }
```

- [ ] **Step 2: Run**

Run: `make test 2>&1 | grep smptest` → `ALL PASSED`, with a
`kernel user migrations: N` line, N > 0.

- [ ] **Step 3: Commit**

```bash
git add userland/smptest.c
git commit -m "Phase 14: smptest fails if the kernel saw zero user migrations

<trailer>"
```

---

## Task 15: `userland/evtest.c` — the app that does NOT use the TTY

**Files:**
- Create: `userland/evtest.c`
- Modify: `Makefile` (build rule `EVTEST.ELF`, `mcopy` to `::BIN/EVTEST.ELF`, `REQUIRED_MARKERS += "[evtest] ALL PASSED"`)
- Modify: `kernel/kernel.c` (`spawn("/BIN/EVTEST.ELF")` — ordered **before** `spawn("/BIN/TTYTEST.ELF")`)

**Interfaces:**
- Consumes: `open`/`read`/`close`/`ioctl` (musl or `libneoos`); `<linux/input.h>` (Task 12); `neoos_test_inject_key` (`neoos_test.h`, Task 13).
- Produces: the `[evtest] ALL PASSED` marker.

- [ ] **Step 1: Write the test binary**

```c
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <linux/input.h>
#include <neoos_test.h>

static int failures;
#define OK(cond, msg) do { if (!(cond)) { printf("[evtest] FAILED: %s\n", msg); failures++; } } while (0)

static int read_event(int fd, struct input_event *e) {
    return read(fd, e, sizeof *e) == (long)sizeof *e;
}

int main(void) {
    int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) { printf("[evtest] FAILED: open event0 rc=%d\n", fd); return 1; }

    if (neoos_test_inject_key(KEY_A, 1) == -ENOSYS) {
        printf("[evtest] SKIPPED (no test hooks)\n");
        return 0;
    }
    neoos_test_inject_key(KEY_A, 0);

    struct input_event e;
    OK(read_event(fd, &e) && e.type == EV_MSC && e.code == MSC_SCAN, "MSC_SCAN down");
    OK(read_event(fd, &e) && e.type == EV_KEY && e.code == KEY_A && e.value == 1, "KEY_A down");
    OK(read_event(fd, &e) && e.type == EV_SYN, "SYN after down");
    OK(read_event(fd, &e) && e.type == EV_MSC, "MSC_SCAN up");
    OK(read_event(fd, &e) && e.type == EV_KEY && e.value == 0, "KEY_A up");
    OK(read_event(fd, &e) && e.type == EV_SYN, "SYN after up");

    /* empty nonblock read */
    OK(read(fd, &e, sizeof e) == -EAGAIN || read(fd, &e, sizeof e) < 0, "empty read is EAGAIN");
    /* sub-event read */
    char half[4];
    OK(read(fd, half, sizeof half) < 0, "sub-event read rejected");

    /* ioctl surface */
    int ver = 0;
    OK(ioctl(fd, EVIOCGVERSION, &ver) == 0 && ver != 0, "EVIOCGVERSION");
    char name[64] = {0};
    OK(ioctl(fd, EVIOCGNAME(sizeof name), name) > 0 && name[0] != 0, "EVIOCGNAME");
    struct input_id id;
    OK(ioctl(fd, EVIOCGID, &id) == 0, "EVIOCGID");

    /* grab: the tty must not see the key. evtest cannot read the tty
       directly without opening it, so it relies on the kernel
       input_selftest for the negative case and here just checks the
       grab call succeeds and a second grab from a dup fails is N/A
       single-fd — assert the ioctl result. */
    OK(ioctl(fd, EVIOCGRAB, (void *)1) == 0, "EVIOCGRAB on");
    neoos_test_inject_key(KEY_B, 1); neoos_test_inject_key(KEY_B, 0);
    OK(read_event(fd, &e), "grabbed key still delivered to event0");
    OK(ioctl(fd, EVIOCGRAB, (void *)0) == 0, "EVIOCGRAB off");

    close(fd);
    if (failures) { printf("[evtest] %d FAILURES\n", failures); return 1; }
    printf("[evtest] ALL PASSED\n");
    return 0;
}
```
(The TTY-starvation negative case is covered authoritatively by the
kernel `input_selftest`; `evtest` proves the userland ABI.)

- [ ] **Step 2: Wire the build**

Add the `EVTEST.ELF` rule (copy the `SMPTEST.ELF` pattern; it needs
musl if using `<linux/input.h>` with musl headers, or `libneoos` +
local headers — match whichever `ttytest` uses). Add the `mcopy` line
in the `disk-image` target. Add the marker. Add the `spawn` in
`kernel.c` before `TTYTEST`.

- [ ] **Step 3: Run it to see it fail, then pass**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'evtest|FAILED'`
First run (before the spawn/marker wiring): missing marker. After
wiring: `[evtest] ALL PASSED`.

- [ ] **Step 4: Commit**

```bash
git add userland/evtest.c Makefile kernel/kernel.c
git commit -m "Phase 14: evtest — raw key events without the tty

<trailer>"
```

---

## Task 16: Extend `userland/ttytest.c` — injected input on the TTY path

**Files:**
- Modify: `userland/ttytest.c`

**Interfaces:**
- Consumes: `neoos_test_inject_key` (`neoos_test.h`); `tcsetattr`/`tcgetattr`, `read` on stdin; signal handling (`libneoos` / musl).
- Produces: `[ttytest] ALL PASSED` (unchanged marker, more cases).

- [ ] **Step 1: Add a canonical-line case**

```c
static void check_canonical_line(void) {
    if (neoos_test_inject_key(KEY_H, 1) == -ENOSYS) { printf("[ttytest] input SKIPPED\n"); return; }
    neoos_test_inject_key(KEY_H, 0);
    inject_str("i");          // helper: down+up for each char
    neoos_test_inject_key(KEY_X, 1); neoos_test_inject_key(KEY_X, 0);
    neoos_test_inject_key(KEY_BACKSPACE, 1); neoos_test_inject_key(KEY_BACKSPACE, 0);
    neoos_test_inject_key(KEY_ENTER, 1); neoos_test_inject_key(KEY_ENTER, 0);

    char buf[16] = {0};
    long n = read(0, buf, sizeof buf);
    if (n != 3 || buf[0] != 'h' || buf[1] != 'i' || buf[2] != '\n') {
        fail("canonical line with erase", (int)n); return;
    }
    printf("[ttytest] canonical line passed\n");
}
```

- [ ] **Step 2: Add a `^C` → `SIGINT` case**

Install a `SIGINT` handler that sets a `volatile sig_atomic_t got_int`,
inject `KEY_LEFTCTRL` down + `KEY_C` + `KEY_LEFTCTRL` up (or use the
`VINTR` char via a raw write path), spin briefly, assert `got_int`.

- [ ] **Step 3: Add a raw-mode single-byte read**

`tcgetattr`, clear `ICANON`/`ECHO`, set `VMIN=1 VTIME=0`, `tcsetattr`;
inject one key; `read(0, &c, 1)` returns 1 with that byte; restore.

- [ ] **Step 4: Run**

Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'ttytest|FAILED'`
Expected: `[ttytest] canonical line passed`, the SIGINT and raw-mode
lines, `[ttytest] ALL PASSED`. Because `evtest` runs first and releases
its grab, `ttytest`'s injected input must reach the line discipline — a
leaked grab shows up here as a hang.

- [ ] **Step 5: Commit**

```bash
git add userland/ttytest.c
git commit -m "Phase 14: ttytest drives injected input through the line discipline

<trailer>"
```

---

## Task 17: Documentation refresh at milestone close

**Files:**
- Modify: `docs/stdlib.md`
- Modify: `docs/abi-compatibility.md`
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-08-31-phase14-input-and-solidity-design.md` (mark status: implemented)

**Interfaces:**
- Consumes: everything above.
- Produces: docs consistent with the shipped kernel.

- [ ] **Step 1: `docs/stdlib.md`**

Confirm every Phase 14 change is recorded: `O_CREAT`=0x40 and the `O_*`
table; `nanosleep` blocks + returns `-EINTR`; the evdev section (from
Task 12) is complete; `SYS_TEST_HOOK` note; `AT_RANDOM` is a seeded
CSPRNG. Remove any now-false statement (e.g. "`ioctl` always
`-ENOTTY`" — it isn't, on the console).

- [ ] **Step 2: `docs/abi-compatibility.md`**

Refresh the header to "close of Phase 14". Move `O_CREAT` out of
"diverges". Add `struct input_event`, `struct input_id`, `EV_*`/`KEY_*`
to the struct/constant tables. Update §9 "what a real app hits": raw
input is no longer a gap; `ppoll`/`dup2`/`execve argv`/futex requeue
remain (Phase 15). Note the keyboard is US-layout-only.

- [ ] **Step 3: `README.md`**

"What works today": raw input via `/dev/input/event0` (evdev), the
keyboard driver's new depth, the solidity fixes. "Where it's going":
Phase 15 = `ppoll` + `select`/`poll`, `dup2`, `execve(argv)`, futex
requeue.

- [ ] **Step 4: Spec status**

Set the spec's `**Status:**` line to "implemented 2026-…". Add a short
"as-built notes" section for any deviation from the design discovered
during implementation.

- [ ] **Step 5: Full green run**

Run: `rm -f build/disk.img build/disk2.img && make test`
Then run it two more times. All `REQUIRED_MARKERS` present, no
`FAILED`, no `PANIC`, every run.

- [ ] **Step 6: Commit**

```bash
git add docs/ README.md
git commit -m "Phase 14: refresh stdlib, ABI report and README for raw input

<trailer>"
```

---

## Self-Review

**1. Spec coverage:**

| Spec section | Task(s) |
|---|---|
| §3 Phase 13 (verify tlb.c, commit tree) | Task 1 |
| §4.0 reconciliation | Task 1 (stat ships), Task 4 (regression), Task 14 note |
| §4.1 O_CREAT | Task 2 |
| §4.2 nanosleep | Task 3 |
| §4.3 stat timestamps (already done) | Task 1 slice 8, Task 4 |
| §4.4 SIGSTOP/SIGCONT reproducer | Task 5 |
| §4.5 smptest (deliberately not asserted) | Task 14 (kernel-counter check only) |
| §4.6 proc_list | Task 6 |
| §4.7 AT_RANDOM | Task 7 |
| §5 file_ops ioctl+poll, devfs, sys_ioctl | Task 8 |
| §6 keyboard decoder | Task 9 |
| §7.1 input core / fan-out / grab | Task 10 |
| §7.2 evdev_client | Task 10 |
| §7.3 evdev ABI / event0 | Task 11 |
| §7.4 linux/input.h | Task 12 |
| §7.5 injection hook | Task 13 |
| §8 evtest | Task 15 |
| §8 ttytest extension | Task 16 |
| §8 input_selftest | Task 10 |
| §9 error/edge cases | Tasks 10 (grab-holder exit, overflow), 6 (rank), 13 (prod build), 2 (audit) |
| §11 docs | Tasks 2, 3, 6, 7, 12, 17 |

No spec section is without a task.

**2. Placeholder scan:** Kernel driver bodies are given as interface +
key logic + the test, not always line-complete — acceptable for
freestanding C where the surrounding file dictates style, and every
task names the exact file/pattern to follow. Test code and all
constants/struct layouts are complete. No "TBD"/"handle errors"/"similar
to Task N".

**3. Type consistency:** `struct key_event` (Task 9) is consumed
unchanged by `input_key_event` (Task 10). `struct input_event` is
defined identically in `kernel/dev/input.h` (Task 10) and
`lib/include/linux/input.h` (Task 12) — Task 12 Step 2 adds the
cross-check. `evdev_client_*` signatures in Task 10's Produces block
match their use in Task 11. `neoos_test_inject_key` / `TESTHOOK_*`
consistent across Tasks 13, 15, 16. `proc_global_lock` (Task 6) —
rename applied everywhere the Files list enumerates.

**Fix applied during review:** Task 8's `POLL*` values are declared
kernel-internal but *must* equal Linux's (Phase 15 `ppoll` passes them
to userland) — noted inline in the interface block.
