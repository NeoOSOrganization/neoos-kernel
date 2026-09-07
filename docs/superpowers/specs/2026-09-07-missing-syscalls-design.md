# Missing Syscalls — Catalogue and Fill-In Plan

## Why

NeoOS today: **111** native syscalls (`kernel/syscall/syscall_nr.h`),
**97** Linux→NeoOS mappings in the musl shim
(`third_party/shim/neoos_syscall.c`). Everything unmapped returns
`-ENOSYS` and the shim prints `[shim] ENOSYS <linux-nr>` once per
distinct number (`neo_report_enosys`) — that print is the primary
discovery tool and this plan is essentially "work the list it
produces."

The ASP.NET Core bring-up
(`docs/aspnet-missing-syscalls.md`) surfaced the first batch. This
spec catalogues everything a modern Linux userland — musl,
NativeAOT/.NET, Go, Rust, Node, Python, and the coreutils/busybox
line — actually reaches for, groups it by what it unblocks, and gives
each entry a shape.

## Ground rules (from `CLAUDE.md`)

- **Translation, not emulation.** The shim maps a Linux number onto a
  NeoOS primitive. If a primitive doesn't exist, it goes in the
  *kernel*, Linux-SHAPED, under NeoOS's own number — not faked in the
  shim.
- Struct layouts / flag values / errno numbers / semantics that
  cross into userland match Linux x86_64 exactly.
- Every addition updates `docs/stdlib.md` (the function, or the
  divergence) and, at milestone end, `docs/abi-compatibility.md`.
- Boot-verified in QEMU; each group gets a musl-linked C selftest
  (`userland/`-style) plus, where a real app exercises it, a
  re-run of that app (BusyBox gauntlet, the .NET tests).

---

## Tier 0 — actively blocking a known workload

### `sched_setaffinity` (Linux 203) — **blocks .NET Server GC**

Kernel has `SYS_SCHED_GETAFFINITY` (93, mapped from Linux 204); no
setaffinity. .NET Server GC pins one heap thread per CPU with
`sched_setaffinity` and **hangs before `Main`** on ENOSYS. Current
workaround: publish web apps with `-p:ServerGarbageCollection=false`.

- New `SYS_SCHED_SETAFFINITY`. Args: `pid`, `cpusetsize`, `mask*`.
- Semantics: validate the mask is non-empty and intersects the set
  of online CPUs (else `-EINVAL`); store it on `struct thread`
  (`cpus_allowed` — the field the SCH-2 balancer will need anyway);
  if the caller's current CPU is no longer allowed, migrate it now.
- **Divergence to record** (`docs/stdlib.md`): until the advanced
  scheduler's balancer (SCH-2) exists, affinity is *recorded and
  honoured at placement/steal time* but there is no active push —
  the guarantee is "eventually runs only on allowed CPUs," and it is
  exact once SCH-2 lands. A mask that includes ≥1 online CPU always
  succeeds even if NeoOS's steal logic is coarse.
- Also finish `getcpu` (Linux 309 → existing `SYS_GETCPU` 41):
  one-line shim `case`. Kills the second-most-common `[shim] ENOSYS`.
- `prctl` (Linux 157): accept the no-op-safe ops
  (`PR_SET_NAME`/`PR_GET_NAME` → store on `struct thread`,
  surface in `/proc/[pid]/comm` and `/proc/[pid]/task/*/comm`;
  `PR_SET_THP_DISABLE`, `PR_SET_DUMPABLE`, `PR_CAPBSET_*`,
  `PR_SET_PDEATHSIG`, `PR_SET_CHILD_SUBREAPER` → store or accept);
  `-EINVAL` for the rest. `PR_SET_SECCOMP` explicitly unsupported.

### `eventfd2` (Linux 290) — **suspected in the concurrent-request path**

.NET's `SocketAsyncEngine` and many event loops (libuv/Node,
tokio, Go's netpoller fallback) use an `eventfd` as the "wake the
poll loop" primitive. NeoOS has none.

- New `SYS_EVENTFD2(initval, flags)` → an fd backed by a 64-bit
  counter. `read` returns+clears the counter (or the value 1 in
  `EFD_SEMAPHORE` mode) or blocks if zero; `write` adds; `poll`
  reports `POLLIN` when non-zero, `POLLOUT` when a write of 1
  wouldn't overflow. Flags: `EFD_CLOEXEC`, `EFD_NONBLOCK`,
  `EFD_SEMAPHORE`.
