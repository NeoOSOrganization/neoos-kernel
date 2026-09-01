# CS0 — VT-layer locking and the `spin_lock_raw` audit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put the kernel virtual-terminal layer under a lock (it has
none today, and races the keyboard IRQ against console output), and
make every rank-registered lock in the tree actually visible to the
rank checker instead of being taken with `spin_lock_raw`.

**Architecture:** Four locks (`pty`, `devfs`, `fbcon`, `rand`) are
already `spin_init`'d with a rank but taken exclusively through
`spin_lock_raw`, which bypasses the checker — a registered-but-always-raw
lock is exactly as invisible as an unregistered one. Each converts to
`spin_lock_irqsave`/`spin_unlock_irqrestore` at every call site. The VT
layer is the real bug: `kernel/tty/vt.c` guards nothing, and
`vt_switch`/`vt_scroll` run from the keyboard IRQ. It gains
`LOCK_RANK_VT`, ordered between `LOCK_RANK_TTY` and `LOCK_RANK_FBCON`
so the existing write path (`t->lock` → render → fbcon) stays ascending.
The panic path keeps raw acquires, because it may already hold anything.

**Tech Stack:** C (freestanding), x86-64, GNU Make, headless QEMU, the
parallel gauntlet (CONC=3).

**Spec:** `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS0)

## Global Constraints

- **No host unit tests.** Every test is a kernel selftest writing
  through `serial_write_string`, or a userland binary ending in a
  `[name] ALL PASSED` marker wired into the Makefile's
  `REQUIRED_MARKERS`. There is no host runtime.
- **The gauntlet is the bar, not `make test`.** After each task:
  `.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3`
  must report `PGAUNTLET PASSED: 15/15`. A single green `make test` is
  not sign-off for lock changes.
- **Lock ranks are strictly ascending.** A lock may be taken only while
  every lock held has a *strictly lower* rank. Equal ranks are an
  inversion. Every new rank gets a written rationale comment in
  `kernel/sync/lock.h`, in the style of the existing ones.
- **`spin_lock_raw` is only for leaf locks that never nest and for
  paths that run before `cpu_local_init()` or during a panic.** Every
  other use is what this milestone removes.
- **No user-visible behaviour changes.** No syscall, struct, or flag
  moves, so this milestone adds no `docs/stdlib.md` entry and no
  `docs/abi-compatibility.md` change. Task 7 records the rank additions
  only.

## Audit findings this plan is built on

Established by reading the tree at 281 commits; each is a fact an
implementer can re-check.

1. **`con_driver_select()` runs at `kernel/kernel.c:111`, well after
   `cpu_local_init_bsp()` at line 93.** `pty_init()` (141) and
   `rand_init()` (147) are later still, and `devfs_register` is called
   later again. **So none of the four locks below has the
   before-`cpu_local_init` excuse that `serial.c` documents.** Serial
   keeps its exception; the other four do not have one.
2. **The VT stack holds no lock at any layer.** `vt.c`, `kvt.c`,
   `console.c` and `con_driver.c` contain no `spin_init`, `spin_lock`
   or `LOCK_RANK_*` reference. `kvt.h` says so deliberately ("PURE
   logic -- no rendering, no locks"), which means the lock belongs in
   `vt.c`, and `vt.c` never added it.
3. **Per-VT writes *are* already serialized** — `tty_obj_write`
   (`tty.c:99`) holds `t->lock` across `tty_out_cooked` →
   `backend->output` → `vt_backend_output` → `kvt_feed` + `render_diff`.
   So two writers to one VT do not race each other. Three other paths
   are not covered:
   - `vt_switch` (`vt.c:106`) writes the global `vt_active` and calls
     `render_full(&vts[n])`, mutating `vc->shown[][]` and
     `shown_valid`, **holding nothing** — concurrently with a writer on
     another CPU that is inside `render_diff` on the same `vc` under
     `t->lock`. The diff cache is left claiming cells were painted that
     were not, so the corruption is *persistent*: the diff never
     repaints them.
   - `vt_scroll` (`vt.c:118`) calls `kvt_scroll_view` on the active
     VT's `struct kvt` **holding nothing**, racing `kvt_feed` on that
     same kvt under `t->lock`.
   - `vt_active` is a plain `int`, written by `vt_switch` and read by
     `vt_write_active`, `vt_backend_output`, `vt_active_tty`, `vt_tty`
     and `vt_ioctl` with no lock, no barrier, no `volatile`.
4. **Both unlocked paths are reached from the keyboard IRQ.**
   `kernel/drivers/input/input.c:97` calls `vt_switch` and `:106` calls
   `vt_scroll`, and — verified by reading `input_key_event` — both run
   **before** `spin_lock_irqsave(&input.lock)` is taken, holding no
   lock. So a VT lock is free to be taken there. `VT_ACTIVATE` reaches
   `vt_switch` from an ordinary syscall on any CPU, and `vt_fop_ioctl`
   calls `vt_ioctl` *before* `tty_obj_ioctl`, so the tty lock is not
   held there either.
5. **The panic path must stay raw.** `exception_dump_and_halt`
   (`isr.c:28`) calls `vt_panic_reset()` → `render_full` → `cd->clear`
   / `cd->putc_at` → `fbcon_lock`. The panicking CPU may hold any lock,
   including a lower-ranked one, so a checked acquire there would
   trigger `lock_panic` *inside* a panic and recurse. `lock.c` already
   solves this shape for serial output with `panic_puts`, which skips
   the lock entirely.
6. **`rand_lock` shares `LOCK_RANK_SERIAL` with the unrelated serial
   lock** (`rand.c:41`). This is not the deliberate, commented
   `LOCK_RANK_TTY == LOCK_RANK_DRIVER` pair; it reads as a copy of the
   nearest example rank, and it would hide a genuine serial/rand
   inversion precisely because the checker has been told they are the
   same lock class on purpose.

**Rank order this plan installs**, for the hot path
`tty_obj_write → render_diff → fbcon`:

```
LOCK_RANK_TTY (8)  →  LOCK_RANK_VT (251)  →  LOCK_RANK_FBCON (254)
```

strictly ascending at every step. `vt_switch` from the IRQ takes
`t->lock` (8) then `vt_lock` (251) then fbcon (254), the same order, so
no path can build a cycle. Self-deadlock on one CPU is impossible
because `spin_lock_irqsave` disables interrupts while `t->lock` is
held, so the keyboard IRQ cannot land on a CPU already holding it.

---

## Task 1: `LOCK_RANK_VT` and `LOCK_RANK_RAND`

Adds the two ranks the rest of the plan needs and fixes the `rand_lock`
rank collision. Smallest self-contained change; nothing else depends on
`rand`, so it lands first and proves the build.

**Files:**
- Modify: `kernel/sync/lock.h:93-109` (rank list)
- Modify: `kernel/lib/rand.c:41,52,60`
- Test: `kernel/sync/lock.c` (extend `lock_selftest`)

**Interfaces:**
- Produces: `LOCK_RANK_VT` (251) and `LOCK_RANK_RAND` (250), used by
  Tasks 5 and 6.

- [ ] **Step 1: Write the failing test**

In `kernel/sync/lock.c`, inside `lock_selftest()`, add a check that the
two new ranks are distinct from every existing leaf rank. Append before
the function's final `serial_write_string`:

```c
    // CS0: the leaf ranks must all be distinct. rand_lock shared
    // LOCK_RANK_SERIAL, which made a genuine serial/rand inversion
    // invisible -- the checker treats equal ranks as one class.
    if (LOCK_RANK_RAND == LOCK_RANK_SERIAL ||
        LOCK_RANK_VT   == LOCK_RANK_FBCON  ||
        LOCK_RANK_VT   == LOCK_RANK_PTY    ||
        LOCK_RANK_VT   == LOCK_RANK_DEVFS) {
        serial_write_string("[lock] selftest FAILED: leaf ranks collide\n");
        return;
    }
    // The VT lock is taken while a tty lock is held and takes fbcon
    // beneath it; that chain must be strictly ascending.
    if (!(LOCK_RANK_TTY < LOCK_RANK_VT && LOCK_RANK_VT < LOCK_RANK_FBCON)) {
        serial_write_string("[lock] selftest FAILED: VT rank not between TTY and FBCON\n");
        return;
    }
