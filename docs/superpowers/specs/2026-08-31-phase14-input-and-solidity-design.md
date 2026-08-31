# Phase 14: raw keyboard input, and a correctness-hardening sweep

**Status:** design, approved 2026-08-31.
**Predecessor:** Phase 13 (below) lands the currently-uncommitted tree.
**Successor:** Phase 15 (separate spec) — `ppoll` + `select`/`poll`
shim, `dup`/`dup2`/`F_DUPFD`, real `execve(argv, envp)`, futex
`REQUEUE`/`CMP_REQUEUE`.

---

## 1. Why

Two unrelated needs, deliberately bundled because both are about making
what already exists *solid* before more is built on it.

**The keyboard is welded to the line discipline.** `keyboard_handler()`
(`kernel/dev/keyboard.c`) unconditionally calls `tty_input_char(c)`.
There is no way for an application to read raw key events — a
full-screen editor, a game, anything that wants key-up/key-down or keys
the line discipline swallows. The driver itself is also minimal:
Scancode Set 1 make-codes only, no `0xE0` extended keys, no modifier
state, no key-release tracking.

**Accumulated rough edges.** A catalogue of known bugs and half-done
things has built up across the ABI and scheduler work. The project's
rule is that a milestone is not complete until a test *fails on the
pre-change kernel*; several of these items have never had that test
written. They are collected here and fixed together.

## 2. Scope

### In

- **Phase 13 pre-step:** verify the uncommitted `kernel/smp/tlb.c` fix,
  then commit the existing tree.
- **Solidity sweep (§4):** `O_CREAT` value; `nanosleep` busy-spin;
  `stat` timestamps; the SIGSTOP/SIGCONT smoke test; the `smptest`
  assertion; the `proc_list` shadow and its linear scans; `AT_RANDOM`.
- **Input (§5–§7):** keyboard driver rework; a `file_ops` extension
  (`ioctl`, `poll`); an input core with grab-based fan-out;
  `/dev/input/event0` with the evdev ABI; a test-only key-injection
  hook; two userland test suites.
- Docs: `docs/stdlib.md`, `docs/abi-compatibility.md`, `README.md`.

### Out (deferred to Phase 15)

- The `ppoll` **syscall** and the `select`/`poll`/`pselect` shim. The
  `poll` **op** in `struct file_ops` lands in Phase 14; only the
  syscall and the fd walker are deferred.
- `dup`/`dup2`/`F_DUPFD`.
- `execve` taking `argv`/`envp` (today's `exec` takes none; `spawnv` is
  a separate call).
- futex `REQUEUE`/`CMP_REQUEUE` (condition-variable broadcast still
  wakes every waiter).
- Full removal of `proc_lock` — see §4.6.
- A real entropy pool / `getrandom` syscall — §4.7 seeds a PRNG only.
- Non-US keyboard layouts.
- `EVIOCSCLOCKID`, `EVIOCSFF`/force feedback, LED and switch classes,
  multiple input devices, a mouse.

---

## 3. Phase 13 — land the existing tree

Not new engineering, but it gates the milestone.

1. **Verify `kernel/smp/tlb.c`.** The working tree has an uncommitted
   fix: the deferred-free queue was a fixed 64-entry array that did an
   emergency `tlb_shootdown(0)` when full — illegal, because
   `vma_munmap` unmaps under `mm_lock` and `tlb_shootdown` asserts no
   lock is held. The fix makes the queue unbounded (256-entry fast path
   + a `kmalloc`ed overflow list allocated *before* `deferred_lock` is
   taken; on OOM the frame is deliberately leaked). The panic it fixes
   hits roughly 1 boot in 10, so the bar is **15 consecutive green
   `make test` runs**. A failure here is a Phase 13 bug.
2. **Commit the tree** in dependency order, one commit per slice:
   `kernel/` source reorg + `-Ikernel`; syscall dispatch table split;
   `stat` family + `struct stat`; working directory; VFAT long names;
   RTC + `CLOCK_REALTIME`; TTY line discipline; Tier-0 syscalls; the
   `kernel/smp/tlb.c` fix; the doc refresh.
3. Refresh `docs/abi-compatibility.md`'s header to "close of Phase 13".

---

## 4. Solidity sweep

Each item is independently committable and ships with a test that
fails on the pre-fix kernel.

### 4.1 `O_CREAT` has the wrong value