- Integrates with the poll broadcast the same way pipes do
  (`kernel/ipc/pipe.c` is the model — a counter object with a
  `poll_head`).
- Shim maps `eventfd` (284) → `eventfd2(initval, 0)`.

---

## Tier 1 — modern event/IO plumbing (unblocks most server runtimes)

### `timerfd_*` (Linux 283/286/287)

`timerfd_create`, `timerfd_settime`, `timerfd_gettime`. .NET's
`Threading.Timer` on Linux, Go's timer subsystem, and any
`epoll`-based scheduler use a timerfd rather than `SIGALRM`.

- Backed by the same timeout machinery as `nanosleep`/`sleep_deadline`
  (`struct thread.sleep_deadline` + `timeout_next` list, or a
  per-object timer). fd object with a counter of expirations,
  `poll`-able. `CLOCK_MONOTONIC` and `CLOCK_REALTIME`, `TFD_TIMER_ABSTIME`.

### `signalfd4` (Linux 289)

`signalfd` — read pending signals as `struct signalfd_siginfo` from
an fd instead of a handler. Used by `systemd`, by Go's os/signal on
some paths, by anything that wants signals in its event loop. Sits on
`kernel/ipc/signal.c`'s pending-signal queue; `poll` reports `POLLIN`
when a signal in the fd's mask is pending.

### `pidfd_open` / `pidfd_send_signal` / `pidfd_getfd` (Linux 434/424/438) + `P_PIDFD` for `waitid`

Race-free process management. `pidfd_open(pid)` → an fd that
`poll`s readable when the process exits; `pidfd_send_signal`;
`waitid(P_PIDFD, ...)`. Go 1.23+, modern Rust `std::process`,
container tooling. NeoOS's `lib/` already has wait-by-pid; a pidfd
is the fd-shaped version and composes with epoll.

### `ppoll` (Linux 271) / `pselect6` (Linux 270) / `epoll_pwait2` (Linux 441)

The atomic-sigmask-swap variants. NeoOS has `poll`/`select`/
`epoll_pwait` but ignores the sigmask (documented divergence). Real
`ppoll` needs the per-call mask swap — which needs the same
mechanism `rt_sigsuspend` already has (`docs/stdlib.md` notes this).
Also `ppoll` takes a `struct timespec` (ns) not `int ms`.

### `close_range` (Linux 436), `dup3` already present

`close_range(first, last, flags)` — bulk fd close, used by every
modern `posix_spawn`/exec path (musl's `__libc_start_main` cleanup,
Go, Python 3.9+). Cheap: loop `fd_table_close`.

### `pipe2` — present; audit `O_DIRECT`/`O_NOTIFICATION_PIPE` (ignore, document)

---

## Tier 2 — filesystem & metadata (unblocks builds, package managers, DBs)

### `statx` (Linux 332)

The extended stat. `git`, `rustup`, `cargo`, coreutils ≥ 9, and
glibc/musl's own `stat` fast paths use it. Returns `struct statx`
(birth time, `stx_mask`, `stx_attributes`). Map from NeoOS's `stat`
data; fill `STATX_BASIC_STATS`, leave btime unset unless the FS has
it.

### `*at` family completeness

Present: `openat`(via OPEN?), `newfstatat`. Missing and commonly
needed: `mkdirat`, `unlinkat` (with `AT_REMOVEDIR`), `renameat2`
(Linux 316 — `RENAME_NOREPLACE`/`RENAME_EXCHANGE`, used by
`sqlite`, `dpkg`, atomic-rename patterns), `linkat`, `symlinkat`,
`readlinkat` (have `readlink`), `fchmodat`, `fchownat`, `utimensat`
(Linux 280 — mtime/atime set, used by `tar`, `cp -p`, `make`),
`faccessat2` (Linux 439), `fstatat` variants.

### `fallocate` (Linux 285), `ftruncate` (present?), `fdatasync`/`fsync`/`sync_file_range`

`fallocate` for preallocating DB/log files (SQLite, PostgreSQL,
.NET `FileStream` with `preallocationSize`). `fsync`/`fdatasync`
must actually flush the block cache (`kernel/fs/blkcache.c`) or
databases lose durability guarantees silently.