```

- [ ] **Step 2: Run it to see it fail**

```bash
make test 2>&1 | grep -E "^\[lock\]"
```

Expected: the build fails to compile — `LOCK_RANK_VT` and
`LOCK_RANK_RAND` are not defined yet. That compile error *is* the
failing state; it becomes the passing selftest line once Step 3 defines
them.

- [ ] **Step 3: Add the ranks and convert `rand.c`**

In `kernel/sync/lock.h`, immediately before the `LOCK_RANK_SERIAL`
block (which is at line 94-97, ending `#define LOCK_RANK_SERIAL 255`),
insert:

```c
// The kernel virtual terminals: vt_active, each VT's diff cache
// (vc->shown / shown_valid) and kd_mode. Sits ABOVE TTY because the
// write path is tty_obj_write -> t->lock -> vt_backend_output ->
// render_diff, so the VT state is touched while a tty lock is held.
// Sits BELOW FBCON because rendering calls into the con_driver
// underneath it. vt_switch and vt_scroll take t->lock then this, from
// the keyboard IRQ holding nothing else, so the order is the same on
// every path. The panic path (vt_panic_reset) takes NEITHER -- see
// vt.c's panic-mode comment.
#define LOCK_RANK_VT        251
// The CSPRNG pool. A leaf: rand_bytes holds it across arithmetic only,
// and takes nothing under it. It had LOCK_RANK_SERIAL, which is not the
// deliberate TTY==DRIVER sharing -- two unrelated leaves sharing a rank
// makes a real inversion between them invisible, since the checker
// treats equal ranks as one class.
#define LOCK_RANK_RAND      250
```