`O_CREAT` is `0x100`; Linux x86-64 is `0x40`. A binary compiled against
Linux headers passes `0x40`, which NeoOS does not recognise as create,
so the `open` silently fails to make the file. Called out since Phase
10.

**Fix.** Change the constant in the kernel (`kernel/syscall/…`, the
`O_*` definitions) and `lib/include/fcntl.h`. In the same pass, audit
**every** `O_*` and `AT_*` value against Linux x86-64 and fix any
other mismatch found. Record the corrected set (and any remaining
deliberate divergence) in `docs/stdlib.md`.

**Test.** `fileio`/`stattest` opens a path with `O_WRONLY | 0x40` (the
literal Linux value) and the file is created and `stat`-able.

### 4.2 `nanosleep` busy-spins

`sys_nanosleep` (`kernel/syscall/sys_misc.c`) is
`while (timer_ticks() < deadline) { schedule(); }` — it never blocks,
it spins through the scheduler for the whole sleep, burning a core on
an otherwise idle system.

**Fix.** `waitq_sleep_timeout(&q, NULL, deadline)` already exists for
exactly this (it is what `futex(FUTEX_WAIT)` with a timeout uses). Use
a private `struct waitq` that nothing ever wakes; the sleeper is
dequeued by `waitq_timeout_tick()` when the deadline passes. Preserve
the current semantics: relative, rounded **up** to a whole tick, `rem`
ignored (documented — nothing interrupts a sleep partway yet).

**Test.** A selftest thread sleeps 50ms while a counter tracks
scheduler passes on its CPU; post-fix the count is near zero, pre-fix
it is in the thousands.

### 4.3 `stat` timestamps are all zero

`vfs_stat_vnode` synthesizes `st_atime`/`st_mtime`/`st_ctime` as 0
because `struct vnode` carries no timestamps — even though the FAT
**write** path already computes wall time and stores the DOS
date/time fields (`kernel/fs/fatfs.c`).

**Fix.** Add `int64_t mtime, atime, ctime` (seconds since the Unix
epoch) to `struct vnode`. FAT `read_inode` decodes the DOS date/time
fields it already writes into these. `vfs_stat_vnode` copies them
through. `ramfs` reports the boot epoch (its files are created at
runtime and it has no on-disk time); `devfs` reports the boot epoch.
When the RTC could not be read at boot, FAT timestamps decode against
a 1980 base as DOS intends and `docs/stdlib.md` notes the skew.

**Test.** `stattest`: create a file, `stat` it, assert `st_mtime` is
within a few seconds of a `clock_gettime(CLOCK_REALTIME)` reading.

### 4.4 The SIGSTOP/SIGCONT race test is a smoke test

`sigtest`'s stop/continue loop passes with the atomicity fix in
`signal_do_stop`/`signal_do_continue` *reverted* — the vulnerable
window is microseconds wide and ten rounds never land in it. The fix
rests on construction, not on a reproducer.

**Fix.** A build switch `NEOOS_DEBUG_STOP_WINDOW` inserts a
`schedule()` yield into the exact window between the two operations.
With the switch on and the fix reverted, a bounded loop in `sigtest`
reliably observes the lost wakeup (a continued thread that never
runs); with the fix in place it never does.

**Test.** `sigtest` gains that loop; it is expected to fail when built
`NEOOS_DEBUG_STOP_WINDOW=1` against a tree with the fix reverted. CI
runs the normal build; the switch is a manual regression check
documented in the handoff.

### 4.5 `smptest` reports instead of asserting

`smptest` prints which CPUs its threads ran on rather than asserting a
spread — and whether they spread depends on another CPU being idle,
which with a dozen boot programs it often is not.

**Fix.** The kernel already asserts the real invariant in the steal
selftest via a user-thread migration counter. Expose that counter to
userland through the same `-DNEOOS_TEST_HOOKS`-gated syscall used for
key injection (§7.5) — one more request code — and make `smptest`'s
pass condition "migration count > 0 across the run". In a non-test
build the read returns `-ENOSYS` and `smptest` keeps its current
reporting behaviour.

**Test.** `smptest` fails if the counter is zero.

### 4.6 `proc_list` shadows the process hash table

