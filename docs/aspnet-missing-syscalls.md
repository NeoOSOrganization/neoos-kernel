# ASP.NET Core on NeoOS — status and remaining gaps

**Milestone reached (2026-09-07): ASP.NET Core / Kestrel serves HTTP on
NeoOS.** A `WebApplication.CreateSlimBuilder` app with a `MapGet("/")`
handler, built NativeAOT for `linux-musl-x64` against the
`~/opt/cross-x86_64-neoos` toolchain, boots on NeoOS, binds
`0.0.0.0:30000`, and answers real requests: `curl` from the host (via
QEMU `hostfwd`) gets `HTTP/1.1 200 OK` with the handler's body. Verified
**40/40 sequential requests**, **40/40 at concurrency 8**, and
**100/100 at concurrency 16** (2026-09-08, after EPOLLET landed).

## Kernel fixes this needed (all landed)

| area | fix |
|---|---|
| `userland/user.ld` | `.eh_frame`/`.eh_frame_hdr` were gc'd → GC stack walk FailFast. Fixed + script rebuilt from the stock ld script. (Earlier milestone.) |
| `kernel/syscall/sys_poll.c` | `epoll_wait` snapshotted its fd set once and blocked forever. .NET's `SocketAsyncEngine` starts its event-loop thread (which calls `epoll_wait` on an **empty** set) and registers sockets only afterwards. `epoll_wait_core` now re-snapshots in a loop with a bounded `poll_core` deadline. |
| `kernel/sync/epoll.c` + `kernel/sched/fd_table.c` | `close(fd)` did not remove the fd from epoll sets (Linux does). .NET closes a connection socket without `EPOLL_CTL_DEL` and reuses the fd number → `epoll_ctl(ADD)` hit `EEXIST`. Added `epoll_forget_fd()`, called from `fd_table_close()`/`fd_table_dup2()`, plus a global epoll-object registry (new `LOCK_RANK_EPOLL_LIST`). `epoll_ctl_do()` now also fires `waitq_poll_notify()`. |
| `kernel/net/socket.c` + `kernel/net/tcp.c` | `MSG_PEEK` was ignored → .NET's 1-byte peek probe consumed the first request byte (`GET` parsed as `ET` → 405). `tcp_recv()` gained a `peek` parameter; `MSG_DONTWAIT` now forces the call non-blocking. |
| `kernel/net/socket.c` | `sock_ioctl` returned `ENOTTY` for everything. Added `FIONREAD` (returns `rcv_len`) and `FIONBIO`. |

## Missing / mishandled syscalls (Linux x86_64 numbers)

| # | name | status | notes |
|---|---|---|---|
| 203 | `sched_setaffinity` | **ENOSYS — needs impl** | **.NET Server GC hangs before `Main`.** Server GC starts one heap thread per CPU and pins each with `sched_setaffinity`; ENOSYS hangs that startup. Workaround: publish with `-p:ServerGarbageCollection=false` (Workstation GC). `Microsoft.NET.Sdk.Web` defaults Server GC on, so this is mandatory for a web app today. Fix: accept + validate the cpumask (must intersect online CPUs else EINVAL), return 0; NeoOS may ignore the hint but must not ENOSYS. Record the divergence in `docs/stdlib.md`. |
| 157 | `prctl` | ENOSYS — benign so far | `[shim] ENOSYS 157` at startup (`PR_SET_NAME` / `PR_SET_THP_DISABLE`). Non-fatal. |
| 309 | `getcpu` | ENOSYS — benign so far | `[shim] ENOSYS 309`. Kernel *has* `SYS_GETCPU` (41); the shim just doesn't map Linux 309 → 41. One-line shim fix. |
| — | UDP `MSG_PEEK` | not implemented | `recv_one()` (datagram path) still ignores `MSG_PEEK`/`MSG_DONTWAIT`. Only the TCP path was fixed. DNS works without it. |

## Concurrent request handling — FIXED (2026-09-08)

Concurrency 8 used to stop answering under sustained load: a HANG, not
the `AccessViolation` this section once described (that was the mm
frame-exhaustion bug fixed earlier — thread stacks eagerly committing
8 MiB each). No `[usrflt]`, no `[fault-audit]`, nothing in the kernel
log. What the kernel saw at the stall: **24 of the 32 TCBs in
`CLOSE_WAIT` with the socket still open and `tcp_close` never called**
— Kestrel accepting connections and then neither serving nor closing
them.

