# SMP hardening — handoff (2026-08-31)

---

## UPDATE (2026-08-31, third session): the milestone is COMPLETE

Everything the second session flagged as "not done / not mechanical" is
done. Full write-up: `docs/optimization-summary.md` → "Phase 13.6";
design + plan under `docs/superpowers/`
(`2026-08-31-smp-lifetime-and-lock-detangle*`).

- The `signal.c` thread-list walks are guarded by `p->lock`.
- The signal-send path is split into thin public + static `_locked`
  inner forms; `signal_tkill` no longer recurses.
- `struct thread` and `struct process` are reference-counted.
- `proc_table_for_each_ref` (streaming, ref'd) replaces every
  `proc_list` scan; **`proc_list` / `proc_lock` / RCU are deleted.**
- Processes no longer leak on exit.

Commits `948fba3`, `58d7976`, `d53adf7`, `cd0e44c`, `2a5d88b`,
`3d9d5e2`. Gauntlet green through every one (via the parallel
`pgauntlet.sh` harness, CONC=3).

**Residual:** the net socket-lifetime bug (`recv_one` readers hold no
ref on their socket) — same class, ~20 lines, not yet fixed. Then
resume the Phase 14 plan at Task 2.

Post-SMP milestone roadmap:
`docs/superpowers/specs/2026-08-31-post-smp-roadmap.md`.

---

## UPDATE (2026-08-31, second session): gauntlet is 15/15 green

Races #1, #2, #3 and the `stack_slots` under-locking are **fixed and
verified** — 15 consecutive clean `make test` runs on 4 CPUs. That is
the "Phase 13 solid" bar. Full write-up:
`docs/optimization-summary.md` → "Phase 13.5".

| # | Commit | State |
|---|---|---|
| 1 tlb.c | `a2a0014` | **verified** — 15/15, full tick logs |
| 2 serial | `69a3414` | fixed |
| 3 thread-list | `04e4866` | fixed — `proc_lock` guards all 5 sites |
| stack_slots | `0e4c093` | fixed — `p->mm_lock` |

**Not done, and NOT mechanical:** the `signal.c` thread-list walks
(`signal_send_process` etc.) and plan **Task 6** (drop `proc_list`) both
sit on one architectural knot — `proc_lock`, the `proc_table` bucket
locks (same rank `LOCK_RANK_PROCTABLE`), and `sig_lock` (rank above),
with the process-group signal paths holding the outer lock across
delivery. `signal_send_process` is *designed* to be called with
`proc_lock` held (`signal_kill` does), so a plain lock-add
self-deadlocks. This needs a design pass (brainstorm → spec): an
`_locked` variant of the signal-send path, or a real thread refcount so
"find thread, drop lock, act" is safe. RCU is also inert
(`rcu_init`/`synchronize_rcu` never called) — processes leak on exit.

Everything below is the ORIGINAL handoff, kept for context. Its "Races
still open" and "Recommended order" are superseded by the above.

---

**Written for a fresh session.** The Phase 14 plan
(`docs/superpowers/plans/2026-08-31-phase14-input-and-solidity.md`) is
paused at Task 1: landing the pre-Phase-14 tree turned up **multiple
latent SMP races** the handoff docs did not know about. The user chose
to **deep-clean them all** before Phase 14 proceeds.

The SDD ledger is
`.superpowers/sdd/2026-08-31-phase14-input-and-solidity/progress.md` —
read it. Task briefs 2–17 are already generated in that directory.

## Committed and good

| Commit | What |
|---|---|
| `ce963ea` `bb069e7` `967bf85` | Phase 14 spec + reconciliation + plan |
| `a2a0014` `6358b97` | **Phase 13 landed** — coreutils ABI surface, kernel/ reorg, tlb.c fix, tier0test fix, doc refresh. `make test` passes (single runs). |
| `69a3414` | **Race #2 FIXED** — `tty.c` wrote to COM1 via the unlocked `serial_putc`; userland `printf` interleaved with kernel `serial_write_string`, ~1/9 corrupting the boot marker so `make test` reported a completed boot as failed. Now routes through locked `serial_write_string_n` / new `serial_write_raw_n`. Verified: 1 clean gauntlet run, marker intact. |

Environment note: a fresh checkout needs
`git -c protocol.git.allow=always submodule update --init --recursive`
(git disables `git://` for submodules).

## The gauntlet

`.superpowers/sdd/2026-08-31-phase14-input-and-solidity/gauntlet.sh` —
15 consecutive `make test` runs, trusts `make test`'s exit code, saves
the serial log of any failure to `gauntlet.log.serial.runN`. The bar
for "Phase 13 solid" is 15/15 green.

Progress so far: run-1 gauntlet reached 8/15 then hit race #2. After
the #2 fix, a fresh gauntlet hit race #3 on run 1.

## Races still open

### #1 — tlb.c deferred-free (committed, UNVERIFIED)

The fix is in `a2a0014` (`kernel/smp/tlb.c`: unbounded queue, overflow
node `kmalloc`ed before `deferred_lock`). Original symptom: `[lock]
PANIC: tlb_shootdown with a lock held`, ~1/10, presents as a serial log
with **zero `[timer] tick=` lines** (a CPU spinning with IF off). The
gauntlet has not run long enough post-#2-fix to confirm #1 is gone.

### #3 — thread-list under-locking (root-caused, NOT fixed)

`p->threads` and `p->zombies` (linked lists via `thread->proc_next`)
are mutated with **no lock** — only `cli`, which the code's own
comments (`thread_exit_self`) admit is "no protection at all on four"
CPUs.

Sites, all in `kernel/sched/`:

| Site | File:line | Currently |
|---|---|---|
| `thread_alloc` — insert into `p->threads`, `p->refcount++`, and the `t->tid = (p && !p->threads)...` read | `thread.c:70,81-84` | unlocked |
| `thread_exit_self` (`p != NULL` branch) — move t from `threads` to `zombies`, `waitq_wake_all(join_waiters)` | `thread.c:164-179` | `cli` only |
| `thread_join` — scan `zombies`, scan `threads`, unlink from `zombies` | `thread.c:257-280` | unlocked |
| `proc_reap` — drain `p->zombies` | `proc.c:776-790` | unlocked |
| signal iteration over `p->threads` | `proc.c:675,759` | **holds `proc_lock`** (already correct) |

Observed failure: `[smptest] FAILED: thread_join 2`. smptest joins 8
threads that all exit at once. `thread_join` scans `zombies` (miss),
scans `threads` (miss — the exit path just unlinked t, not yet linked
to zombies) → returns `-ESRCH`.

**Fix direction:** guard `p->threads` / `p->zombies` with `proc_lock`
(rank `LOCK_RANK_PROCTABLE` = 0, the outermost — no inversion risk)
in all four unlocked sites, matching what the signal paths already do.
Care points:
- `thread_exit_self`: scope `proc_lock` to JUST the list move. Do NOT
  hold it across `proc_put(p)` (switches CR3, frees the address space,
  sends SIGCHLD to the parent — long, and may take other process
  locks) or across `schedule()` (panics if any lock is held). The
  `else`/kernel-thread branch already shows the tight-scope pattern.
- `thread_join`: scan under `proc_lock`; on "found in zombies", unlink
  + copy `exit_code` under the lock, then **release** before
  `thread_wait_off_cpu(z)` (spins on another CPU) and the frees. On
  "still running", sleep with `waitq_sleep(&p->join_waiters,
  &proc_lock)` — it releases `proc_lock` atomically with the block and
  re-acquires on wake (see `kernel/ipc/futex.c:130` for the exact
  pattern; note it keeps IF off across the release). Restructure the
  loop so it does not double-lock.
- `proc_reap`: grab `p->zombies` head under `proc_lock`, null it,
  release, then free each zombie. Its callers (`wait4`,
  `wait_for_pid`) do NOT hold `proc_lock`, so no recursion.

### Suspect list (same class, not yet triggered)

- **`p->stack_slots` bitmap + stack page tables** — `thread_stack_alloc`
  / `thread_stack_free` (`proc.c:119,149`) manipulate the bitmap AND
  `p->pml4_phys` via `paging_map_into`/`paging_unmap_from` with
  **neither `mm_lock` nor `proc_lock`**. Concurrent `thread_create` on
  a live multi-threaded process = double-mapped stacks / bitmap
  corruption. Should take `p->mm_lock` (rank MM = 3). Callers from
  `spawn`/`exec_task` (`proc.c:369,458`) are on a fresh process (no
  contention) but must not then double-lock.
- **`wait4` iterates `proc_list` lock-free** (`proc.c:826`) and calls
  `proc_reap` — this is also the plan's **Task 6** (remove `proc_list`,
  make `proc_table` the sole store). Doing Task 6 as part of the
  deep-clean is natural: it removes a whole racy structure.