`proc_table` (`kernel/sched/proc_table.h`) is the real lookup
structure, but `proc.c` still maintains a `proc_list` linked list under
`proc_lock` "during transition", and the process-group signal paths
(`proc.c:654`, `:672`, `:826`) plus `wait4` iteration walk it linearly.
`proc_lock` is *also* reused by `thread.c` to guard the `kzombies`
list and is deliberately held across `sig_lock` for group-signal
ordering.

**Fix (scoped).** Make `proc_table` the sole lookup and iteration
source: replace every `for (p = proc_list; …)` with a `proc_table`
iterator, and delete `proc_list` and its add/remove sites. **Keep the
lock** — renamed to what it now actually guards (`kzombies` +
group-signal ordering, e.g. `proc_global_lock`) — with its rank and
acquisition order unchanged. Full removal of the lock is Phase 15.

If the `kzombies`/`sig_lock` entanglement turns out shallow while doing
this, finish the removal; if not, stop at the scan removal — that is
the low-risk solid win.

**Test.** The group-signal and `wait4` selftests are unchanged and
green; add one asserting a process is findable by PID immediately
after `spawn` and gone immediately after reap, with no `proc_list`
present. The rank checker guards the rename.

### 4.7 `AT_RANDOM` is not random

`AT_RANDOM` is derived from the tick counter and nearby addresses.
musl seeds its stack-guard canary from it, so the canary is guessable.

**Fix.** A small CSPRNG (splitmix64 seeding xoshiro256\*\*), seeded at
boot from `rtc_boot_epoch()` ⊕ `RDTSC` ⊕ a stack address ⊕ `RDRAND`
(when `CPUID` advertises it). `AT_RANDOM`'s 16 bytes are drawn from it.
Documented in `docs/stdlib.md` as **a seeded PRNG, not an entropy
pool** — no `/dev/random`, no `getrandom` syscall, reseeding is a
Phase 15+ concern.

**Test.** A selftest asserts the 16 `AT_RANDOM` bytes are not all-zero
and not a monotonic sequence, then draws two further 16-byte blocks
from the CSPRNG and asserts they differ from each other and from
`AT_RANDOM` — i.e. the stream advances rather than repeating.

---

## 5. The `file_ops` extension

`struct file_ops` (`kernel/fs/file.h`) already unifies vnode-backed
files, pipes and sockets behind one vtable. Extend it rather than
inventing a devfs-specific one.

```c
struct file_ops {
    /* … existing: read, write, lseek, getdents, dup, close … */
    int64_t (*ioctl)(struct file_descriptor *f, uint64_t request, void *arg);
    // Returns a POLL* readiness mask for the events the caller asked
    // about. Used by the Phase 15 ppoll walker; implemented now for
    // the fd kinds this milestone touches, a truthful stub elsewhere.
    int     (*poll)(struct file_descriptor *f, int events);
};
```

No op pointer is ever NULL (the existing rule): an fd that cannot
`ioctl` returns `-ENOTTY`, one that cannot be polled reports "always
ready for what it supports".

**`sys_ioctl` (`kernel/syscall/sys_file.c`)** collapses to
`return f->ops->ioctl(f, request, arg);`. The
`devfs_vnode_is_tty()` special-case is deleted.

**devfs (`kernel/fs/devfs.c`)** stops using `{read(buf,len),
write(buf,len)}`. Each device entry names a `const struct file_ops *`
and an open constructor that fills `f->priv`:

| Device | ops | priv |
|---|---|---|
| `CONSOLE`, `TTY` | tty ops | `&console_tty` |
| `NULL`, `ZERO` | trivial ops | none |
| `input/event0` | evdev ops | a per-open `struct evdev_client` |

`isatty` becomes "the fd's `ioctl` accepts `TIOCGWINSZ`" — which is
already how musl probes — so no separate `is_tty` query is needed.

---

## 6. Keyboard driver rework

`kernel/dev/keyboard.c`, IRQ vector `0x21`.

**Decoder.** A Scancode Set 1 state machine: `0xE0` extended prefix,
make codes and break codes (bit 7), and a modifier/lock state
(`Shift_L/R`, `Ctrl_L/R`, `Alt_L/R`, `CapsLock`, `NumLock`). It emits:

```c
struct key_event {
    uint16_t keycode;   // Linux KEY_* value
    uint8_t  pressed;    // 1 = down, 0 = up (2 = autorepeat, if added)
    uint8_t  raw_scan;   // the Set-1 code, for EV_MSC/MSC_SCAN
};
```