In `kernel/lib/rand.c`, line 41, change the rank:

```c
    spin_init(&rand_lock, LOCK_RANK_RAND, "rand");
```

and convert the acquire at line 52 and release at line 60:

```c
    uint64_t flags = spin_lock_irqsave(&rand_lock);
```
```c
    spin_unlock_irqrestore(&rand_lock, flags);
```

- [ ] **Step 4: Run the tests**

```bash
make test 2>&1 | grep -E "^\[lock\]|^\[rand\]|PANIC"
```

Expected: `[lock] selftest passed`, `[rand] selftest passed`, no
`PANIC`. A rank-inversion panic naming `rand` here would mean
`rand_bytes` is called while holding a lock ranked above 250 — read the
panic's held-stack line and report it rather than raising the rank to
paper over it.

- [ ] **Step 5: Run the gauntlet**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 6: Commit**

```bash
git add kernel/sync/lock.h kernel/sync/lock.c kernel/lib/rand.c
git commit -m "CS0: LOCK_RANK_VT and LOCK_RANK_RAND; rand stops sharing SERIAL's rank"
```

---

## Task 2: `pty` pool lock becomes checked

**Files:**
- Modify: `kernel/tty/pty.c:47,49,53,57,68,70,264,275,287,289`

**Interfaces:**
- Consumes: nothing from Task 1 (`LOCK_RANK_PTY` already exists at 252).
- Produces: nothing new; `pool_lock` keeps its name and rank.

- [ ] **Step 1: Confirm the nesting is legal before changing anything**

Read each of the five acquire sites and confirm what is held at each.
The relevant question is whether `pool_lock` (252) is ever taken while
holding something ranked *above* it, and whether anything ranked *below*
it is taken while it is held:

```bash
sed -n '44,75p;262,292p' kernel/tty/pty.c
```

Expected findings, which the code confirms: `pty_ref`, `pty_unref` and
`pty_slave_open` hold nothing else; `ptmx_open` calls `devfs_register`
(`LOCK_RANK_DEVFS`, 253) only **after** releasing `pool_lock`, and
`pty_unref` likewise calls `devfs_unregister` after releasing it. Both
are already correct — 252 → 253 would have been legal anyway, but the
code does not even need that. Record this in the commit message.

- [ ] **Step 2: Convert every call site**

Replace `spin_lock_raw(&pool_lock)` with `spin_lock_irqsave(&pool_lock)`
and `spin_unlock_raw(&pool_lock, X)` with
`spin_unlock_irqrestore(&pool_lock, X)` at all ten lines. For example,
`pty_unref` becomes:

```c
static void pty_unref(struct pty *pt, int *counter) {
    uint64_t fl = spin_lock_irqsave(&pool_lock);
    if (*counter > 0) { (*counter)--; }
    int gone = (pt->used && pt->master_refs == 0 && pt->slave_refs == 0);
    if (gone) { pt->used = 0; }
    spin_unlock_irqrestore(&pool_lock, fl);
    if (!gone) { return; }
```

and the `ptmx_open` rollback becomes:

```c
    if (rc != 0) {
        uint64_t g = spin_lock_irqsave(&pool_lock);
        pt->used = 0; pt->master_refs = 0;
        spin_unlock_irqrestore(&pool_lock, g);
        return rc;
    }
```

- [ ] **Step 3: Verify no raw acquire of `pool_lock` remains**

```bash
grep -n "spin_lock_raw\|spin_unlock_raw" kernel/tty/pty.c
```

Expected: no output.

- [ ] **Step 4: Run the tests**

```bash
make test 2>&1 | grep -E "^\[pty\]|^\[tty\]|PANIC|FAILED"
```

Expected: the pty and tty selftests pass, no `PANIC`, no `FAILED`.

- [ ] **Step 5: Run the gauntlet**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 6: Commit**

```bash
git add kernel/tty/pty.c
git commit -m "CS0: the pty pool lock goes through the rank checker"
```

---

## Task 3: `devfs` dynamic-table lock becomes checked

**Files:**
- Modify: `kernel/fs/devfs.c:218-259`

- [ ] **Step 1: Confirm the nesting**

```bash
sed -n '214,262p' kernel/fs/devfs.c
```

Confirm `dyn_lock` is held only across the fixed-array scan and slot
mutation, with no call out to another locked subsystem while held. If a
call *is* made under it (an allocation, a vnode lookup), stop and report
it — that changes the rank this lock can legally hold, and Task 3 should
then move the call outside the critical section rather than adjust the
rank.