Two kernel bugs, found in that order. Both are fixed, and ASP.NET Core
now serves 40/40 at concurrency 8 and 100/100 at concurrency 16.

### Bug 1: NeoOS's epoll was level-triggered; .NET registers EPOLLET

Instrumenting `epoll_ctl_do` showed, for every socket .NET adds to its
engine's epoll set:

```
[ep-dbg] ctl op=1 events=0x80000005
```

`0x80000005` = `EPOLLET | EPOLLOUT | EPOLLIN`. NeoOS's epoll was
level-triggered only, and the flag was not merely ignored, it was
erased: `epoll_wait_core` copied the registration into a
`struct pollfd` with `pfd[i].events = (short)e->events`, truncating a
32-bit mask into 16 bits.

**Why that hangs Kestrel rather than merely being inefficient.** The
usual intuition — "level-triggered delivers a superset of edge-
triggered, so it is safe" — is wrong for a consumer *written against*
ET. `SocketAsyncEngine`'s loop is: `epoll_wait` returns fd X readable →
hand X to the thread pool → straight back to `epoll_wait`. Under ET
that blocks until *new* data arrives. Under LT, X is still readable
(the work item has not run yet), so the engine spins dispatching
duplicate work items for the same socket, the pool fills, and accepted
connections are never serviced or closed.

It also explained every other data point: sequential requests worked
(one connection, duplicates harmless); 2 and 4 worked, 8 did not; and
`userland/epolltcp.c` passed 16/16 at concurrency 8 because it is
*written* for LT and drains each fd in the iteration it is reported in.

**The fix** (`kernel/sync/epoll.{c,h}`, `kernel/syscall/sys_poll.c`,
`kernel/sync/poll_head.{c,h}`, `kernel/fs/file.{c,h}`,
`kernel/net/socket.c`, `kernel/ipc/socketpair.c`, `kernel/ipc/pipe.c`):
stop truncating the mask; per registration remember the mask already
reported (`last_ready`) and the object's readiness counter at that
moment (`last_seq`); report `ready_now & ~last_ready`; re-arm when the
object's counter moves. `poll_head_notify` bumps that counter, and a
TCP stream socket — which has no poll head, because the TCB is recycled
and pollers must not be threaded onto it — exposes its TCB's counter
through the new `file_ops.ready_seq`. `EPOLL_CTL_MOD` and a re-`ADD`
re-arm. Full semantics and the two deliberate divergences are in
`docs/stdlib.md`.

Sampling alone would NOT have been enough: if the level drops and rises
entirely between two `epoll_wait` calls (the worker drains the socket
on a pool thread while the engine is in `epoll_wait`, then the next
request arrives), "ready now" equals "ready last time" and the edge is
invisible. That is what the per-object counter is for, and
`userland/epollet.c` case 8 is the regression test for exactly it.

### Bug 2: the wall clock stopped, so no timed wait ever expired

Found while writing that test: `epoll_wait(fd, ..., 100)` never
returned. Two defects in `kernel/drivers/char/timer.c`, both of which
made `timer_ticks()` — the unit EVERY timed sleep in the system is
measured in, including TCP's retransmit and delayed-ACK deadlines —
advance far slower than real time:

1. **The one-shot was re-armed AFTER `schedule()`.** `timer_handler`
   computed the next interval at the end of the function, past a
   `schedule()` call that switches stacks and does not come back until
   the preempted task is picked again. A CPU that took one mid-handler
   preemption therefore had no armed timer at all — and could not
   schedule that task back without one. Measured: fewer than 500 BSP
   timer interrupts across a two-minute boot. The clock is BSP-owned,
   so it simply stopped.
2. **The accumulator banked what was asked for, not what was armed.**
   `ns_to_lapic_count` clamps the programmed count to a ~500 us floor
   while `bsp_ns_accum` kept crediting the unclamped request, so a busy
   run queue handing out 50 us slices banked 50 us for every 500 us of
   real time — a clock running up to 10x slow under exactly the load
   this milestone is about.

Fixed by arming before any `schedule()` and clamping the interval to
the range the LAPIC is actually programmed within before banking it.
This is invisible to a program that only ever blocks forever (which is
why .NET, whose engine passes `timeout = -1`, hit bug 1 and not this
one) and fatal to anything using a real timeout.

### Regression tests