and, for character-producing keys under the current modifiers, one
ASCII byte via a **US-QWERTY** table (unshifted/shifted columns;
CapsLock affects letters only; Ctrl maps to control characters).
Layout is US-only, recorded as a divergence.

Both outputs go to the input core (§7). `keyboard.c` no longer calls
`tty_input_char` directly.

---

## 7. The input core and evdev

### 7.1 `kernel/dev/input.c` — fan-out

On each `key_event` from the decoder:

1. **Every** open `evdev_client`: append `EV_MSC/MSC_SCAN/raw_scan`,
   then `EV_KEY/keycode/(0|1|2)`, then `EV_SYN/SYN_REPORT/0` to the
   client's ring buffer; wake its blocked reader.
2. **If `input_grab_owner == NULL`:** forward the decoded character (if
   any) to `tty_input_char()`.

`input_grab_owner` is a single global `struct evdev_client *`. A plain
`open("/dev/input/event0")` never sets it — events flow to that fd's
ring **and** to the line discipline, exactly as on Linux. Only
`ioctl(fd, EVIOCGRAB, 1)` sets it (`-EBUSY` if another client holds
it); `EVIOCGRAB(0)` or `close` clears it. This is the sole mechanism
that severs keyboard → tty.

### 7.2 `struct evdev_client`

Per open. Holds a fixed-size ring of `struct input_event`, a wait
queue for blocked readers, the live key-state bitmap, and a
dropped-events flag. On ring overflow the **oldest** event is dropped
(Linux behaviour); key state is absolute and unaffected.

### 7.3 evdev ABI — `/dev/input/event0`

`struct input_event` on x86-64 is **24 bytes**: `struct timeval`
(two 8-byte `long` = 16) + `__u16 type` + `__u16 code` + `__s32 value`.
Timestamps from `CLOCK_REALTIME`.

- **read** returns whole events only; a buffer smaller than one event
  is `-EINVAL`. Blocks unless `O_NONBLOCK` (`-EAGAIN` on an empty
  ring).
- **write** is `-EINVAL` (no injected events, no force feedback).
- **ioctl:**

  | Request | Behaviour |
  |---|---|
  | `EVIOCGVERSION` | `EV_VERSION` |
  | `EVIOCGID` | `struct input_id { BUS_I8042, vendor, product, version }` |
  | `EVIOCGNAME(len)` | `"NeoOS AT keyboard"`, truncated to `len` |
  | `EVIOCGBIT(0, len)` | EV bitmap: `EV_SYN | EV_KEY | EV_MSC` |
  | `EVIOCGBIT(EV_KEY, len)` | bitmap of the `KEY_*` codes the driver can produce |
  | `EVIOCGBIT(EV_MSC, len)` | `MSC_SCAN` |
  | `EVIOCGKEY(len)` | live key-state bitmap |
  | `EVIOCGRAB` | the grab (§7.1) |
  | `EVIOCGPHYS`, `EVIOCGUNIQ` | `-ENOENT` |
  | `EVIOCSCLOCKID` | `-EINVAL` (documented; timestamps are always `CLOCK_REALTIME`) |
  | anything else | `-EINVAL` |

- **poll** reports `POLLIN` when the ring is non-empty.

### 7.4 Userland headers

`lib/include/linux/input.h` and
`lib/include/linux/input-event-codes.h` ship the `struct input_event`,
`struct input_id`, the `EV_*`/`KEY_*`/`MSC_*`/`SYN_*` constants, the
`EVIOC*` macros, and `BUS_I8042` — musl carries no `linux/*` UAPI
headers. Only the subset NeoOS implements plus the `KEY_*` range a US
keyboard needs. `docs/stdlib.md` gets an evdev section: the supported
ioctl subset, US-layout-only, single device, no `EVIOCSCLOCKID`, ring
overflow drops oldest.

### 7.5 Test-only injection hook

`void input_inject_key(uint16_t keycode, int pressed)` in `input.c`
feeds a synthetic event through the same decoder-output path. It is
reachable from userland only through a syscall compiled under
`-DNEOOS_TEST_HOOKS` (which `make test` sets); the syscall number
returns `-ENOSYS` in a normal build. Documented in `docs/stdlib.md` as
**not part of the ABI, present only in test builds** — the same
pattern the tty selftest already uses with `tty_input_char()`.

---

## 8. The two userland suites

### `userland/evtest.c` — the app that does NOT use the tty