Note `devfs.c:218` initializes the lock lazily
(`if (!dyn_lock_ready) { spin_init(...); dyn_lock_ready = 1; }`) rather
than from an init function. Leave that mechanism alone; this task
changes acquires only.

- [ ] **Step 2: Convert every call site**

Replace `spin_lock_raw(&dyn_lock)` with `spin_lock_irqsave(&dyn_lock)`
and every `spin_unlock_raw(&dyn_lock, fl)` with
`spin_unlock_irqrestore(&dyn_lock, fl)` — note the *three* early-return
unlocks at lines 222, 225 and 234 in the register path, which are easy
to miss:

```c
        if (dyn[i].used && name_eq(dyn[i].path, path)) { spin_unlock_irqrestore(&dyn_lock, fl); return -EEXIST; }
```
```c
    if (free_slot < 0) { spin_unlock_irqrestore(&dyn_lock, fl); return -ENOSPC; }
```

- [ ] **Step 3: Verify no raw acquire remains**

```bash
grep -n "spin_lock_raw\|spin_unlock_raw" kernel/fs/devfs.c
```

Expected: no output.

- [ ] **Step 4: Run the tests**

```bash
make test 2>&1 | grep -E "^\[devfs\]|^\[pty\]|PANIC|FAILED"
```

Expected: devfs and pty selftests pass (pty exercises devfs through
`devfs_register`/`devfs_unregister` on every `ptmx_open`/teardown), no
`PANIC`.

- [ ] **Step 5: Run the gauntlet**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/devfs.c
git commit -m "CS0: the devfs dynamic-table lock goes through the rank checker"
```

---

## Task 4: `fbcon` becomes checked, with a raw panic path

The only one of the four with a genuine exception. Everything that
renders normally becomes checked; the panic route stays raw, because
the panicking CPU may already hold a lower-ranked lock and a checked
acquire would call `lock_panic` from inside a panic.

**Files:**
- Modify: `kernel/drivers/video/fbcon.c:79-81,99-101,106-108,127-129,154-157,162,182,198,203`
- Modify: `kernel/drivers/video/fbcon.h` (declare `fbcon_enter_panic`)
- Modify: `kernel/arch/isr.c:28` (call it)

**Interfaces:**
- Produces: `void fbcon_enter_panic(void);` — sets one-way panic mode.
  Task 5 calls it from `vt_panic_reset`.

- [ ] **Step 1: Add the panic-mode acquire helpers**

In `kernel/drivers/video/fbcon.c`, immediately after the `fbcon_lock`
definition, add:

```c
// One-way switch, set by the exception path before it repaints. The
// panicking CPU may hold any lock, so a rank-checked acquire here would
// call lock_panic() from inside a panic and recurse. lock.c solves the
// same problem for serial output with panic_puts(), which skips the
// lock entirely; this keeps the lock (another CPU may be mid-paint) but
// drops the rank check. Never cleared: a panic is terminal.
static volatile int fbcon_panicking;

void fbcon_enter_panic(void) { fbcon_panicking = 1; }

static uint64_t fbcon_acquire(void) {
    return fbcon_panicking ? spin_lock_raw(&fbcon_lock)
                           : spin_lock_irqsave(&fbcon_lock);
}
static void fbcon_release(uint64_t f) {
    if (fbcon_panicking) { spin_unlock_raw(&fbcon_lock, f); }
    else                 { spin_unlock_irqrestore(&fbcon_lock, f); }
}
```

Declare it in `kernel/drivers/video/fbcon.h` alongside the other
prototypes:

```c
void fbcon_enter_panic(void);   // one-way: drop the rank check while panicking
```

- [ ] **Step 2: Route every call site through the helpers**

Replace all eight acquire sites (`fbcon_clear`, `fbcon_putc`,
`fbcon_write`, `fbcon_putc_attr`, `fbcon_putc_at`, `fbcon_cursor`,
`fbcon_selftest`, and the remaining one at line 127) with
`uint64_t f = fbcon_acquire();` and every matching release with
`fbcon_release(f);`. For example `fbcon_putc_at` becomes:

```c
    uint64_t f = fbcon_acquire();
    put_cell((uint32_t)col, (uint32_t)row, (unsigned char)(ch ? ch : ' '), fg, bg);
    if (row == cur_row && col == cur_col) { cur_row = cur_col = -1; }  // painted over
    fbcon_release(f);