### `copy_file_range` (Linux 326)

In-kernel file copy. coreutils `cp`, Go's `io.Copy` fast path,
container image extraction. Can start as a read/write loop in the
kernel (still saves the user↔kernel bounce) and become a
block-cache-to-block-cache copy later.

### `flock` (Linux 73) / `fcntl(F_SETLK/F_OFD_SETLK)`

Advisory file locking. `fcntl` is present but likely `F_GETFL/SETFL`
only (per `syscall_nr.h:89`). SQLite, `dpkg`, `flock(1)`,
build-system lockfiles all need at least OFD locks
(`F_OFD_SETLK`/`GETLK`/`SETLKW`).

### `inotify` — present (`init1`/`add_watch`/`rm_watch`); `fanotify` — skip

---

## Tier 3 — process / namespace / credentials

### `clone3` (Linux 435)

The modern `clone`. Go 1.21+, glibc ≥ 2.34 `pthread_create` on some
configs, `posix_spawn`. Superset of `clone`'s flag handling with a
`struct clone_args`. NeoOS's `sys_clone` already exists with a
restricted flag set (`NEOOS_CLONE_FLAGS_SUPPORTED`); `clone3` is a
new entry that parses `clone_args` and forwards. `CLONE_INTO_CGROUP`
depends on SCH-5.

### `setresuid`/`setresgid`/`getresuid`/`getresgid` (117/119/118/120), `setgroups`/`getgroups` (116/115)

Present: `setuid`/`setgid`/`getuid`/`geteuid`/`getgid`/`getegid`.
Missing the res* and groups calls — `su`, `sudo`, `sshd`, any
privilege-dropping daemon needs the full set. Also `setfsuid`/
`setfsgid` (122/123 — legacy, accept as alias of setuid/setgid).

### `capget`/`capset` (125/126), `prctl(PR_CAP*)`

A capability model. Needed properly for containers; can start as
"uid 0 has everything, everyone else has nothing" with the syscalls
returning consistent data.

### `unshare` (272) / `setns` (308) / namespace `CLONE_NEW*`

Namespaces are a large piece — mount, PID, net, user, UTS, IPC,
cgroup, time. Out of scope for a single milestone; gets its own
spec. But `unshare(CLONE_FILES)` / `unshare(0)` and
`unshare(CLONE_FS)` are cheap and unblock some `posix_spawn` and
sandbox libraries — do those two now, defer the rest.

### `prlimit64` (Linux 302), `getrlimit`/`setrlimit`

Present? `getrusage` is. `RLIMIT_NOFILE` (fd table already has a
cap — expose it), `RLIMIT_STACK`, `RLIMIT_AS`, `RLIMIT_CORE`,
`RLIMIT_NPROC`. .NET reads `RLIMIT_NOFILE` at startup;
`ulimit` needs setrlimit.

### `sched_setattr`/`sched_getattr` (314/315), `sched_getscheduler`/`setscheduler` (144/145), `sched_get_priority_min/max` (160/161), `sched_rr_get_interval` (148), `setpriority`/`getpriority` (141/140)

The scheduler ABI — **delivered by the advanced-scheduler spec**
(SCH-1 for nice/NORMAL/BATCH/IDLE/setattr; SCH-3 for FIFO/RR; SCH-4
for DEADLINE). Listed here for cross-reference; not duplicated.

---

## Tier 4 — IO performance (nice-to-have, big wins for specific apps)

### `sendfile` (Linux 40), `splice` (275), `tee` (276), `vmsplice` (278)

Zero-copy file→socket. Kestrel's static-file middleware, nginx-style
servers, Go's `http.ServeContent`. `sendfile` first (read-loop
implementation is fine and already helps); real `splice` needs pipe
buffers to be page-granular and shareable — larger.

### `io_uring` (425/426/427) — **explicitly deferred**

A whole subsystem. Modern .NET (`System.Net.Sockets` on `io_uring`
when available), Go (planned), and every high-perf server want it.
NeoOS should get it *eventually* but it is its own multi-milestone
spec and epoll+eventfd covers the current needs. Programs that probe
for `io_uring_setup` and fall back to epoll (which is all of them
today) work fine without it.

### `preadv2`/`pwritev2` (327/328), `pread64`/`pwrite64` (17/18)