Never opens `/dev/CONSOLE`. Opens `/dev/input/event0` and runs:

1. **Ungrabbed:** `input_inject_key(KEY_A, 1/0)` → reads the
   `EV_MSC`+`EV_KEY`+`EV_SYN` sequence in order, with the right
   `type`/`code`/`value`; a cooperating check confirms `'a'` also
   reached the tty.
2. **Grabbed** (`EVIOCGRAB, 1`): inject `KEY_B` → event0 sees it, the
   tty does not.
3. **Released** (`EVIOCGRAB, 0`): inject `KEY_C` → the tty sees it
   again.
4. `EVIOCGID` / `EVIOCGNAME` / `EVIOCGBIT` return sane values;
   `O_NONBLOCK` read of an empty ring is `-EAGAIN`; a 4-byte read is
   `-EINVAL`.

Prints `[evtest] ALL PASSED`. In a non-test build (`input_inject_key`
syscall → `-ENOSYS`) it prints `[evtest] SKIPPED` and exits 0.

### `userland/ttytest.c` — the app that DOES use the tty (extended)

Keeps its current layout / `isatty` / `TIOCGWINSZ` checks, adds:

- a canonical line with an erase, injected via the hook, read back
  corrected;
- an injected `^C` delivering `SIGINT` to the foreground process group
  (a handler sets a flag);
- a raw-mode read (`VMIN=1, VTIME=0`) returning a single injected
  byte.

Keeps `[ttytest] ALL PASSED`.

### Wiring

Both added to `REQUIRED_MARKERS` in the `Makefile`. `evtest` is
ordered **before** `ttytest` in the boot so a leaked grab hangs the
tty suite — a visible regression. A kernel `input_selftest` covers
decoder correctness, modifier state, `EV_SYN` framing and grab
routing, and asserts the tty recovers when a grab-holding process
exits without releasing.

---

## 9. Error handling and edge cases

- **Grab holder exits without releasing** → the `close` path (and the
  process-exit fd teardown) clears `input_grab_owner`; `input_selftest`
  asserts recovery.
- **Ring overflow** → oldest event dropped, dropped-flag set; key
  state stays absolute.
- **`proc_lock` rename** → rank and acquisition order unchanged; any
  inversion panics the boot via the rank checker.
- **`stat` timestamps with no RTC** → decode against the 1980 DOS
  base; skew documented.
- **Injection syscall in a production build** → `-ENOSYS`; `evtest`
  reports `SKIPPED`, stays a valid binary.
- **`O_CREAT` audit finds a second wrong constant** → fixed in the
  same commit, noted in `docs/stdlib.md`.

---

## 10. Testing

- Every §4 fix ships a test that fails on the pre-fix kernel.
- `input_selftest` (kernel) + `evtest` + `ttytest` (userland) cover
  §5–§8.
- Full `make test` on 4 CPUs stays green; `make test SMP_CPUS=1` still
  isolates SMP bugs.
- Phase 13's `kernel/smp/tlb.c` gate: 15 consecutive green runs.
- `NEOOS_DEBUG_STOP_WINDOW=1` is a documented manual regression check,
  not part of CI.

---

## 11. Documentation at milestone close

- **`docs/stdlib.md`** — the evdev section; the injection-hook note;
  the `O_CREAT`/`O_*`/`AT_*` corrected table; `nanosleep` now truly
  blocks; `stat` timestamps now real; `AT_RANDOM` is a seeded PRNG.
- **`docs/abi-compatibility.md`** — refresh to close-of-Phase-14; move
  the closed items (`O_CREAT`, `stat` timestamps); add evdev and
  `linux/input.h`; note `ppoll` still deferred.
- **`README.md`** — status: raw input, the driver rework, the
  solidity fixes.
- **This spec** committed under `docs/superpowers/specs/`.

---

## 12. Risks

- **§4.6 (`proc_list`)** is the largest. Mitigation: the scoped
  version (scan removal + lock rename, lock kept) is the deliverable;
  full removal is explicitly Phase 15.
- **Keyboard decoder breadth.** Set-1 + extended + modifiers is fiddly;
  the US-only table keeps it bounded. `input_selftest` drives known
  scancode sequences so regressions are caught immediately.
- **evdev ioctl surface.** Real apps probe more than `evtest` does;
  the table in §7.3 is the committed subset and every gap returns a
  truthful error, so an unsupported probe fails cleanly rather than
  misbehaving.