```

Note `fbcon_init` calls `spin_init` and then `fbcon_clear()`, which now
takes a checked lock. That is correct and legal: `fbcon_init` runs from
`con_driver_select()` at `kernel/kernel.c:111`, after
`cpu_local_init_bsp()` at line 93.

- [ ] **Step 3: Set panic mode from the exception handler**

In `kernel/arch/isr.c`, in `exception_dump_and_halt`, add the call
before the existing repaint calls:

```c
static void exception_dump_and_halt(struct registers *regs) {
    // Reclaim the screen from any userland terminal so this dump paints.
    fbcon_enter_panic();          // drop the rank check: we may hold anything
    console_set_fb_owned(0);
    tty_set_active(0);
    vt_panic_reset();
```

Add `#include "drivers/video/fbcon.h"` to `isr.c` if it is not already
included.

- [ ] **Step 4: Verify no direct raw acquire remains**

```bash
grep -n "spin_lock_raw\|spin_unlock_raw" kernel/drivers/video/fbcon.c
```

Expected: exactly two hits, both inside `fbcon_acquire`/`fbcon_release`.

- [ ] **Step 5: Run the tests**

```bash
make test 2>&1 | grep -E "^\[fbcon\]|^\[con\]|^\[vt\]|PANIC|FAILED"
```

Expected: `[fbcon] selftest passed`, `[con] selftest passed`, `[vt]
selftest passed`, no `PANIC`.

- [ ] **Step 6: Prove the panic path still paints**

The exception path is not exercised by `make test`. Verify it by hand
with a temporary fault. Add to the end of `kmain`, build, boot, and
confirm the exception screen renders rather than recursing into a lock
panic:

```c
    // TEMPORARY -- CS0 Task 4 verification, revert before committing
    *(volatile uint64_t *)0xdeadbeef000 = 1;
```

```bash
make run-headless 2>&1 | tail -40
```

Expected: the `[exception] Page Fault` dump appears with its register
lines, and **no** `[lock] PANIC: rank inversion` before it. Then revert
the temporary line — `git checkout kernel/kernel.c` — and rebuild before
Step 7.

- [ ] **Step 7: Run the gauntlet**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 8: Commit**

```bash
git add kernel/drivers/video/fbcon.c kernel/drivers/video/fbcon.h kernel/arch/isr.c
git commit -m "CS0: fbcon goes through the rank checker, except while panicking"
```

---

## Task 5: The VT layer gets a lock

The actual bug. Everything before this was tooling visibility; this one
closes three real races.

**Files:**
- Modify: `kernel/tty/vt.c` (add `vt_lock`, guard the state)
- Test: `kernel/tty/vt.c` (extend `vt_selftest`)

**Interfaces:**
- Consumes: `LOCK_RANK_VT` (Task 1), `fbcon_enter_panic` (Task 4).
- Produces: no new external symbols; `vt.h` is unchanged.

- [ ] **Step 1: Write the failing test**

The race needs two CPUs, so the test is a kernel thread that hammers
`vt_switch` while the boot CPU writes to the VT being switched away
from — the exact `render_full` vs `render_diff` interleaving from
finding 3. Add to `kernel/tty/vt.c`, above `vt_selftest`:

```c
// CS0: the switch-vs-write race. A background kernel thread flips
// between VT 0 and VT 1 while the caller writes to VT 0. Without a
// lock, render_full (from the switch) and render_diff (from the write)
// interleave on vts[0].shown, and shown_valid ends up 1 with cells that
// were never painted -- so a later diff skips them forever. The check
// is that the diff cache agrees with the grid once both settle.
static volatile int vtstress_run;
static volatile int vtstress_done;

static void vt_stress_thread(void) {
    while (vtstress_run) {
        vt_switch(1);
        vt_switch(0);
    }
    vtstress_done = 1;
    thread_exit_self(0);
}

static int vt_stress_selftest(void) {
    vtstress_run = 1;
    vtstress_done = 0;
    vt_switch(0);
    // _on(cpu 1): the race needs the switcher on a DIFFERENT core from
    // the writer. On a single-CPU boot there is nothing to race, so a
    // null return is a skip, not a failure.
    if (!thread_alloc_kernel_on(vt_stress_thread, 1)) {
        serial_write_string("[vt] stress SKIPPED: no second CPU\n");
        return 0;
    }
    for (int i = 0; i < 2000; i++) {
        tty_obj_write(&vts[0].tty, "x", 1);
    }
    vtstress_run = 0;
    while (!vtstress_done) { asm volatile("pause"); }

    // Settle: force a full repaint, then verify every cell the diff
    // cache claims is painted actually matches the grid.
    vt_switch(0);
    render_full(&vts[0]);
    int bad = 0;
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            const struct vc_cell *cell = kvt_cell(&vts[0].scr, r, c);
            const struct vc_cell *prev = &vts[0].shown[r][c];
            if (cell->ch != prev->ch || cell->fg != prev->fg ||
                cell->bg != prev->bg || cell->attr != prev->attr) { bad++; }
        }
    }
    if (bad) {
        serial_write_string("[vt] stress FAILED: diff cache diverged, cells=");
        serial_write_hex64((uint64_t)bad);
        serial_write_string("\n");
        return 1;
    }
    serial_write_string("[vt] stress passed\n");
    return 0;
}
```

Call it from `vt_selftest`, folding its result into the existing `fail`:

```c
    if (vt_stress_selftest()) { fail = 1; }

    serial_write_string(fail ? "[vt] selftest FAILED\n" : "[vt] selftest passed\n");
```

`vt.c` already includes `sched/proc.h`, which declares
`thread_alloc_kernel_on` (line 223) and `thread_exit_self` (line 291),
so no new include is needed. Confirm with:

```bash
grep -n "sched/proc.h" kernel/tty/vt.c
```

- [ ] **Step 2: Run it to see it fail**

```bash
make test 2>&1 | grep -E "^\[vt\]"
```

Expected: `[vt] stress FAILED: diff cache diverged, cells=...` on at
least some runs. **This test is a race detector, so it is expected to be
intermittent before the fix** — run it repeatedly to confirm it fails at
all, rather than concluding from one green run that there is no bug:

```bash
for i in (seq 10); make test 2>&1 | grep -E "^\[vt\] stress"; end
```

Expected: at least one `FAILED` across the ten. If ten runs are all
green, raise the write count from 2000 to 20000 before concluding the
race is not reachable, and report the result either way.

- [ ] **Step 3: Add the lock and guard the state**

In `kernel/tty/vt.c`, after the `vts`/`vt_active` definitions, add:

```c
// CS0. Guards vt_active, each VT's diff cache (shown / shown_valid) and
// kd_mode. Rank VT sits above TTY and below FBCON: the write path
// arrives here already holding t->lock (tty_obj_write -> backend
// output) and calls into the con_driver underneath, so the chain
// TTY -> VT -> FBCON is ascending on every path. vt_switch and
// vt_scroll come from the keyboard IRQ holding nothing and take
// t->lock first, in that same order.
static struct spinlock vt_lock;

// One-way, set by the panic path: it may hold any lock, so it takes
// none. Same reasoning as fbcon_panicking.
static volatile int vt_panicking;
```

Initialize it in `vt_init`, before the per-VT loop:

```c
void vt_init(void) {
    spin_init(&vt_lock, LOCK_RANK_VT, "vt");
    con_driver_geometry(&g_cols, &g_rows);
```

Guard `render_diff`'s caller rather than `render_diff` itself (it is
called from both a locked and an unlocked path). Rename the existing
bodies to `_locked` suffixes and add wrappers:

