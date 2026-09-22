# High-resolution timers: hrtimers + timer wheel — design

Date: 2026-09-23. Status: approved direction (approach C: hierarchical
timer wheel + hrtimers, Linux's split), sections below written from it.

## Problem

Everything time-related in NeoOS is quantised to a 10 ms tick:

- `clock_gettime` (every clock, even `CLOCK_MONOTONIC`) returns
  `ticks × 10 ms` — **10 ms resolution** for every program.
- Every timed wait — `nanosleep`, `clock_nanosleep`, `poll`/`ppoll`/
  `select`/`epoll_wait`, `futex(FUTEX_WAIT)`, `rt_sigtimedwait`, the
  kernel's own sleeps — is a tick deadline, rounded up to a whole tick,
  on ONE global list that the BSP scans once per tick. A 100 µs sleep
  takes 10–20 ms.
- The wall clock is a counter the BSP advances by summing the intervals
  it *armed*, which has already drifted once (see `MIN_ARM_NS` in
  `timer.c`).
- Scheduler preemption is event-driven (LAPIC one-shot per slice) but
  floored at **500 µs**, so a sub-500 µs slice is not honoured.
- An idle CPU still wakes at 100 Hz.
- There is no `clock_getres` syscall at all.

The TSC-based nanosecond clock already exists (`sched_clock_ns()`), but
only the scheduler reads it.

## Goals

1. One nanosecond time base for the whole kernel and every clock
   userland can read.
2. Every timed wait expires at its deadline in nanoseconds, not at the
   next tick after it.
3. The scheduler's slice end is an hrtimer: no 500 µs floor, only an
   interrupt-storm guard of a few µs.
4. Coarse kernel timeouts (TCP retransmit / delayed ACK / TIME_WAIT,
   ARP retry) live on an O(1) hierarchical timer wheel instead of a
   thread polling every tick.
5. Tickless idle: an idle CPU arms only its next real timer.
6. Linux-shaped where observable: `clock_gettime` / `clock_getres`
   semantics and values, `TIMER_ABSTIME`, `EINTR` + remaining time.

Non-goals (recorded in `docs/abi-compatibility.md` as still missing):
POSIX interval timers (`timer_create`, `setitimer`, `alarm`),
`timerfd`, `clock_settime`, per-task timer slack (`PR_SET_TIMERSLACK`),
NTP-style clock adjustment.

## 1. Time base and clock-event devices

**Clocksource.** `ktime_get_ns()` — nanoseconds since boot from the
TSC, calibrated against the PIT at boot (today's `sched_clock_ns()`,
renamed and made the single source; `sched_clock_ns()` stays as an
alias). QEMU keeps TSCs in sync across vCPUs; on KVM the host's
`constant_tsc`/`nonstop_tsc` keep it invariant. A per-CPU
last-returned value guards against any backwards step on one CPU.

**Derived clocks.**
- `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW`, `CLOCK_BOOTTIME`:
  `ktime_get_ns()`.
- `CLOCK_REALTIME`: `rtc_boot_epoch() × 10⁹ + ktime_get_ns()`.
- `CLOCK_PROCESS_CPUTIME_ID` / `CLOCK_THREAD_CPUTIME_ID`: the
  scheduler's nanosecond runtime accounting (`sum_exec_runtime`), not
  ticks.
- `clock_getres` (new syscall, all of the above): 1 ns, as Linux
  reports for hrtimer-backed clocks.
- `timer_ticks()` survives as `ktime_get_ns() / 10 ms`: derived, so it
  can no longer drift, and its remaining callers keep working
  unchanged. The BSP-owned `tick_count` counter and the
  armed-interval accumulator are deleted.

**Clock-event device (per CPU), chosen once at boot:**
- **TSC-deadline** when CPUID.1:ECX[24] is set: program
  `IA32_TSC_DEADLINE` (MSR 0x6E0) with the absolute TSC value of the
  expiry. No conversion drift, no maximum interval. Present on the KVM
  host and on real hardware.
- **LAPIC one-shot count mode** otherwise — this is what every TCG test
  run uses (QEMU TCG implements neither TSC-deadline nor invtsc).
  `delta_ns` converts through the boot calibration; intervals beyond
  the 32-bit counter are armed at the maximum and re-armed on expiry.
- **Minimum delta** (`CLOCKEVENT_MIN_NS`): an expiry closer than this
  is armed at `now + min` instead. 2 µs for TSC-deadline, 10 µs for
  LAPIC count mode. Replaces the old 500 µs floor.
- A deadline already in the past is not armed; the caller runs the
  expired timers immediately.

The boot log names the chosen device: `[hrtimer] clockevent:
tsc-deadline` or `[hrtimer] clockevent: lapic-oneshot`.

## 2. hrtimers

```c
struct hrtimer {
    struct rb_node node;          // in the owning CPU's base
    uint64_t expires_ns;          // absolute, ktime_get_ns() scale
    enum hrtimer_restart (*fn)(struct hrtimer *);
    uint8_t  state;               // INACTIVE / QUEUED / RUNNING
    uint8_t  cpu;                 // base it is queued on
};
void hrtimer_init(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *));
void hrtimer_start(struct hrtimer *t, uint64_t expires_ns);  // on THIS CPU
int  hrtimer_cancel(struct hrtimer *t);  // waits out a running callback
int  hrtimer_try_cancel(struct hrtimer *t); // -1 if its callback is running
void hrtimer_forward(struct hrtimer *t, uint64_t interval_ns); // for RESTART
```

- **Per-CPU base**: the existing intrusive rbtree (`kernel/lib/rbtree`),
  keyed by `expires_ns`, with a cached leftmost node. Intrusive, so
  starting a timer never allocates. Protected by a per-CPU spinlock at
  a new rank below the run-queue lock (callbacks wake threads, which
  takes a run-queue lock *after* the base lock is dropped).
- **Interrupt handler**: `timer_handler` becomes
  `hrtimer_interrupt`: take every timer with `expires_ns <= now`, drop
  the base lock, run its callback (IRQs off, never sleeps), re-queue it
  if it returns `HRTIMER_RESTART`, then program the device for the new
  leftmost. Callbacks that wake threads use the same `thread_wake`
  path the tick-scan uses today.
- **Cancel from another CPU**: takes the owning CPU's base lock; if the
  callback is RUNNING, `hrtimer_cancel` spins until it finishes (the
  guarantee `waitq_sleep_timeout` needs before its stack frame goes
  away).
- **Re-arming the device** happens only when the leftmost timer
  changes, so starting a later timer costs no MSR/LAPIC write.

## 3. What moves onto hrtimers

**Timed sleeps.** `struct thread` embeds one `struct hrtimer
sleep_timer`. The existing waitq timeout functions keep their names and
signatures, but their `deadline` argument changes unit: it is now an
ABSOLUTE `ktime_get_ns()` value (`0` still means "none" where it did,
`UINT64_MAX` still means "forever" in `poll_core`):

```c
int waitq_sleep_timeout(struct waitq *q, struct spinlock *release,
                        uint64_t deadline_ns);
int waitq_sleep_timeout_unless(..., uint64_t deadline_ns, volatile int *abort);
int waitq_poll_wait(uint64_t deadline_ns, volatile int *abort);
uint64_t ktime_after_ns(uint64_t ns);   // now + ns, saturating
uint64_t ktime_after_ticks(uint64_t t); // now + t × 10 ms, for coarse callers
```

Every one of the 14 callers is converted in the same change (a single
mechanism, no tick-deadline wrappers left behind to be misused). The
sleeper starts its own `sleep_timer` on its CPU; the callback marks it
timed-out and wakes it; after waking it `hrtimer_cancel`s. The global
`timeout_list`, `timeout_lock` and `waitq_timeout_tick()` are deleted.

**Syscalls converted to exact nanosecond deadlines** (no tick
rounding; relative timeouts become `now + ns`, absolute ones are used
as-is):
- `nanosleep`, `clock_nanosleep` (incl. `TIMER_ABSTIME` against
  `CLOCK_REALTIME` and `CLOCK_MONOTONIC`); on `EINTR` a relative sleep
  writes the remaining time to `rem` (today ignored — a documented
  divergence this removes).
- `poll`, `ppoll`, `select`, `pselect6`, `epoll_wait`/`epoll_pwait`
  (`poll_core`'s deadline becomes ns; epoll's periodic re-evaluation
  cap stays, expressed in ns).
- `futex(FUTEX_WAIT)` (relative; `FUTEX_WAIT_BITSET` is not
  implemented today and stays out of scope).
- `rt_sigtimedwait`.

**The scheduler.** Each CPU has a `sched_timer` hrtimer. It is (re)armed
at `now + fair_slice_remaining_ns()` whenever the running task changes
(in `schedule()` after the switch decision) and when the slice is
recomputed (`sched_setattr`, wakeup preemption checks). Its callback
calls `sched_tick()` and, if that asks for a reschedule, records it in
a per-CPU flag. NeoOS has no need-resched-on-return path: exactly as
`timer_handler` does today, `hrtimer_interrupt` re-programs the device
for the new leftmost timer FIRST (a CPU that switches away with its
device unarmed stops taking timer interrupts for good — see the
"ARM THE NEXT ONE-SHOT BEFORE schedule()" note in `timer.c`) and then
calls `schedule()` itself if any callback flagged it. The idle task arms
no scheduler timer. This is the "force the scheduler to use it" part:
the slice boundary is exactly the hrtimer expiry, bounded only by
`CLOCKEVENT_MIN_NS`.

## 4. The timer wheel

For coarse, usually-cancelled kernel timeouts, Linux-shaped:

```c
struct timer_list {
    struct timer_list *next, **pprev;   // wheel bucket, O(1) unlink
    uint64_t expires;                   // in jiffies
    void (*fn)(struct timer_list *);
    uint8_t  cpu;
};
void timer_setup(struct timer_list *t, void (*fn)(struct timer_list *));
void mod_timer(struct timer_list *t, uint64_t expires_jiffies); // arm or re-arm
int  del_timer(struct timer_list *t);
int  del_timer_sync(struct timer_list *t);   // also waits out a running fn
```

- **Jiffies at 1000 Hz** (`JIFFY_NS = 1 ms`), derived from
  `ktime_get_ns()`. `timer_ticks()` stays at its own 100 Hz scale for
  existing callers; the wheel uses jiffies.
- **Hierarchy**: 4 levels × 64 buckets, each level 8× coarser (1 ms,
  8 ms, 64 ms, 512 ms granularity; ~36 min horizon; later expiries
  clamp to the last level and re-cascade). O(1) insert and delete;
  expiry within one bucket granularity of the requested jiffy (the
  documented wheel trade-off: coarse timers fire slightly late, never
  early).
- **Per-CPU wheel**, locked like the hrtimer base.
- **Driven by a per-CPU `wheel_timer` hrtimer**, armed only for the
  next occupied bucket's jiffy — never a periodic tick when the wheel
  is empty.
- **Callbacks run in thread context**, in a per-CPU `ktimerd` kernel
  thread the wheel hrtimer wakes — NOT in the interrupt. Firing a TCP
  timer transmits, which takes the ARP lock and may spin on the device
  (exactly why `tcp_timer_thread` is a thread today).

Plus `timer_reduce(t, expires)`: arm if idle, otherwise only ever move
the expiry EARLIER (Linux's `timer_reduce`).

**Migrated onto the wheel** — contained, without rewriting the TCP
state machine:
- TCP: ONE `tcp_timer` `timer_list`. `tcp_timer_tick()`'s per-
  connection logic (deadlines + the reclaim rule) is kept as is and
  becomes the callback, which re-arms for the earliest remaining
  deadline across all connections (not armed at all when none).
  Every `x_deadline = timer_ticks() + N` assignment goes through
  `tcp_deadline_in(N)`, which `timer_reduce`s the timer to that
  deadline; the reclaim transitions (`sock_gone` set, entry to CLOSED)
  kick it to the next jiffy. `tcp_timer_thread` and its every-tick
  poll are deleted.
- ARP: ONE `arp_timer`, `timer_reduce`d to each retry deadline, whose
  callback runs `arp_tick()`. netrx stops sleeping one tick at a time
  while a request is pending.

## 5. Tickless idle and housekeeping

There is no periodic tick any more. What the 10 ms tick used to do,
and where it goes:

| tick duty | now |
|---|---|
| advance the wall clock | nothing — derived from the TSC |
| wake expired sleepers | per-thread `sleep_timer` hrtimers |
| slice preemption | per-CPU `sched_timer` |
| TCP/ARP timeouts | the wheel |
| CPU busy/idle accounting | nanoseconds, charged at context switch (`cpu_usage_ticks()` keeps its signature, reporting 10 ms units derived from the ns totals) |
| `[timer] tick=` log once a second | a 1 s hrtimer on the BSP (the log line is kept, same format) |

An idle CPU therefore sleeps in `hlt` until its earliest hrtimer or an
IPI. Every CPU arms its housekeeping state at bring-up, and the BSP's
1 s log timer plus each CPU's first `sched_timer` keep
`[smp] local timer selftest` meaningful: it now requires each CPU to
take at least one hrtimer interrupt, which it still must.

## 6. Error handling and edge cases

- Deadline in the past at start: the timer runs on the next
  `hrtimer_interrupt` pass, which the start path triggers by arming
  `now + min`.
- `hrtimer_start` on an already-queued timer re-keys it (dequeue,
  re-insert).
- A thread migrates CPUs while sleeping: its `sleep_timer` stays on the
  CPU it was started on; cancel works cross-CPU.
- TSC-deadline: writing a past value fires immediately (SDM
  behaviour); count mode: counts below 1 are clamped to 1.
- Overflow: `expires_ns` is 64-bit ns (≈584 years); user timespecs
  whose conversion overflows are clamped to `UINT64_MAX` (sleep
  forever), matching Linux's `KTIME_MAX` handling.

## 7. ABI and documentation

- New syscall `clock_getres` (NeoOS number, shim entry mapping musl's
  `SYS_clock_getres`), returning `{0, 1}` for supported clocks and
  `EINVAL` otherwise.
- `docs/stdlib.md`: remove the "10 ms resolution" and "`rem` ignored on
  `EINTR`" divergences; document the clock-event minimum delta.
- `docs/abi-compatibility.md`: refreshed at the end of the milestone,
  listing interval timers / timerfd / `clock_settime` / timer slack as
  the remaining gaps.

## 8. Testing

Kernel selftests (boot-time, serial markers added to
`CORE_REQUIRED_MARKERS`):
- `[hrtimer] selftest passed`: ordering of out-of-order starts, cancel
  of a queued timer, re-key of a queued timer, cross-CPU cancel of a
  running callback, RESTART/forward.
- `[wheel] selftest passed`: insert at every level, cascade from level
  3 to 0, `del_timer` from each level, expiry never early.

Userland test `hrtest.nex` (embedded in the suite, marker
`PASS hrtest`):
- `clock_getres(CLOCK_MONOTONIC)` = 1 ns; 1000 back-to-back
  `clock_gettime` calls are monotonic with sub-ms steps.
- `nanosleep` 200 µs ×50: never early; the median overshoot bound is
  asserted loosely under TCG (< 2 ms) and reported in the log for KVM.
- `poll(NULL, 0, 3)` returns after ≥ 3 ms and < 13 ms (proves it is no
  longer tick-rounded to 10–20 ms).
- Two CPU-bound threads pinned to one CPU with a 100 µs slice
  (`sched_setattr`): the observed switch rate is > 2000/s — impossible
  under the old 500 µs floor.
- `clock_nanosleep(TIMER_ABSTIME)` wakes at or after its absolute
  deadline.

Regression bar: `tools/gauntlet.sh 15` at 15/15 with zero retries,
the whole `wm-*` suite, and `make desktop KVM=1` checked by hand on the
TSC-deadline path.

## Deviations recorded during implementation (2026-09-23)

What shipped differs from the sections above in these places; each was
a decision made while implementing, with its reason:

- **One global timer wheel and one `ktimerd`**, not one per CPU
  (section 4). NeoOS's timer counts do not need the split, and one wheel
  keeps cancel trivially correct. The **horizon is ~33 s** (64 × 512 ms),
  not "~36 min" as section 4 said; later expiries clamp to the last
  level and re-cascade, as described.
- **The wheel forwards its clock before filing a timer** and arms
  `ktimerd` for the next occupied bucket's *slot* time. Without the
  first, a timer filed after an idle stretch landed in a coarse level
  and fired hundreds of ms late; without the second, `ktimerd` woke
  every jiffy until a coarse bucket came due. Due timers wait on a
  locked list so `del_timer_sync` cannot race one collected but not yet
  run.
- **A timed waitq sleep starts its `sleep_timer` inside the sleep path**,
  after the thread is queued and the caller's guard is dropped, with
  interrupts still off — not before the sleep, under the guard (section
  3). The latter is a lock-rank inversion under guards ranked above the
  hrtimer base (poll heads, epoll) and has a lost-wakeup window. Only a
  callback that itself dequeues the thread reports `-ETIMEDOUT`, so a
  real wake racing the expiry returns 0 (Linux semantics).
- **Wakeup preemption and idle pokes** (`sched_wakeup_check`): a thread
  queued on a busy CPU fires that CPU's slice timer at once (EEVDF's
  check, within the anti-thrash floor) and pokes one idle CPU to steal
  it. Section 3 named "wakeup preemption checks"; with tickless idle
  (section 5) the poke is also the only way an idle CPU learns of work.
- **The idle loop looks for work with interrupts off and then does an
  atomic `sti; hlt`**; with a tick, a wake between the two cost ≤ 10 ms,
  tickless it could cost forever.
- **The boot's network waits** (`netrx_boot_park`) arm their own 10 ms
  one-shot: their bounds were counted in ticks.
- **The virtio-net interrupt is re-routed to an AP** once the APs are
  up. The BSP spends tens of seconds with interrupts off in `kmain`
  under load; the old BSP-only timeout scan froze DHCP's clock with it,
  hiding that RX was starved. Per-thread hrtimers exposed it.
- **Selftests**: the wheel is checked on a private instance with a
  simulated clock (a level-3 timer needs > 4 s of real time and a boot
  is over sooner), plus two real timers through `ktimerd`; the tickless
  check measures an idle AP over ≥ 250 ms windows of uninterrupted
  idleness. `hrtest` asserts sleep bounds on medians and reports the
  worst case (the boot's ATA flush polls with interrupts off for
  ~35 ms on KVM, delaying timers on that CPU). Its slice check counts
  switches per CPU with `sched_getcpu` over 2 × ncpu spinners, because
  `sched_setaffinity` is recorded but not enforced.
