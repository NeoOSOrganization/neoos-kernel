# ASP.NET Core on NeoOS — status and remaining gaps

**Milestone reached (2026-09-07): ASP.NET Core / Kestrel serves HTTP on
NeoOS.** A `WebApplication.CreateSlimBuilder` app with a `MapGet("/")`
handler, built NativeAOT for `linux-musl-x64` against the
`~/opt/cross-x86_64-neoos` toolchain, boots on NeoOS, binds
`0.0.0.0:30000`, and answers real requests: `curl` from the host (via
QEMU `hostfwd`) gets `HTTP/1.1 200 OK` with the handler's body. Verified
**40/40 sequential requests** and concurrency 2 and 4. Concurrency 8
under sustained load is not yet stable — see "Known remaining issue".

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

## Known remaining issue — concurrent request handling

**Status as of 2026-09-08: root-caused, fix not yet written.** Four
kernel bugs blocking Kestrel were found and fixed (below), and ASP.NET
Core now serves **40/40 sequential requests** and passes at concurrency
2 and 4. At concurrency 8 under sustained load it stops answering, and
the cause is now known: **NeoOS's epoll is level-triggered and .NET
registers EPOLLET.** See the section below.

The failure is a HANG, not the `AccessViolation` this section used to
describe — no `[usrflt]`, no `[fault-audit]`, nothing in the kernel log.
The GC-corruption theory is superseded: the original AccessViolation was
the mm frame-exhaustion bug root-caused in the concurrent-request-crash
spec (thread stacks were eagerly committing 8MiB each), which is fixed.

What the kernel sees at the point it stops: **24 of the 32 TCBs sitting
in `CLOSE_WAIT` with their socket still open, and `tcp_close` never
called.** CLOSE_WAIT means the peer sent FIN, NeoOS ACKed, and the
application has not closed its end. So Kestrel is accepting connections
and then neither serving nor closing them — its `SocketAsyncEngine` is
not draining what it accepted. `userland/epolltcp.c` (the .NET-free
oracle, `make epolltcp`) does the same epoll accept/serve/close cycle at
concurrency 8 and passes 16/16, which puts this above the kernel's epoll
layer.

### ROOT CAUSE (confirmed 2026-09-08): NeoOS's epoll is level-triggered,
### .NET's SocketAsyncEngine is edge-triggered

Instrumenting `epoll_ctl_do` to log the event mask userland registers
gives, for every socket .NET adds to its engine's epoll set:

```
[ep-dbg] ctl op=1 events=0x80000005
```

`0x80000005` = `EPOLLET (0x80000000) | EPOLLOUT (0x4) | EPOLLIN (0x1)`.
So **.NET asks for EDGE-TRIGGERED notification**, and NeoOS's epoll is
level-triggered only (`kernel/sync/epoll.h` says so explicitly). Worse,
the flag is not merely ignored, it is silently *erased*:
`epoll_wait_core` copies the registration into a `struct pollfd` with

```c
pfd[i].events = (short)e->events;     /* 0x80000005 -> 0x0005 */
```

a 32-bit mask truncated into a 16-bit field, so `EPOLLET` cannot even be
seen by the code that would honour it.

**Why that hangs Kestrel rather than merely being inefficient.** The
usual intuition -- "level-triggered delivers a superset of edge-
triggered, so it is safe" -- is wrong for a consumer *written against*
ET. `SocketAsyncEngine`'s loop is:

1. `epoll_wait` returns fd X readable
2. hand X to the thread pool as a work item
3. go straight back to `epoll_wait`

Under ET, step 3 blocks until *new* data arrives. Under LT, X is *still*
readable (nobody has read it yet -- the thread-pool item has not run),
so `epoll_wait` returns X again immediately, and again, and again. The
engine thread spins dispatching duplicate work items for the same socket
as fast as it can, the thread pool fills with them, and the connections
that were accepted are never actually serviced or closed.

That is exactly the state the kernel observes at the stall: 24 of 32
TCBs in `CLOSE_WAIT` with the socket still open and `tcp_close` never
called, 6 more `ESTABLISHED`, and zero request handlers invoked.

It also explains every other data point:
- **Sequential requests work** (40/40): with one connection in flight
  the duplicate dispatches are harmless -- the work item runs, drains
  the socket, and the level goes away.
- **Concurrency 2 and 4 work**; 8 does not. More sockets simultaneously
  readable means more duplicate dispatch, and the pool has a fixed size.
- **`userland/epolltcp.c` passes 16/16 at concurrency 8** -- it is
  written for level-triggered and drains each fd in the same loop
  iteration it is reported in, so LT is correct for it. The oracle
  cleared the kernel's epoll layer because the bug is not in delivery,
  it is in the *semantics* the caller asked for and did not get.

### The fix

Implement `EPOLLET` in `kernel/sync/epoll.c` + `kernel/syscall/sys_poll.c`:

1. Stop truncating. `struct epoll_entry.events` is already `uint32_t`;
   the `(short)` cast into `pfd[i].events` is where `EPOLLET` dies.
   Either carry the ET bit alongside the `pollfd` array or stop routing
   epoll through `struct pollfd` for this.
2. Per registration, remember the last reported ready mask
   (`e->last_ready`). On a scan, report only
   `now_ready & ~e->last_ready` -- the *edge* -- and store
   `e->last_ready = now_ready`. When an fd polls not-ready, clear it so
   the next transition re-arms.
3. `EPOLL_CTL_MOD` and a re-`ADD` after close must reset `last_ready`,
   or a reused fd number inherits a stale edge.

Until then, a workaround that would confirm the diagnosis without
kernel work: force .NET onto its blocking-socket path (it has one) or
run Kestrel with a single I/O thread.

### Also still true regardless

`TCP_MAX_CONNS` is **32** statically-allocated ~76KB TCBs (32KB send +
32KB receive + reassembly). That is far too few for a web server
whatever else is fixed, and the buffers should become dynamically
allocated rather than the count merely raised.

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