```c
// Callers must hold vt_lock, or be the panic path.
static void render_diff_locked(struct vt_console *vc) { /* existing render_diff body */ }

static void render_full_locked(struct vt_console *vc) {
    vc->shown_valid = 0;
    render_diff_locked(vc);
}
```

`vt_backend_output` runs under `t->lock`, so it takes `vt_lock` inside
it:

```c
static void vt_backend_output(struct tty *t, const char *s, uint32_t n) {
    struct vt_console *vc = t->backend_priv;
    kvt_feed(&vc->scr, s, n);                  // under t->lock already
    uint64_t f = vt_panicking ? 0 : spin_lock_irqsave(&vt_lock);
    if (vc == &vts[vt_active]) {
        serial_write_raw_n(s, n);              // the visible VT mirrors to serial
        if (vc->kd_mode == KD_TEXT) { render_diff_locked(vc); }
    }
    if (!vt_panicking) { spin_unlock_irqrestore(&vt_lock, f); }
}
```

`vt_switch` takes the target VT's tty lock (so `render_full_locked`'s
read of `vc->scr` cannot race a `kvt_feed`), then `vt_lock`:

```c
void vt_switch(int n) {
    if (n < 0 || n >= VT_COUNT) { return; }
    struct tty *t = &vts[n].tty;
    uint64_t tf = spin_lock_irqsave(&t->lock);
    uint64_t f  = spin_lock_irqsave(&vt_lock);
    if (n == vt_active) {
        spin_unlock_irqrestore(&vt_lock, f);
        spin_unlock_irqrestore(&t->lock, tf);
        return;
    }
    vt_active = n;
    struct con_driver *cd = con_driver_active();
    if (cd && cd->clear) { cd->clear(); }
    if (vts[n].kd_mode == KD_TEXT) { render_full_locked(&vts[n]); }
    spin_unlock_irqrestore(&vt_lock, f);
    spin_unlock_irqrestore(&t->lock, tf);
    waitq_wake_all(&vts[n].wait_active);   // after the locks, never under them
}
```

Note the early-return for `n == vt_active` moved *inside* the lock: read
outside it and two CPUs can both decide they are switching.

`vt_scroll` mutates the kvt, so it needs the tty lock too:

