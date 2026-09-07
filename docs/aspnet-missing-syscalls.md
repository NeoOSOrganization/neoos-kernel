# ASP.NET Core on NeoOS — status and remaining gaps

**Milestone reached (2026-09-07): ASP.NET Core / Kestrel serves HTTP on
NeoOS.** A `WebApplication.CreateSlimBuilder` app with a `MapGet("/")`
handler, built NativeAOT for `linux-musl-x64` against the
`~/opt/cross-x86_64-neoos` toolchain, boots on NeoOS, binds
`0.0.0.0:30000`, and answers real requests: `curl` from the host (via
QEMU `hostfwd`) gets `HTTP/1.1 200 OK` with the handler's body. Verified
**50/50 sequential requests**. Concurrent request handling is not yet
stable — see "Known remaining issue" below.

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

Under 3+ simultaneous in-flight requests, the .NET process either hangs
or hits an `AccessViolation` at a fixed address inside
`ConcurrentQueueSegment<SocketAsyncEngine.SocketIOEvent>::.ctor` — i.e.
a `new Slot[N]` GC allocation returns an **unmapped** address (`err=0x6`
write to non-present page in the `0x5000…` mmap range). This is GC-heap
corruption that only appears once several .NET threadpool threads are
allocating and handling sockets in parallel — the most concurrent
multithreaded workload NeoOS has ever run.

Not yet root-caused. Candidates, roughly in order:
- a pre-existing NeoOS `mm` race (concurrent `mmap`/`munmap`/COW from
  multiple threads of one process)
- a scheduler / TLS / `fs_base` issue under many threads (this area has
  had several recent fixes — see `git log`)
- the epoll `epoll_forget_fd` ↔ `epoll_wait` snapshot window handing a
  just-closed connection's stale `data` pointer to .NET's engine
  (partially mitigated: `epoll_wait` now drops `POLLNVAL` entries)

Sequential HTTP serving is solid; this is the next thing to chase for a
real Kestrel workload.

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