- `kzombies` global list — the kernel-thread exit branch holds
  `proc_lock`; `idle_entry`'s drain should be checked for the same.

## Recommended order for the deep-clean

1. Fix #3 (thread-list `proc_lock`) — one coherent pass over the four
   sites. Build, gauntlet.
2. Fix the `stack_slots` / stack-pagetable locking (`mm_lock`).
   Build, gauntlet.
3. Do plan **Task 6** (drop `proc_list`, `proc_lock` → rename,
   `proc_table` iterator) — kills the `wait4` race. Build, gauntlet.
4. Whatever runs 4–15 of the gauntlet then surface, root-cause each
   with `superpowers:systematic-debugging`. Save every failure's
   serial log.
5. When 15/15 green: that is "Phase 13 solid". Update
   `docs/optimization-summary.md` (or a new Phase 13.5 section) with
   the races found and fixed. Then resume the Phase 14 plan at Task 2.

Each fix is its own commit, `Phase 13:` prefix (or start a `Phase 13.5:`
/ `SMP hardening:` prefix), standard trailer. Each should ship with a
test that fails on the pre-fix kernel where one can be made
deterministic; where it genuinely can't (pure timing races), the
gauntlet is the regression test and the commit message says so.

## Known cosmetic issue (not a blocker)

`[timer] tick=` prints as three separate locked `serial_*` calls, so it
can still interleave *between* its parts (`[timer] tick=<other>0x..`).
Never splits a single-call marker, so it does not fail `make test`.
Fold `[timer] tick=` into one `serial_write_string_n` of a
pre-formatted buffer when convenient, or gate the whole tick log behind
a debug flag (it is debug spam — hundreds of lines per run).