```c
void vt_scroll(int delta_lines) {
    uint64_t f = spin_lock_irqsave(&vt_lock);
    int n = vt_active;
    spin_unlock_irqrestore(&vt_lock, f);

    struct tty *t = &vts[n].tty;
    uint64_t tf = spin_lock_irqsave(&t->lock);
    f = spin_lock_irqsave(&vt_lock);
    if (n == vt_active) {                    // still current after the re-lock
        kvt_scroll_view(&vts[n].scr, delta_lines);
        render_full_locked(&vts[n]);
    }
    spin_unlock_irqrestore(&vt_lock, f);
    spin_unlock_irqrestore(&t->lock, tf);
}
```

`vt_panic_reset` takes nothing, and says why:

```c
void vt_panic_reset(void) {
    // No locks: the panicking CPU may hold any of them, and a checked
    // acquire would call lock_panic() from inside a panic. fbcon does
    // the same (fbcon_enter_panic). Another CPU may be mid-paint; the
    // panic-stop NMI has already been sent by the time this runs.
    vt_panicking = 1;
    vt_active = 0;
    vts[0].kd_mode = KD_TEXT;
    struct con_driver *cd = con_driver_active();
    if (cd && cd->clear) { cd->clear(); }
    render_full_locked(&vts[0]);
}
```

In `vt_ioctl`, guard the two `kd_mode` sites:

```c
    case KDSETMODE: {
        uint64_t f = spin_lock_irqsave(&vt_lock);
        vts[idx].kd_mode = (a == KD_GRAPHICS) ? KD_GRAPHICS : KD_TEXT;
        int repaint = (idx == vt_active && vts[idx].kd_mode == KD_TEXT);
        if (repaint) { render_full_locked(&vts[idx]); }
        spin_unlock_irqrestore(&vt_lock, f);
        return 0;
    }
    case KDGETMODE: {
        if (!arg) { return -EFAULT; }
        uint64_t f = spin_lock_irqsave(&vt_lock);
        *(int *)arg = vts[idx].kd_mode;
        spin_unlock_irqrestore(&vt_lock, f);
        return 0;
    }
```

Leave `vt_active_tty`, `vt_tty` and `vt_active_index` reading
`vt_active` unlocked, but make the variable `volatile` so the compiler
cannot cache it across a call:

```c
static volatile int vt_active;
```

A torn read is impossible for an aligned `int`, and these callers only
need *a* valid VT index, not a stable one — document that:

```c
// vt_active is volatile and read without vt_lock here on purpose: these
// resolve "whichever VT is active right now", which is inherently a
// snapshot. An aligned int cannot tear, so the worst case is routing to
// the VT that was active a microsecond ago -- which is what /dev/tty0
// means anyway.
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
make test 2>&1 | grep -E "^\[vt\]"
```

Expected: `[vt] stress passed` and `[vt] selftest passed`. Then confirm
it is *reliably* fixed, since the test is a race detector:

```bash
for i in (seq 10); make test 2>&1 | grep -E "^\[vt\] stress"; end
```

Expected: ten `[vt] stress passed`, no failures.

- [ ] **Step 5: Verify the panic path still works**

Re-run Task 4 Step 6's temporary fault, since `vt_panic_reset` changed:

```c
    // TEMPORARY -- CS0 Task 5 verification, revert before committing
    *(volatile uint64_t *)0xdeadbeef000 = 1;
```

```bash
make run-headless 2>&1 | tail -40
```

Expected: the exception dump paints, with no `[lock] PANIC` preceding
it. Revert the temporary line and rebuild.

- [ ] **Step 6: Run the gauntlet**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`. A rank-inversion panic naming `vt`
here means a path reaches the VT layer holding something ranked above
251 — read the held-stack line in the panic output and report it; do not
raise `LOCK_RANK_VT` to silence it, because the TTY → VT → FBCON chain
is what makes the rest of this task correct.

- [ ] **Step 7: Commit**

```bash
git add kernel/tty/vt.c
git commit -m "CS0: the VT layer gets a lock; switch and scroll stop racing console output"
```

---

## Task 6: Adversarial selftest for the newly-checked ranks

Proves the checker actually catches an inversion at each rank this
milestone converted — otherwise Tasks 2-5 have made the locks *look*
checked without evidence that the checking works at those ranks.

**Files:**
- Modify: `kernel/sync/lock.c` (extend `lock_selftest`)

**Interfaces:**
- Consumes: `lock_rank_ok(uint8_t rank)` from `lock.h:132` — returns 1
  if acquiring `rank` right now would be legal on this CPU. It exists
  precisely so a selftest can prove detection without tripping the
  panic.

- [ ] **Step 1: Write the failing test**

Append to `lock_selftest()` in `kernel/sync/lock.c`:

```c
    // CS0: prove the checker rejects a descending acquire at each rank
    // converted from spin_lock_raw. Holding a high leaf rank, every
    // lower rank must be refused.
    {
        struct spinlock probe;
        spin_init(&probe, LOCK_RANK_FBCON, "rank-probe");
        uint64_t f = spin_lock_irqsave(&probe);
        int bad = 0;
        if (lock_rank_ok(LOCK_RANK_VT))    { bad = 1; }   // 251 under 254
        if (lock_rank_ok(LOCK_RANK_PTY))   { bad = 1; }   // 252 under 254
        if (lock_rank_ok(LOCK_RANK_DEVFS)) { bad = 1; }   // 253 under 254
        if (lock_rank_ok(LOCK_RANK_RAND))  { bad = 1; }   // 250 under 254
        if (lock_rank_ok(LOCK_RANK_FBCON)) { bad = 1; }   // equal rank is an inversion
        // and the legal direction is still legal
        if (!lock_rank_ok(LOCK_RANK_SERIAL)) { bad = 1; } // 255 above 254
        spin_unlock_irqrestore(&probe, f);
        if (bad) {
            serial_write_string("[lock] selftest FAILED: rank check wrong at a CS0 rank\n");
            return;
        }
    }