Positioned vector IO. `pread`/`pwrite` (non-`v`) are table stakes for
databases and .NET `RandomAccess` — check whether present; if not,
Tier 2.

---

## Tier 5 — time & misc

- `clock_nanosleep` (Linux 230) — absolute-deadline sleep
  (`TIMER_ABSTIME`). .NET, Go, and musl's `pthread_cond_timedwait`
  want it. NeoOS has relative `nanosleep`; add the abs variant on
  the same deadline machinery.
- `clock_getres` (229), `clock_settime` (227), `gettimeofday`
  (96 — usually vDSO, but a real syscall fallback exists),
  `adjtimex`/`clock_adjtime` (skip — no NTP).
- `getrandom` — present. `sysinfo` — present.
- `sysconf`-backing bits: `sched_getaffinity` (have), `getcpu`
  (Tier 0), `_SC_NPROCESSORS_ONLN` (have `SYS_CPU_COUNT`).
- `membarrier` — present; audit the command set
  (`MEMBARRIER_CMD_PRIVATE_EXPEDITED*` is what .NET uses — confirm
  it's the mapped one).
- `rseq` (Linux 334) — restartable sequences. glibc ≥ 2.35 registers
  one unconditionally at thread start; **musl does not**, so NeoOS
  can return `-ENOSYS` and musl-linked programs (which is all of
  NeoOS's) are unaffected. Document as a deliberate non-target.
- `set_robust_list`/`get_robust_list` (273/274) — robust futexes.
  glibc `pthread_mutexattr_setrobust`. musl handles robustness
  differently; low priority. Accept `set_robust_list` as a no-op
  store so programs that call it unconditionally don't fail.
- `getdents64` — present. `getdents` (non-64) — add as an alias for
  old binaries.
- `umask` (95), `chmod`/`fchmod` (90/91), `chown`/`fchown`/`lchown`
  (92/93/94), `access`/`faccessat` — check which are present; the
  gaps here bite `install`, `cp -p`, `tar -x`.

---

## Delivery plan

| batch | contents | unblocks | effort |
|---|---|---|---|
| **MSC-1** | `sched_setaffinity`, `getcpu` shim, `prctl` no-op ops, `eventfd2` | .NET Server GC; the SocketAsyncEngine wake path (concurrent-request investigation depends on this) | small |
| **MSC-2** | `timerfd_*`, `signalfd4`, `close_range`, `clock_nanosleep`, `ppoll` | Go/Node/.NET event loops, `posix_spawn` | medium |
| **MSC-3** | `*at` completeness, `statx`, `utimensat`, `renameat2`, `fallocate`, `fsync`/`fdatasync` real flush, OFD `fcntl` locks | git, cargo, dpkg, SQLite, tar, make | medium |
| **MSC-4** | `pidfd_*`, `clone3`, `setres[ug]id`/`getgroups`/`setgroups`, `prlimit64`/`getrlimit`/`setrlimit`, `capget`/`capset` (root-all model) | su/sudo/sshd, modern process libs | medium |
| **MSC-5** | `sendfile`, `copy_file_range`, `pread64`/`pwrite64`/`preadv2`, `flock` | static-file serving, DB engines | medium |
| **later** | `unshare`/`setns`/namespaces, `io_uring`, `fanotify`, full `splice` | containers, top-tier IO perf | own specs |

MSC-1 is the immediate one — it directly feeds both the advanced
scheduler (`cpus_allowed` field, `getcpu`) and the concurrent-request
crash investigation (`eventfd2` is a prime suspect in .NET's socket
engine, and Server-GC support means the crash can be reproduced with
the *default* publish config instead of a workstation-GC workaround).

## Testing

- One `userland/` C selftest per batch, musl-linked, exercising each
  new call's success + documented-error paths, emitting a
  `[msc-N] … PASSED` marker wired into the gauntlet's
  `required_markers` (via a `.test.json` `boot_entries` +
  `required_markers`, the pattern `tools/gen-embedfs.py` already
  supports).
- Re-run the BusyBox gauntlet and the .NET hello/tcp/thread/gc/web
  tests after each batch; the `[shim] ENOSYS` count should
  monotonically drop and is asserted in the selftest.
- `docs/abi-compatibility.md` refreshed at each batch end with the
  new "implemented / stubbed / diverges" rows.