- `make epollet` — `userland/epollet.c`, 8 EPOLLET conformance cases on
  a pipe and a socketpair, no network and no host driver. Case 1 pins
  level-triggered behaviour down so the ET work cannot quietly change
  it; case 8 is the drain-and-refill-between-waits case above.
- `make epolltcp EPOLLTCP_CONC=8` — the LT oracle, unchanged, still
  16/16.
- `./tools/gauntlet.sh` — 15/15.

### Also still true regardless

`TCP_MAX_CONNS` is **32** statically-allocated ~76KB TCBs (32KB send +
32KB receive + reassembly). Concurrency 16 passes with room to spare --
each connection closes and its slot is reaped on demand -- but 32 is
still the ceiling on connections IN FLIGHT, which is far too few for a
web server, and the buffers should become dynamically allocated rather
than the count merely raised.

### Kernel fixes this milestone (all landed)

| area | fix |
|---|---|
| `kernel/net/socket.c` | `socket_create` compared the RAW `type` argument against `SOCK_STREAM`, so `socket(AF_INET, SOCK_STREAM\|SOCK_CLOEXEC, IPPROTO_TCP)` -- what .NET issues for every socket -- returned `-EPROTONOSUPPORT`. Linux masks `SOCK_NONBLOCK`/`SOCK_CLOEXEC` off first. Now masked, `SOCK_NONBLOCK` honoured on the new fd, `SOCK_CLOEXEC` accepted and ignored (NeoOS has no `FD_CLOEXEC`). **This alone is what took the app from answering nothing to serving HTTP.** |
| `kernel/ipc/futex.c` | `FUTEX_REQUEUE`/`FUTEX_CMP_REQUEUE` were `-ENOSYS`. musl's `pthread_cond_timedwait` releases its internal lock with `futex(l, FUTEX_REQUEUE, 0, 1, r)` and only checks the result to fall back between the private and shared forms -- so `-ENOSYS` from both meant a thread blocked in the condvar's own lock was never woken at all. NeoOS wakes the waiters rather than moving them (see the DIVERGENCE note in `futex.c`). |
| `kernel/net/tcp.c` | A connection completing its handshake when the listener's accept queue was full was left ESTABLISHED and never queued -- a black hole the peer could not tell from a hung server. It is reset now. `TCP_BACKLOG_MAX` 8 -> 128, and `listen()`'s own backlog is honoured. |
| `kernel/net/tcp.c` | `tcp_alloc` refused new connections while dead slots waited on a 100Hz timer to be collected; under load 30 of 32 slots were already finished. It now reaps a finished slot on demand (`reap_dead_locked`), preferring CLOSED-and-socket-gone and falling back to the longest-settled TIME_WAIT. |
| `kernel/net/socket.c` | `accept4`'s `SOCK_NONBLOCK` applies to the accept AND the accepted socket, where Linux applies it only to the accepted socket. Recorded as a divergence. |

Note also that the shim mappings for `sched_setaffinity` and friends
already existed in `third_party/shim/`; the ENOSYS 203 hang was a
**stale `neoos-musl/upstream` tree**. Rebuild musl (`build.sh` with
`KERNEL_SHIM_DIR` pointing at this repo) before concluding a syscall is
missing.

## Test app publish recipe

```
PATH=$HOME/opt/cross-x86_64-neoos/bin:$PATH \
dotnet publish -c Release -r linux-musl-x64 -p:PublishAot=true \
  -p:ServerGarbageCollection=false -p:InvariantGlobalization=true \
  -p:CppCompilerAndLinker=x86_64-neoos-linux-musl-gcc \
  -p:ObjCopyName=x86_64-neoos-linux-musl-objcopy \
  -p:StaticExecutable=true -p:PositionIndependentExecutable=false
```
csproj carries `<ExtraLinkerArg Include="-T,.../userland/user.ld" />` and
`<ExtraLinkerArg Include="--no-relax" />`.

One gotcha with the .NET 10 ILCompiler: its link step always appends
clang's `--target=x86_64-linux-musl`, which the NeoOS cross **gcc**
rejects, and the `TargetTriple` property it comes from is set inside
the targets file (so `-p:TargetTriple=` on the command line does not
clear it). Point `CppCompilerAndLinker` at a one-line wrapper that
drops any `--target=*` argument and `exec`s the real
`x86_64-neoos-linux-musl-gcc`.