```

- [ ] **Step 2: Run it**

```bash
make test 2>&1 | grep -E "^\[lock\]"
```

Expected: `[lock] selftest passed`. If it reports the CS0 failure line,
the checker is not enforcing at these ranks — that is a real finding
about `lock_rank_ok`, not a test bug. Investigate `lock.c`'s rank
comparison before changing the test.

- [ ] **Step 3: Run the gauntlet**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 4: Commit**

```bash
git add kernel/sync/lock.c
git commit -m "CS0: prove the checker rejects inversions at the converted ranks"
```

---

## Task 7: Close out CS0

**Files:**
- Modify: `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS0 section)
- Modify: `kernel/sync/lock.h` (the raw-acquire comment)

- [ ] **Step 1: Record what the audit found**

In the spec's CS0.1, replace "pending confirmation" framing with the
result. Write down the three races found (`vt_active` unguarded,
`shown`/`shown_valid` raced between `render_full` and `render_diff`,
`kvt_scroll_view` racing `kvt_feed`), that per-VT *writes* were already
serialized by `t->lock`, and that the fix is the TTY → VT → FBCON
chain. Note that the source document looked at `console.c`/`kvt.c`/
`con_driver.c` but not `vt.c`, which is where the state actually lives.

In CS0.2, record that `con_driver_select()` runs after
`cpu_local_init_bsp()`, so none of the four locks had the
before-per-CPU-state excuse, and that `fbcon` needed a panic-mode
exception for a different reason.

- [ ] **Step 2: Update the `spin_lock_raw` doc comment**

In `kernel/sync/lock.h`, extend the comment above `spin_lock_raw`
(currently at lines 133-137) to name the two surviving legitimate uses,
so the CI grep in CS5.4 has a written rule to enforce:

```c
// Rank-free acquire/release. Uses no per-CPU state, so it is safe
// before cpu_local_init() has installed a GS base -- which serial
// output needs, since it runs from the very first line of kmain. Only
// for leaf locks that never nest inside another lock.
//
// There are exactly two legitimate uses, and CS5.4 adds a CI check that
// fails review on any third:
//   1. serial.c -- runs before cpu_local_init().
//   2. The panic path (fbcon_acquire / vt.c when vt_panicking): the
//      panicking CPU may hold any lock, so a checked acquire would call
//      lock_panic() from inside a panic and recurse.
// Everything else was converted in CS0; a registered-but-always-raw
// lock is exactly as invisible to the checker as an unregistered one.
```

- [ ] **Step 3: Verify the tree-wide state**

```bash
grep -rn "spin_lock_raw" kernel --include="*.c" | grep -v "kernel/sync/lock.c"
```

Expected: hits only in `kernel/drivers/char/serial.c`,
`kernel/drivers/video/fbcon.c` (inside `fbcon_acquire`/`fbcon_release`),
and `kernel/tty/vt.c` (the `vt_panicking` branches). Any other file is
an incomplete conversion.

- [ ] **Step 4: Final gauntlet**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md kernel/sync/lock.h
git commit -m "CS0 done; record what the VT audit turned up"
```

---

## Notes for the executor

- **Task order matters for Tasks 1, 4 and 5 only.** Task 1 defines the
  ranks Task 5 needs; Task 4's `fbcon_enter_panic` is called by Task
  5's panic path. Tasks 2 and 3 are independent of everything and can
  move.
- **The `for i in (seq 10)` loops are fish syntax**, matching this
  project's shell. In bash they are `for i in $(seq 10); do ...; done`.
- **Do not "fix" a rank-inversion panic by raising a rank.** Every rank
  in this plan has a written reason tied to a call chain. A panic means
  a path exists that the audit did not find — report it with the
  panic's held-stack line, which `lock_panic` prints.
- **CS0 changes no user-visible behaviour**, so there is no
  `docs/stdlib.md` or `docs/abi-compatibility.md` work. If a conversion
  turns out to require an ABI-visible change, stop: that is a signal the
  design is wrong, not a reason to edit those files.
