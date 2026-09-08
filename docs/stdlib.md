# NeoOS Standard Library Reference

Every exported function in `libneoos.a`, grouped by header. Per
`/CLAUDE.md`'s standard-library convention: any kernel feature that
becomes usable by a user-mode program must come with an update here
alongside the library code that exposes it.

## `<unistd.h>`

- `void exit(int code)` — terminates the calling process with the
  given exit code, including every other thread in it. Threads blocked
  in a syscall are interrupted rather than left running. Never returns.
  Contrast `thread_exit`, which ends only the calling thread.
- `int64_t write(int fd, const void *buf, uint64_t len)` — writes
  `len` bytes from `buf` to the file (or console, for fd
  `STDOUT_FILENO`/`STDERR_FILENO`) open on `fd`. Returns the number of
  bytes written, or a negative `<errno.h>` code on failure.
- `int64_t read(int fd, void *buf, uint64_t len)` — reads up to `len`
  bytes from the file (or console, for fd `STDIN_FILENO`, which always
  returns 0 -- there is no keyboard-to-process input path yet) open on
  `fd` into `buf`. Returns the number of bytes actually read (0 at
  EOF), or a negative `<errno.h>` code on failure.
- `int close(int fd)` — closes `fd`. Returns 0, or a negative
  `<errno.h>` code on failure.
- `int64_t lseek(int fd, int64_t offset, int whence)` — moves `fd`'s
  read/write position. `whence` is `SEEK_SET`/`SEEK_CUR`/`SEEK_END`.
  Returns the new absolute position, or a negative `<errno.h>` code on
  failure. Writing past the current end of file (via a forward
  `lseek`) zero-fills the gap with real allocated bytes, not a logical
  sparse hole.
- `int chdir(const char *path)` — sets the calling process's working
  directory. `path` may itself be relative, and is resolved against the
  current one. Returns 0, `-ENOENT` if it does not exist, `-ENOTDIR` if
  it is not a directory, or `-ENAMETOOLONG`. A failed `chdir` leaves the
  working directory unchanged.
- `char *getcwd(char *buf, uint64_t size)` — writes the working
  directory, always absolute and always canonical, into `buf`. Returns
  `buf`, or `NULL` if `size` is too small for the path plus its NUL.
  The raw syscall underneath returns Linux's length-including-NUL, or
  `-ERANGE`; the `NULL` translation is library code, as in musl.
- `int getpid(void)` — returns the calling process's PID.
- `void yield(void)` — voluntarily gives up the CPU. Since SCH-1 this
  is a real EEVDF yield (the caller is charged a full slice of virtual
  time and drops behind every other runnable task), not just a
  reschedule. `sched_yield()` from `<sched.h>` maps to the same thing.
- `int spawn(const char *path)` — builds a fresh process directly from
  the ELF executable at `path` (NUL-terminated) and returns its PID,
  or `-1` on failure. NeoOS-specific: not `fork`+`exec`. The child
  **inherits the caller's open file descriptors**, as `posix_spawn` and
  `fork` do; only the first process, which has no spawner, is given
  fresh `/dev/console` streams.
- `int wait(int pid)` — blocks until the process with the given PID
  exits, reaps it, and returns its exit code. NeoOS-specific: takes
  one specific PID, not "any child".
- `int fork(void)` — duplicates the calling process. Returns `0` in
  the child, the child's PID in the parent, or `-1` on failure (parent
  unaffected). Each side's open file descriptors are independent
  copies after this call -- reads/writes/`lseek`s on inherited fds no
  longer share a position between parent and child (unlike POSIX,
  which shares one underlying open-file description).
  NeoOS-specific simplification.
- `int exec(const char *path)`, `int execv(const char *path, char *const argv[])`,
  `int execve(const char *path, char *const argv[], char *const envp[])`
  — replace the calling process's
  address space with the ELF executable at `path`. Open file
  descriptors, PID, and parent are preserved. On success, never
  returns. Returns `-1` on failure (bad path, out of memory), leaving
  the calling process completely unchanged and still running its
  original code — *including* its other threads, which are terminated
  only once the new image has been built successfully. As in Linux
  `execve`, the new program starts as the single remaining thread of
  the process; every sibling is terminated and waited for before the
  old address space is released.
- `int mount(const char *source, const char *target, const char *fstype)`
  — mounts a filesystem at `target`. `fstype` is `"fat"`, `"ramfs"`, or
  `"devfs"`; `source` is `"hd0"` or `"hd1"` for `"fat"` and ignored
  otherwise. FAT16 versus FAT32 is auto-detected from the volume's
  cluster count. Returns 0, or `-ENODEV`, `-EEXIST`, or `-ENOSPC`.
- `int umount(const char *target)` — unmounts the filesystem at
  `target`. Returns 0, `-ENOENT` if nothing is mounted there, or
  `-EBUSY` if any file on it is still open. The mount is left
  completely intact on `-EBUSY`.
- `int mkdir(const char *path)` — creates a new, empty directory.
  Returns 0, or a negative `<errno.h>` code on failure.
- `int unlink(const char *path)` — deletes the file at `path`. Returns
  0, or a negative `<errno.h>` code on failure (including `-EISDIR` if
  `path` is a directory; there is no `rmdir`).
- `STDIN_FILENO`/`STDOUT_FILENO`/`STDERR_FILENO` (0/1/2) and
  `SEEK_SET`/`SEEK_CUR`/`SEEK_END` (0/1/2) constants. The three
  standard streams are ordinary file descriptors open on
  `/dev/CONSOLE`, not special-cased numbers: they can be `close`d, and
  a later `open` may reuse the slot.

## `<fcntl.h>`

- `int open(const char *path, int flags)` — opens (or, with
  `O_CREAT`, creates) the file at `path`. Returns a file descriptor,
  or a negative `<errno.h>` code on failure. The lowest free
  descriptor at or above 3 is returned; a process may hold up to
  16,384 descriptors at once.
- `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`
  flag constants.

## `<dirent.h>`

- `DIR *opendir(const char *path)` — opens a directory for reading.
  Returns `0` on failure (path missing, not a directory, or more than
  four directories already open).
- `struct dirent *readdir(DIR *d)` — returns the next entry, or `0` at
  end of directory. The pointer is **into the `DIR`'s own buffer** and
  is invalidated by the next `readdir` or `closedir` on that `DIR`.
- `int closedir(DIR *d)` — closes the directory. Returns 0, or a
  negative `<errno.h>` code.
- `int getdents(int fd, void *buf, int bytes)` — the raw syscall.
  Fills `buf` with as many complete records as fit, returning the
  number of **bytes** written, `0` at end of directory, `-EINVAL` if
  the buffer cannot hold even one record, or `-ENOTDIR`.

`struct dirent` is **Linux's `getdents64` record**: `d_ino` (8),
`d_off` (8), `d_reclen` (2), `d_type` (1), then a NUL-terminated
`d_name` at offset 19, the whole record padded so the next starts
8-byte aligned. `d_type` uses Linux's values — `DT_REG` 8, `DT_DIR` 4,
`DT_CHR` 2.

Records are **variable length**, which is why `getdents` counts bytes
rather than entries and why a caller walks the buffer by adding
`d_reclen`. This is exactly the shape a shim cannot fake: without
`d_reclen` there is nothing to step by.

`d_ino` is the same inode `stat` reports for the same file, so pairing
`readdir` with `stat` sees one file, not two.

### Note: `d_name` is bounded by the filesystem, not by the struct

The declared `d_name[256]` is a ceiling so callers can hold a `struct
dirent` by value. The kernel writes only as many bytes as the name
needs, up to `VFS_NAME_MAX` (256 — a VFAT long name is at most 255
characters plus a NUL). On the FAT volumes a short 8.3 name still
produces a short record; a file created or listed with a long name
carries it in full.

### Previously

Until this milestone `struct dirent` was `{ char name[13]; uint8_t
type; }` with private `DT_*` values of 1/2/3, and `getdents` counted
entries. Nothing about that matched Linux, and every field and constant
listed above changed. Any code using `e->name`/`e->type` becomes
`e->d_name`/`e->d_type`.

## `<thread.h>`

- `int thread_create(thread_t *out, void (*fn)(void *), void *arg)` —
  starts `fn(arg)` on a new thread sharing this process's address space
  and file descriptors. Returns 0 and stores the tid in `*out`, or
  `-EAGAIN` if the process already has 16 threads. Each thread gets its
  own 16KiB user stack with an unmapped guard page below it, so a stack
  overflow faults instead of corrupting a neighbouring thread's stack.
- `void thread_exit(int code)` — ends the calling thread. When it is
  the last thread of the process, the process ends too. Never returns.
- `int thread_join(thread_t t, int *exit_code)` — waits for `t` to exit
  and stores its exit code. Returns 0, `-ESRCH` if no such thread
  exists in this process, `-EDEADLK` if `t` is the caller, or `-EINTR`
  if the calling thread was killed while waiting. Joining is the only
  way to reclaim a thread's stacks before the process exits; unjoined
  threads are reclaimed when the process is reaped.
- `thread_t thread_self(void)` — the calling thread's tid. TIDs and
  PIDs share one number space, so a tid never equals an unrelated
  process's pid. A process's first thread has `tid == pid`.

## `<signal.h>`

- `int sigaction(int sig, const struct sigaction *act, struct sigaction *old)`
  — installs a handler. `sa_flags` accepts `SA_RESTART`, `SA_ONSTACK`,
  `SA_NODEFER`, `SA_RESETHAND`, and `SA_SIGINFO`. Returns 0, or
  `-EINVAL` for `SIGKILL`/`SIGSTOP`, which can be neither caught nor
  ignored. The `SA_RESTORER` the kernel requires is filled in
  automatically — callers never see it.
- `int raise(int sig)` — sends `sig` to the calling thread.
- `int kill(int pid, int sig)` — `pid > 0` targets that process,
  `pid == 0` the caller's process group, `pid < -1` the group `-pid`,
  and `pid == -1` every process. `sig == 0` is an existence probe.
- `int tkill(int tid, int sig)` / `int tgkill(int tgid, int tid, int sig)`
  — send to one specific thread.
- `int sigprocmask(int how, const sigset_t *set, sigset_t *old)` —
  `how` is `SIG_BLOCK`/`SIG_UNBLOCK`/`SIG_SETMASK`. Masks are
  **per-thread**; dispositions are per-process. `SIGKILL` and `SIGSTOP`
  are silently dropped from any mask, as POSIX requires — not an error.
- `int sigpending(sigset_t *set)` — signals that are pending **and**
  blocked. An unblocked pending signal would already have been
  delivered.
- `int sigsuspend(const sigset_t *mask)` — atomically installs `mask`,
  waits for a signal, and restores the previous mask. Always returns
  `-EINTR`.
- `int sigaltstack(const stack_t *ss, stack_t *old)` — installs an
  alternate signal stack, used by handlers registered with
  `SA_ONSTACK`. Returns `-ENOMEM` for a stack smaller than 2048 bytes.
  This is what makes a `SIGSEGV` handler survivable after a stack
  overflow: every thread has an unmapped guard page below its stack, so
  without an alternate stack the handler re-faults on the same page.
- `sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset`, `sigismember`
  — `sigset_t` is a 64-bit mask, one bit per signal.
- Signal numbers are Linux's: `SIGHUP` 1 through `SIGSYS` 31, with
  real-time signals `SIGRTMIN` (32) to `SIGRTMAX` (64).
- **Standard signals do not queue**: repeat deliveries of a blocked
  signal collapse into one. **Real-time signals queue** with their
  payloads. One signal is delivered per return to user mode, since a
  handler blocks its own signal until it returns.
- A user-mode fault raises a signal rather than killing the machine:
  divide-by-zero raises `SIGFPE`, a bad memory access `SIGSEGV`, an
  invalid opcode `SIGILL`.

## `<sys/wait.h>`

- `int wait4(int pid, int *status, int options, void *rusage)` — the
  POSIX-shaped wait. `pid > 0` waits for that child, `pid == -1` for
  any, `pid == 0` for any in the caller's group, `pid < -1` for group
  `-pid`. `options` accepts `WNOHANG` and `WUNTRACED`. Returns the
  reaped pid, 0 for `WNOHANG` with nothing ready, or `-ECHILD`.
  `rusage` is accepted and ignored — NeoOS keeps no per-process
  resource accounting.
- `int waitpid(int pid, int *status, int options)` — `wait4` with no
  `rusage`.
- `WIFEXITED`/`WEXITSTATUS`, `WIFSIGNALED`/`WTERMSIG`,
  `WIFSTOPPED`/`WSTOPSIG`, `WIFCONTINUED`, `WCOREDUMP` — the status
  encoding matches Linux exactly.
- `int setpgid(int pid, int pgid)`, `int getpgid(int pid)`,
  `int setsid(void)`, `int getsid(int pid)` — process groups and
  sessions, inherited by `fork` and `spawn`.
- **Orphan reparenting matches Linux.** When a process exits, every
  still-alive child of it has its parent set to PID 1, and PID 1's
  `wait4(-1)` loop reaps it. There is no subreaper mechanism
  (`PR_SET_CHILD_SUBREAPER`); the reparent target is always 1.

### Divergences from POSIX

- **`wait` remains NeoOS-native** (one pid, a bare exit code) and lives
  **beside** `wait4` rather than being replaced by it. Neither
  supersedes the other.
- **A stopped orphan reparented to PID 1 is not woken.** Linux applies
  the orphaned-process-group rule (`SIGHUP` + `SIGCONT` to stopped
  members); NeoOS does not, so a process left in `SIGSTOP` when its
  parent dies stays stopped until something signals it. PID 1's plain
  `wait4(-1, …, 0)` will block on such a child indefinitely.
- **No core dumps.** `WCOREDUMP`'s bit is defined but never set.
- **No `SA_NOCLDWAIT`**, and setting `SIGCHLD` to `SIG_IGN` does not
  auto-reap children.
- **`SIGTTIN`/`SIGTTOU` and orphaned-process-group `SIGHUP` are never
  generated.** NeoOS has no controlling terminal.
- **`sigqueue` depth is a fixed pool**; exhaustion returns `-EAGAIN`.

## `<errno.h>`

Every `open`/`read`/`write`/`close`/`lseek`/`mkdir`/`unlink` call
returns its negative error code directly instead of a bare `-1` --
there is no separate settable `errno` variable. `spawn`/`wait`/
`getpid` are unaffected and keep their existing plain `-1`-on-failure
convention.

- `EPERM` (1) — the operation is not permitted on this filesystem
  (e.g. creating or deleting a node under `/dev`).
- `ESRCH` (3) — `thread_join` given a tid that is not a thread of the
  calling process.
- `EINTR` (4) — a blocking call was interrupted by a signal. With
  `SA_RESTART` the call is restarted instead of returning this.
- `ENOENT` (2) — path/file not found, or nothing mounted at a
  `umount` target.
- `EBADF` (9) — invalid or closed file descriptor.
- `ECHILD` (10) — `wait4` called with no matching child.
- `ENOMEM` (12) — `sigaltstack` given a stack smaller than 2048 bytes.
- `EAGAIN` (11) — `thread_create` when the process already has its
  maximum of 16 threads, or the kernel is out of memory for one.
- `EBUSY` (16) — `umount` called while a file on that filesystem is
  still open. The mount is left completely intact.
- `EEXIST` (17) — `mkdir`/`open(O_CREAT)` target already exists, or
  something is already mounted at a `mount` target.
- `ENODEV` (19) — `mount` given an unknown `fstype`, or a volume it
  cannot read or recognise (FAT12 is detected and rejected here).
- `ENOTDIR` (20) — a path component used as a directory isn't one.
- `EISDIR` (21) — `unlink` called on a directory, or a directory
  opened for writing.
- `EINVAL` (22) — bad argument (e.g. an `lseek` result would be
  negative, or an unrecognized `whence`).
- `ENFILE` (23) — the system-wide open-file table is full. Distinct
  from `EMFILE`, which is per-process.
- `EMFILE` (24) — the process's file descriptor table is full (16,384
  entries, of which 0/1/2 are the standard streams, so 16,381 files at
  once, maximum). The table is sparse: a process pays only for the
  512-entry blocks it actually reaches, so the ceiling costs nothing
  until it is approached.
- `EDEADLK` (35) — `thread_join` called on the calling thread itself.
- `ETIMEDOUT` (110) — a timed wait expired (`rt_sigtimedwait`).
- `ENOSPC` (28) — disk full (no free cluster), the mount table is
  full, or a FAT16 root directory is full (it has a fixed maximum
  entry count).

## `<string.h>`

- `uint64_t strlen(const char *s)`
- `void *memcpy(void *dst, const void *src, uint64_t n)`
- `void *memset(void *s, int c, uint64_t n)`
- `void *memmove(void *dst, const void *src, uint64_t n)`

## `<stdio.h>`

- `int printf(const char *fmt, ...)` — supports `%s`, `%d`, `%u`,
  `%x`, `%c`, `%%` only. No floating point, no width/precision, no
  length modifiers. Formats into a fixed internal buffer and writes it
  out via one `write()` call; there is no `FILE*`/streams concept, so
  `printf` always targets the same console `write()` does.

## CPU vector extensions

**SSE through SSE4.2 are guaranteed.** The kernel halts at boot if any
of SSE, SSE2, SSE3, SSSE3, SSE4.1 or SSE4.2 is missing, and every
program is compiled for them, so they need no runtime check. GCC's
`<xmmintrin.h>`/`<emmintrin.h>`/`<smmintrin.h>` intrinsics are
available.

**MMX is available.** Its registers alias the x87 stack, which the
kernel already preserves, so nothing special is required beyond the
usual `EMMS` discipline before returning to x87 or SSE code.

**AVX and AVX2 are available only where the CPU has them, and are not
part of the default compile flags.** NeoOS still runs on pre-AVX CPUs
(QEMU's `-cpu Nehalem` has no `XSAVE` at all), so a program that wants
AVX must:

1. be built with `-mavx -mavx2` — see the `AVXTEST.ELF` rule in the
   `Makefile`; and
2. **check `CPUID` at run time** before executing a single AVX
   instruction, and fall back or skip if absent. Executing one on a CPU
   without it raises `SIGILL`.

`userland/avxtest.c` is the worked example of both.

**All of it is saved and restored per thread automatically.** Each
thread owns an extended-state area sized from `CPUID.0Dh` — 512 bytes
where the kernel falls back to `FXSAVE`, 832 with x87+SSE+AVX enabled.
A signal handler sees its own state, and `sigreturn` restores the
interrupted one.

Two caveats worth knowing when writing AVX code that must survive a
context switch:

- **GCC emits `vzeroupper` before a call** from AVX code, which zeroes
  the upper 128 bits of every `ymm` register. Code that must keep `ymm`
  state across a `yield()` has to avoid the call, for example by
  issuing the syscall inline.
- **`syscall` returns its result in `rax`**, so hand-written inline
  syscalls must declare `rax` as an output, not just an input.

**AVX-512 is not supported**, and no interface exists to enable it.


## SMP visibility: `sysconf` and `sched_getcpu`

NeoOS runs on up to 128 CPUs (`MAX_CPUS`), and two POSIX calls expose
that to a program:

```c
#include <unistd.h>

long sysconf(int name);   // _SC_NPROCESSORS_ONLN, _SC_NPROCESSORS_CONF
int  sched_getcpu(void);  // index of the CPU the caller is running on
```

`sysconf` returns the number of CPUs that came online during boot; a
CPU the kernel failed to start is logged and excluded rather than
counted. `sched_getcpu` returns a **dense index** in `0 ..
sysconf(_SC_NPROCESSORS_ONLN) - 1`, never a Local APIC id — APIC ids
are sparse and can exceed the CPU count on real hardware.

`_SC_NPROCESSORS_CONF` and `_SC_NPROCESSORS_ONLN` are the values Linux
uses (83 and 84), so a program compiled against glibc or musl headers
sees the same constants.

### Divergences from Linux

- **`_SC_NPROCESSORS_CONF` always equals `_SC_NPROCESSORS_ONLN`.**
  NeoOS has no CPU hotplug, so there is no such thing as a configured
  but offline CPU. On Linux the two can differ.
- **No NUMA node.** Linux's `getcpu(2)` also reports a node id, and
  `sched_getcpu` is layered on it. NeoOS has no NUMA, so no node is
  reported; a port that wants one should assume node 0.
- **These are real syscalls here.** On Linux both are library code over
  the vDSO and sysfs. NeoOS answers them from the kernel under its own
  syscall numbers. This is invisible to a caller using the functions,
  and only matters to a program issuing raw syscalls.
- **`sched_getcpu` is a snapshot, not a lease.** The value can be stale
  the instant it is read, exactly as on Linux. It is fit for statistics
  and affinity hints, not for indexing per-CPU data without a lock.
- **`getcpu(2)` (Linux 309) is wired** (SCH-1): `sched_getcpu()` issues
  it directly. Only the CPU-index word is filled; the node word, if the
  caller passes one, is left as-is (assume node 0).

## Scheduling policy: `nice`, `sched_setscheduler`, `sched_setattr`, `sched_setaffinity`

The fair scheduler is EEVDF (`kernel/sched/fair.c`, spec
`docs/superpowers/specs/2026-09-07-advanced-scheduler-design.md`). The
full nice/policy/slice/affinity ABI is exposed, on NeoOS syscall
numbers the shim maps musl's Linux numbers onto:

```c
#include <sched.h>
int  sched_yield(void);
int  nice(int inc);
int  getpriority(int which, id_t who);
int  setpriority(int which, id_t who, int prio);
int  sched_getscheduler(pid_t pid);
int  sched_setscheduler(pid_t pid, int policy, const struct sched_param *);
int  sched_getparam(pid_t pid, struct sched_param *);
int  sched_setparam(pid_t pid, const struct sched_param *);
int  sched_get_priority_max(int policy);
int  sched_get_priority_min(int policy);
int  sched_rr_get_interval(pid_t pid, struct timespec *);
int  sched_setattr(pid_t pid, struct sched_attr *, unsigned int flags);
int  sched_getattr(pid_t pid, struct sched_attr *, unsigned int size, unsigned int flags);
int  sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
```

- **`sched_yield()` is a real EEVDF yield**, not a bare reschedule: the
  caller is charged a full slice of virtual time, so it drops behind
  every other runnable task before the scheduler re-picks.
- **`nice` / `setpriority` / `getpriority`** cover `PRIO_PROCESS` only.
  `PRIO_PGRP` / `PRIO_USER` return `-EPERM` (no process-group or
  per-user renice yet). `getpriority` returns `20 - nice` on the
  syscall (musl's wrapper subtracts it back), matching Linux.
- **`SCHED_NORMAL`, `SCHED_BATCH`, `SCHED_IDLE`** are accepted.
  `SCHED_BATCH` sets a "skip wake-preemption" hint; `SCHED_IDLE` drops
  the entity's weight below nice 19. **`SCHED_FIFO` / `SCHED_RR` /
  `SCHED_DEADLINE` return `-EINVAL`** — the real-time and deadline
  classes are SCH-3 / SCH-4, not yet built. This is "not yet", not
  "never": a port that hard-requires `SCHED_FIFO` will fail today.
- **`sched_get_priority_min/max`** return `0` for the fair policies
  (`1`/`99` for `SCHED_FIFO`/`RR`, so a probe sees a sane range even
  though setting those policies fails).
- **`sched_setattr` `sched_runtime`** maps to the EEVDF per-task
  **slice** (the single latency knob — NeoOS has no
  `sched_latency_ns` / `sched_min_granularity_ns`). Clamped to
  `[base/16, base*100]` around the 0.7 ms base. `sched_deadline` /
  `sched_period` are ignored (no `SCHED_DEADLINE`).
- **`sched_rr_get_interval`** reports the 0.7 ms base slice.
- **`sched_setaffinity` is recorded, not yet enforced.** The mask is
  stored on the thread (`cpus_allowed`); wake placement and the work
  stealer honour it only once the SMP balancer (SCH-2) lands. An empty
  intersection with the online set is `-EINVAL`. Affinity is capped at
  64 CPUs (one word) — a documented divergence from Linux's arbitrary
  `cpu_set_t`.
- **A renice/policy change to another thread** (not the caller) is
  recorded on its entity and applied at its next enqueue — within one
  slice for a running task, on wake for a blocked one — rather than
  reaching into another CPU's runqueue synchronously. Self-directed
  changes take effect immediately.
- **These are real syscalls**, as with `sched_getcpu`: invisible to a
  caller using the libc functions, relevant only to raw-syscall code.

### Sockets: `socket()` and `accept4()` flags

- **`SOCK_CLOEXEC` is accepted and ignored.** NeoOS has no `FD_CLOEXEC`
  at all — no descriptor is closed by `exec` — so there is nothing for
  the flag to set. Refusing it is not an option: every modern runtime
  ORs it into `socket()`'s type argument, and NeoOS used to compare the
  raw argument against `SOCK_STREAM` and answer `-EPROTONOSUPPORT`,
  which is what stopped .NET creating any socket at all.
- **`SOCK_NONBLOCK` on `accept4()` applies to the accept as well as to
  the accepted socket.** Linux applies it only to the accepted socket
  and decides whether the accept itself blocks from the LISTENER's own
  `O_NONBLOCK`. A program that sets the listener non-blocking with
  `fcntl` gets Linux's behaviour here either way; one that relies on a
  blocking `accept4(..., SOCK_NONBLOCK)` gets `-EAGAIN` instead.
- **`listen()`'s backlog is honoured, capped at 128.** A connection
  that completes its handshake with the queue full is RESET. It used to
  be left ESTABLISHED and never queued, which the peer could not tell
  from a hung server.

## Synchronisation: `<futex.h>`, `<semaphore.h>`, `<pthread.h>`

The kernel provides exactly one synchronisation primitive — Linux's
**futex** — and everything else is built on it in userland. That split
is deliberate and follows `/CLAUDE.md`'s rule: the kernel supplies a
Linux-shaped primitive, and the library does translation, not
emulation. When musl is integrated it brings its own mutexes,
condition variables and semaphores, and they land on this same futex
unchanged; the three headers below go away, the syscall does not.

### `<futex.h>`

```c
long futex(int *uaddr, int op, int val, const struct timespec *timeout);
int  futex_wait(int *uaddr, int expected);
int  futex_wait_timeout(int *uaddr, int expected, const struct timespec *rel);
int  futex_wake(int *uaddr, int count);
```

`FUTEX_WAIT` sleeps only while `*uaddr == expected`, and the comparison
happens **inside the kernel, under the same lock a wake must take**.
That is the whole point of the primitive: a value change published
between your own read and the call cannot lose the wake. It returns 0
if woken, `-EAGAIN` if the value had already changed, `-ETIMEDOUT`, or
`-EINTR`. A return of 0 does not prove your condition holds — futex
wakeups may be spurious — so every caller re-tests in a loop.

`FUTEX_WAKE` wakes up to `count` waiters and returns how many. Waking a
futex nobody is waiting on returns 0 and is not an error; every
uncontended unlock does exactly that.

The uncontended path never enters the kernel at all. A mutex lock is
one compare-exchange; these calls run only when it fails.

#### Divergences from Linux

- **`FUTEX_REQUEUE` / `FUTEX_CMP_REQUEUE` wake instead of moving.**
  Linux moves up to `val2` waiters from `uaddr`'s queue to `uaddr2`'s
  so a condvar broadcast does not wake N threads that all immediately
  block again on one mutex. NeoOS wakes them where they are: moving one
  would mean holding two futex buckets at once, and both are
  `LOCK_RANK_FUTEX`, which the rank checker refuses by design. This is
  legal rather than approximate — a requeued waiter must be prepared to
  be woken on the target anyway, and every futex user re-tests its
  condition in a loop because `FUTEX_WAIT` may return spuriously. The
  cost is the thundering herd requeue exists to avoid, a performance
  property, not a semantic one. `CMP_REQUEUE` does perform the compare
  and returns `-EAGAIN` on mismatch.

  These were `-ENOSYS` until 2026-09-08, which was NOT a benign gap:
  musl's `pthread_cond_timedwait` releases its internal lock with
  `futex(l, FUTEX_REQUEUE, 0, 1, r)` and checks the result only to fall
  back between the private and shared forms, so `-ENOSYS` from both
  left a thread blocked in that lock forever.
- **The `*_BITSET` operations, `FUTEX_WAKE_OP`, and the
  priority-inheritance futexes (`FUTEX_LOCK_PI` and friends) are
  absent.** Anything else returns `-ENOSYS`.
- **`FUTEX_PRIVATE_FLAG` is accepted and ignored.** On Linux it selects
  a cheaper process-private key. NeoOS keys *every* futex by physical
  address, which is correct for private and shared alike, so the hint
  has nothing to change. Process-shared futexes therefore work with no
  extra flag.
- **The timeout is relative, always.** Linux's `FUTEX_WAIT` is also
  relative, so this matches — but `FUTEX_WAIT_BITSET`, which Linux uses
  for absolute deadlines, does not exist here. NeoOS has no clock
  syscall to build an absolute deadline from yet.
- **Timeouts are rounded up to a 10ms tick.** The scheduler clock is
  the only time source. A 1µs timeout sleeps for one tick.
- **A futex on a copy-on-write page breaks the sharing.** The kernel
  resolves the physical address through `user_range_writable`, which
  un-shares the page. After `fork`, parent and child therefore have
  *different* futexes at the same address — which is what `MAP_PRIVATE`
  means, and what Linux does, but it is worth knowing that a plain
  `FUTEX_WAIT` has a side effect on page sharing.

### `<semaphore.h>`

POSIX unnamed semaphores: `sem_init`, `sem_destroy`, `sem_wait`,
`sem_trywait`, `sem_post`, `sem_getvalue`. The count is the futex word,
so an uncontended wait or post is one atomic instruction.

`pshared` is honoured — a semaphore in shared memory works between
processes, because the futex key is physical.

#### Divergences from POSIX

- **Errors are returned directly as negative values**, per this
  library's convention throughout: `sem_trywait` returns `-EAGAIN`, not
  `-1` with `errno` set. musl's wrappers restore the POSIX convention.
- **`sem_wait` never returns `-EINTR`.** A signal arriving mid-wait
  resumes the wait. POSIX permits `EINTR`, but a semaphore whose
  acquisition can fail spuriously pushes a retry loop into every
  caller, and nothing in NeoOS needs to interrupt one yet.
- **`sem_timedwait` is spelled `sem_timedwait_relative` and takes a
  RELATIVE timeout.** POSIX's takes an absolute `CLOCK_REALTIME` time,
  which NeoOS cannot construct without a clock syscall. The different
  name is on purpose: the divergence cannot be reached by accident.
- **Named semaphores (`sem_open`/`sem_close`/`sem_unlink`) do not
  exist.** They need a filesystem namespace for semaphores.

### `<pthread.h>`

A **subset**: `pthread_create`, `pthread_join`, `pthread_exit`,
`pthread_self`, `pthread_equal`, the `pthread_mutex_*` family, and the
`pthread_cond_*` family. What is present has POSIX's names, signatures
and semantics.

The mutex is Drepper's three-state futex mutex, so lock and unlock cost
nothing but two atomic instructions when uncontended. The condition
variable is the sequence-number design: a waiter reads the sequence
before dropping the mutex and sleeps only while it is unchanged, so a
signal in that window cannot be lost.

This header exists because "POSIX mutex" means `pthread_mutex_t` to
every program that wants one, and shipping the mutexes without
`pthread_create` would be a header that compiles and then fails to
link.

#### What is absent, and what each omission costs

- **Attributes.** There is no `pthread_attr_t`, `pthread_mutexattr_t`
  or `pthread_condattr_t`. `pthread_create` and the `*_init` functions
  take a `const void *attr` that **must be null**; a non-null value
  returns `-EINVAL` rather than being silently ignored. So: no
  configurable stack size, no detached threads, no recursive or
  error-checking mutexes, no process-shared mutexes or condvars.
- **`pthread_detach`.** Every thread must be joined, or its kernel
  stack is reclaimed only when the process exits.
- **Cancellation** (`pthread_cancel`, `pthread_setcancelstate`,
  cleanup handlers). Nothing can be interrupted asynchronously.
- **TLS keys** (`pthread_key_create`, `pthread_getspecific`). These
  need thread-local storage, which needs the FS base in the context
  switch — a later milestone.
- **`pthread_once`, rwlocks, barriers, spinlocks.** All buildable on
  the futex; none built yet.
- **`pthread_cond_timedwait` is spelled
  `pthread_cond_timedwait_relative`** and takes a relative timeout, for
  the same reason as `sem_timedwait_relative`.
- **At most 16 threads per process**, and `pthread_join`'s `void *`
  return value is carried in a fixed 16-entry table rather than in
  thread-local storage — because there is no TLS yet. Both limits
  disappear with musl.
- **`pthread_exit` from the last thread does not exit the process
  cleanly**; it ends that thread, and the process ends when its last
  thread does. POSIX says `pthread_exit` from `main` keeps the process
  alive until every thread finishes, which is the same outcome here by
  a different route.

### `clone` — real musl pthreads, alongside the native subset above

```c
long clone(unsigned long flags, void *child_stack,
           void *ptid, void *ctid, void *tls);
```

Raw Linux argument order and register convention
(`rdi=flags, rsi=child_stack, rdx=ptid, r10=ctid, r8=tls`) — this is
what lets musl's own hand-written `clone.s` reach NeoOS with only its
embedded syscall numbers changed, not its register-shuffling logic.
Not a `lib/`-facing API: nothing outside musl's own `pthread_create`
calls this directly, the same way nothing calls `set_tid_address`
directly either.

**Accepts EXACTLY the flag combination musl's `pthread_create.c`
sends** — `CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD
|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID
|CLONE_DETACHED` (`0x7D0F00`) — and `-EINVAL`s anything else. This is
deliberate, not a temporary limitation: every other `clone(2)` use
(namespaces, `CLONE_VFORK`, selective-sharing process creation) is out
of scope for this primitive, and `fork()` already covers process
creation. `CLONE_SYSVSEM`/`CLONE_DETACHED` are accepted and ignored
(NeoOS has no SysV semaphore undo lists to share, and `CLONE_DETACHED`
has been a no-op on real Linux itself for two decades).

This makes musl's own `pthread_create`/`pthread_join`/
`pthread_mutex_*`/`pthread_cond_*` work **unmodified** — they are a
completely separate, non-interacting implementation from the native
`<thread.h>`/`<pthread.h>` subset documented above, which stays
exactly as it was (a NeoOS-native `thread_create`-based subset with
its own tid space and its own mutex/condvar types). Nothing retires
or replaces the other; pick either, but don't mix a native
`thread_t` with a musl `pthread_t`.

**Mechanism**: a new thread in the calling thread's process (no new
address space, fd table, or signal table — NeoOS's threads already
share all three unconditionally, which is exactly what
`CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND` asks for), resuming at
the parent's exact syscall-return site the same way a `fork()`'d
child does, with the caller-supplied `child_stack` substituted for the
parent's own stack and `tls` substituted for the parent's `fs_base`.

**`CLONE_CHILD_CLEARTID`**: at thread exit, the kernel writes 0 to
`*ctid` and `futex_wake`s it. musl's own `__pthread_exit()` actually
does its OWN userspace futex-wake on `t->detach_state` (a different
word) for the common join path, so this kernel-level behavior is not
what unblocks a plain `pthread_join` — but it is real, documented
Linux behavior other code (`__tl_sync`-based thread-list
synchronization, in some musl configurations) can depend on, so it is
implemented rather than silently dropped despite being requested.

**Divergence, found and fixed by this same milestone**: `exit(2)`
(`sys_exit`) is now thread-aware — it ends only the calling thread if
`live_threads > 1`, falling back to the previous whole-process
`process_exit()` only when this is the last thread. Before real
`clone`-created threads existed this distinction was unobservable (the
calling thread was always the only one); musl's `__pthread_exit()`
ends with a plain `exit(2)` call and depends on exactly this
Linux-correct behavior to avoid taking the whole process down every
time a `pthread_create`'d worker finishes.

Detached (clone-created) threads self-reap: since musl's
`pthread_join` never calls NeoOS's own `thread_join` syscall (it
synchronizes purely via the futex above), nothing would ever drain a
clone-created thread's kernel stack from the native join-list. They
are routed instead through the same self-reap drain process-less
kernel threads already use.

## Pipes

```c
#include <unistd.h>
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);   /* O_NONBLOCK, O_CLOEXEC */
```

`fds[0]` is the read end, `fds[1]` the write end. Both are ordinary
file descriptors: `read`, `write` and `close` work on them unchanged,
and `fork` gives the child its own reference on the same pipe.

The three rules that matter:

- **A read from an empty pipe blocks** until data arrives, or returns
  **0** once every write end has been closed. Buffered bytes are still
  readable after the writer closes — closing the write end means
  end-of-stream, not discard.
- **A write to a pipe with no read ends left raises `SIGPIPE`** and
  returns `-EPIPE`. A write that had already transferred some bytes
  reports those instead, as POSIX requires.
- **A blocked reader is woken when the last writer closes**, not only
  when data arrives; otherwise the EOF above would be unreachable from
  the state it exists for. The same in reverse for a writer blocked on
  a full pipe whose last reader goes away.

`lseek` on a pipe returns `-ESPIPE`. Reading the write end or writing
the read end returns `-EBADF`.

### Divergences from Linux

- **Capacity is 4096 bytes**, one page. Linux's default is 65536. This
  is observable only as the point at which a writer blocks, and as the
  size of the largest write that is guaranteed atomic. POSIX's floor
  (`PIPE_BUF`, 512) is comfortably met.
- **No `F_SETPIPE_SZ`**, and no `fcntl` at all, so the capacity cannot
  be changed at run time.
- **`O_CLOEXEC` is accepted and ignored.** NeoOS's `exec` does not walk
  the fd table closing anything yet, so there is nothing for the flag
  to do. It is accepted rather than rejected so that code written for
  Linux compiles and behaves identically in the single case that
  matters today (no exec between the pipe and its use).
- **Named pipes (FIFOs) do not exist**; there is no `mkfifo` and no
  filesystem node type for one.
- **`pipe()` is library code over `pipe2()`**, exactly as musl does it
  on architectures where Linux dropped the legacy call.

## `eventfd` / `eventfd2` (MSC-1)

```c
#include <sys/eventfd.h>
int eventfd(unsigned int initval, int flags);   // -> eventfd2(initval, 0)
```

A 64-bit counter behind a file descriptor — the "wake the poll loop"
primitive for libuv/Node, .NET's `SocketAsyncEngine`, tokio, and Go's
netpoller fallback. Linux-shaped (`kernel/ipc/eventfd.c`, modelled on
the pipe):

- `read` of ≥ 8 bytes returns the counter and zeroes it, or blocks
  while it is 0 (`EAGAIN` if `EFD_NONBLOCK`). In `EFD_SEMAPHORE` mode
  it returns `1` and decrements.
- `write` of ≥ 8 bytes adds to the counter, or blocks while the add
  would exceed `0xfffffffffffffffe` (`EAGAIN` if non-blocking). A
  write of `0xffffffffffffffff` is `-EINVAL`.
- `poll` reports `POLLIN` when the counter is non-zero, `POLLOUT` when
  it is below the ceiling. Registered on the object's own poll head,
  so `poll`/`epoll` wake precisely.
- **`EFD_CLOEXEC` is accepted but inert** — NeoOS has no close-on-exec
  machinery yet, the same divergence `pipe2`'s `O_CLOEXEC` has.
- A read/write of fewer than 8 bytes is `-EINVAL` (Linux's behaviour).

## `prctl` (MSC-1)

```c
#include <sys/prctl.h>
int prctl(int option, ...);
```

Only the no-op-safe subset is serviced; **everything else returns
`-EINVAL`** (notably `PR_SET_SECCOMP` — NeoOS has no seccomp).

- **`PR_SET_NAME` / `PR_GET_NAME`** read/write the process `comm`
  (16 bytes, NUL-terminated at 15). NeoOS has no per-*thread* name, so
  a multithreaded process shares one `comm` — a divergence from Linux,
  where the name is per-task.
- **`PR_GET_DUMPABLE` → 1**, `PR_GET_NO_NEW_PRIVS` / `PR_GET_THP_DISABLE`
  / `PR_GET_CHILD_SUBREAPER` / `PR_GET_PDEATHSIG` → 0,
  `PR_CAPBSET_READ` → 1 (root-all capability model — every capability
  is present).
- **`PR_SET_DUMPABLE`, `PR_SET_NO_NEW_PRIVS`, `PR_SET_THP_DISABLE`,
  `PR_SET_CHILD_SUBREAPER`, `PR_SET_PDEATHSIG`, `PR_CAPBSET_DROP`** are
  accepted and ignored — NeoOS has no THP, no ptrace-dumpable state,
  no parent-death signal, and no capability bounding set to drop from.
  A program that sets one and later depends on its effect will not get
  it; none of the common callers (musl, Go, .NET, systemd-style init)
  do.

## `ppoll`, `clock_nanosleep`, `close_range` (MSC-2)

```c
#include <poll.h>
int ppoll(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
#include <time.h>
int clock_nanosleep(clockid_t, int flags, const struct timespec *, struct timespec *);
#include <unistd.h>
int close_range(unsigned int first, unsigned int last, unsigned int flags);
```

- **`ppoll`** is `poll` with a `struct timespec` timeout (rounded up to
  the 10 ms tick, like every NeoOS timeout) and an optional signal
  mask. The mask is swapped in for the duration of the wait via the
  same mechanism as `rt_sigsuspend` — **not** with Linux's exact
  atomicity. The difference is observable only to a program that races
  a signal against entering the poll; the mask is otherwise applied
  and restored correctly. `nfds == 0` is an honest masked sleep (what
  `pause()` compiles to).
- **`clock_nanosleep`** adds the absolute-deadline sleep
  (`TIMER_ABSTIME`) NeoOS's relative `nanosleep` lacked —
  `pthread_cond_timedwait`, .NET and Go timers use it. `CLOCK_MONOTONIC`
  and `CLOCK_REALTIME` (the latter offset by the boot epoch);
  `CLOCK_PROCESS_CPUTIME_ID` → `-EINVAL`. `remain` is ignored on
  `-EINTR`, the same divergence `nanosleep` documents.
- **`close_range(first, last, flags)`** closes every fd in the
  inclusive range (silent on unused slots). `CLOSE_RANGE_UNSHARE` is a
  no-op (NeoOS fd tables are already per-process); **`CLOSE_RANGE_CLOEXEC`
  is `-EINVAL`** — there is no close-on-exec state to set.

## `ftruncate` (GUI stack G3)

```c
#include <unistd.h>
int ftruncate(int fd, off_t length);
```

Sets a file's length. Growing extends the file with a hole that reads
as zeros; shrinking discards the tail, and bytes discarded that way do
not reappear if the file is grown again. A negative length is
`-EINVAL`, a directory is `-EISDIR`, and a file larger than the
filesystem can hold is `-EFBIG`.

Implemented for **ramfs** (`/tmp`). Every other filesystem returns
`-EINVAL`: fatfs, embedfs, devfs and procfs have no size-setting path.
There is no `truncate(path, length)` — only the fd form.

This is the first size-setting operation the vnode layer has had;
`vfs_ops.truncate` only ever meant "to zero", which is what `O_TRUNC`
uses. `fallocate` still returns `-EOPNOTSUPP`, deliberately: ramfs
allocates on write, so reserving space is a promise nothing would keep.

**Note on sparse reads.** Adding this exposed a bug worth recording,
since it changes observable behaviour: a `read()` crossing a hole in a
ramfs file used to stop at the hole and return a short count, making a
sparse file look truncated at its first unwritten page. Holes now read
as zeros, which is what POSIX requires and what a file grown by
`ftruncate` is made entirely of.

## `memfd_create` (GUI stack G3)

```c
#include <sys/mman.h>
int memfd_create(const char *name, unsigned int flags);
```

An anonymous shared memory object addressed only by a file descriptor.
Nothing is created in any filesystem, so a crash leaves no stale entry
and an unrelated process cannot open another application's buffer.

This is the compositor's surface primitive: a client creates one, sizes
it with `ftruncate`, maps it `MAP_SHARED`, draws into it, and passes the
**descriptor** to the window manager, which maps the same object and
composites from it.

`MFD_CLOEXEC` (1) and `MFD_ALLOW_SEALING` (2) are Linux's values.

**DIVERGENCES:**

- **`MAP_SHARED` only.** `mmap` of a memfd with `MAP_PRIVATE` returns
  `-EINVAL` rather than silently sharing. Copy-on-write would need a
  fault path these pages do not have, and a private view of a surface
  is not a thing any caller wants.
- **Memory is committed by `ftruncate`, not on first touch.** There is
  no demand paging for a shared object — the whole frame list is mapped
  at `mmap` time — so sizing a memfd is the moment its pages are
  allocated and zeroed. Sizing one to 32 MiB costs 32 MiB immediately.
  The ceiling is 32 MiB per object.
- **Shrinking a shared object is refused** with `-EBUSY` once more than
  one reference exists. The frames are mapped `PAGE_NOFREE`, so freeing
  them under a live mapping would hand pages back to the allocator
  while another process still writes to them.
- **`MFD_ALLOW_SEALING` is stored but `F_ADD_SEALS` is not
  implemented.** The flag is accepted so callers that always pass it
  work; there is no way to actually seal.
- **`MFD_CLOEXEC` is accepted and inert**, like `pipe2`'s `O_CLOEXEC` —
  NeoOS has no close-on-exec machinery.

## `statx`, `fsync`/`fdatasync`, `fallocate`, `access`/`faccessat` (MSC-3)

- **`statx`** fills `STATX_BASIC_STATS` from the same vnode data as
  `stat(2)` — mode, nlink, uid/gid, ino, size, blocks, a/c/mtime. The
  **birth-time bit (`STATX_BTIME`) is cleared**: FAT gives NeoOS no
  timestamps to read (the gap `stat` already has). `AT_EMPTY_PATH`
  with a real fd is the fstat form; only `AT_FDCWD` is accepted for a
  path (no `openat` family yet). Layout is Linux's 256-byte
  `struct statx`.
- **`fsync` / `fdatasync`** validate the fd and return 0. NeoOS's block
  cache writes through — there is no dirty-writeback list — so the
  data a successful `write()` returned from is already on the device.
  Not a lie, but not a barrier either: there is no host-side `fsync`
  behind it.
- **`fallocate` returns `-EOPNOTSUPP`.** NeoOS's filesystems cannot
  preallocate, and the vnode layer has no size-set operation beyond
  truncate-to-zero. SQLite / .NET `FileStream` fall back to writing
  zeros.
- **`access` / `faccessat` / `faccessat2`** are a pure existence check
  — NeoOS has no permission model, so `R_OK`/`W_OK`/`X_OK` on any path
  that resolves all succeed. `faccessat2`'s `flags` are ignored. Only
  `AT_FDCWD` for the `*at` forms.

**Deferred (need FS-layer work, a future milestone — not syscall
plumbing):** `renameat2`, `utimensat`, `linkat`/`symlinkat`,
`fchmodat`/`fchownat`, arbitrary-length `ftruncate`, OFD `fcntl`
locks. NeoOS's VFS currently has no rename, no set-attribute, no
link, and no symlink operation for any mounted filesystem.

**Deferred to a later MSC pass:** `timerfd_*` and `signalfd4` — both
need an fd-object plus a tick-driven expiry/pending-signal poke that
NeoOS's thread-only timeout machinery does not have yet. `pselect6`
and `epoll_pwait2` (the other atomic-sigmask variants) likewise.

## AF_UNIX sockets (GUI stack G3)

```c
int s = socket(AF_UNIX, SOCK_STREAM, 0);
bind(s, (struct sockaddr *)&addr, len);
listen(s, backlog);
int c = accept(s, NULL, NULL);
```

`socket`, `bind`, `listen`, `accept4` and `connect` on `AF_UNIX`, with
Linux's `struct sockaddr_un` (a 2-byte family followed by 108 bytes of
path). `socketpair(2)` still exists and is unchanged; this adds the
other shape — a process connecting to a name published by a process it
shares no ancestor with, which is what a compositor needs.

The byte stream is two of the kernel's pipe rings cross-wired, so
`poll`, `epoll` and edge-triggered re-arming all work with no special
cases. A listening socket reports `POLLIN` when a connection is waiting.

**DIVERGENCES:**

- **Abstract namespace only.** An address whose `sun_path[0]` is `'\0'`
  names an abstract socket, and the name is the remaining `addrlen`
  bytes (not NUL-terminated, embedded NULs allowed) — Linux's rule
  exactly. A **pathname** address returns `-EINVAL`: binding one would
  create an `S_IFSOCK` node in the filesystem and resolve it through the
  VFS, which is not implemented. Most Linux software uses pathname
  sockets, so this is the gap to close first if a port needs them.
- **`SOCK_STREAM` only.** `SOCK_DGRAM` on `AF_UNIX` returns
  `-EPROTOTYPE` for `socket()`, though `socketpair()` still accepts it.
- **32 bound names system-wide**, 16 queued connections per listener.
  Exceeding either is `-ENOSPC` and `-ECONNREFUSED` respectively.
- **`accept` reports no peer address.** An `addrlen` passed in is set to
  0. An accepted `AF_UNIX` socket has no address of its own to report,
  and NeoOS does not track the connecting socket's bound name.
- **No `SO_PEERCRED`**, no credential passing, and `getpeername` on an
  `AF_UNIX` socket is not routed.

## socketpair

```c
#include <sys/socket.h>
int socketpair(int domain, int type, int protocol, int sv[2]);
```

`AF_UNIX` only (`domain` must be `AF_UNIX`; anything else is
`-EAFNOSUPPORT`). `type` is `SOCK_STREAM` or `SOCK_DGRAM` -- both
accepted and treated identically, optionally OR'd with
`SOCK_NONBLOCK`/`SOCK_CLOEXEC` exactly like `socket(2)`'s own `type`
argument. `protocol` must be 0.

Built from two of the same pipes `pipe()` uses, cross-wired: `sv[0]`
reads what `sv[1]` writes and vice versa. Both ends behave like a
connected, bidirectional pipe -- the pipe rules above (blocking read
until data or EOF, `SIGPIPE`/`-EPIPE` on a write with no reader left,
wake-on-last-close) apply to each direction independently. Added for
`curl`'s multi-handle wakeup mechanism, which has no other way to get
a pair of fds it can write a byte into and `poll()` on.

### Divergences from Linux

- **No `SCM_RIGHTS` fd-passing and no genuine out-of-band data.** A
  pipe-backed fd has neither; `sendmsg`/`recvmsg` on a socketpair fd
  are not implemented at all (`-ENOSYS`), unlike a real socket.
- **Inherits every pipe divergence above**: 4096-byte capacity per
  direction (Linux's default is a 212992-byte *socket* buffer, not a
  pipe's 65536), no `F_SETPIPE_SZ`, `SOCK_CLOEXEC` accepted and
  ignored for the same reason `O_CLOEXEC` is on `pipe2`.
- **`SOCK_DGRAM` gets no datagram framing.** A pipe has no message
  boundaries, so a `SOCK_DGRAM` socketpair on NeoOS behaves exactly
  like `SOCK_STREAM` -- reads can return fewer bytes than a single
  write sent, unlike Linux's real `AF_UNIX`/`SOCK_DGRAM`, which
  preserves message boundaries. Nothing on NeoOS relies on that
  preservation yet.

## `sched_getaffinity`, `membarrier`, `mlock`, `madvise`, `sysinfo`, `statfs`, `get_mempolicy`

```c
#include <sched.h>
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
#include <linux/membarrier.h>
int membarrier(int cmd, unsigned int flags);
#include <sys/mman.h>
int mlock(const void *addr, size_t len);
int madvise(void *addr, size_t len, int advice);
#include <sys/sysinfo.h>
int sysinfo(struct sysinfo *info);
#include <sys/statfs.h>
int statfs(const char *path, struct statfs *buf);
#include <numaif.h>
long get_mempolicy(int *mode, unsigned long *nodemask, unsigned long maxnode,
                    void *addr, unsigned long flags);
```

Seven syscalls found missing one at a time while getting a dotnet
NativeAOT (CoreCLR) binary running on NeoOS — its runtime
initialization (`RhInitialize`, then the GC's own startup) calls each
of these, and unlike most of musl's own feature probes, at least one
of them (`mlock`) treats an outright `-ENOSYS` as fatal rather than
falling back gracefully. All seven are genuine Linux shapes, not
NeoOS-native additions, so none of them get a `lib/` wrapper — musl's
existing headers already declare them.

- **`sched_getaffinity`** — `pid` is ignored (NeoOS has no per-thread
  CPU pinning to report differently per pid); the returned mask has
  one bit set for every online CPU (`smp_online_count()`), the same
  answer regardless of which thread asked. `-EINVAL` if `cpusetsize`
  is too small to hold the whole mask (Linux never truncates).
- **`membarrier`** — a REAL cross-CPU barrier
  (`kernel/smp/membarrier.c`'s `membarrier_global()`), not a lie: it
  IPIs every online CPU but the caller's and waits for each to
  acknowledge before returning. On x86-64 taking an interrupt is
  itself a serializing event, so IPI delivery alone satisfies every
  command this implements — including the `SYNC_CORE` variants, which
  document needing exactly that guarantee. Every non-`QUERY` command
  (`GLOBAL`, `GLOBAL_EXPEDITED`, `PRIVATE_EXPEDITED`, and their
  `REGISTER_*` counterparts) is serviced by the same global barrier:
  Linux's `PRIVATE_EXPEDITED` variants only promise to reach the
  calling process's own registered threads, which a global barrier
  already does and more, and NeoOS has no per-process CPU registration
  to make the narrower answer worth tracking. `QUERY` reports every
  command above as supported.
- **`mlock`** — a genuine no-op success, not a lie dressed up as one:
  NeoOS has no swap and never pages out anonymous memory, so every
  resident page already satisfies mlock(2)'s promise before this
  function does anything. The one check kept is the one real Linux
  would also make: the range must actually be mapped (`-ENOMEM`
  otherwise). No `munlock` yet — add one the same way, if and when
  something calls it.
- **`madvise`** — also a genuine no-op success for every advice value:
  advice is purely a hint a conforming kernel may ignore, and NeoOS
  has nothing to act on for any of `MADV_DONTNEED`/`MADV_FREE`/
  `MADV_WILLNEED`/etc (no swap, no speculative readahead to steer).
  Same one check as `mlock`: the range must be mapped.
- **`sysinfo`** — real `totalram`/`freeram`, taken directly from
  `kernel/mm/pmm.h`'s frame counters (`mem_unit` is 1, so they read as
  exact bytes). Everything else in `struct sysinfo` — load averages,
  swap, shared/buffer/high memory, uptime — is zero; none of those
  concepts exist on NeoOS yet.
- **`statfs`** — the path must resolve (a bad path is `-ENOENT` like
  `stat`), but the filesystem-specific fields are a generic,
  best-effort answer: NeoOS mounts several unrelated filesystems (FAT,
  ramfs, devfs, procfs, embedfs) with no single shared free-space or
  magic-number concept, so `f_type` is 0 (no magic this implementation
  claims to match) and the block counts are `kernel/mm/pmm.h`'s frame
  counters — an honest number, just not literally "free space on this
  path's own filesystem".
- **`get_mempolicy`** — NeoOS has exactly one NUMA node, always:
  `mode` (if given) is `MPOL_DEFAULT`, `nodemask` (if given, with
  `maxnode >= 1`) has exactly bit 0 set. `addr`/`MPOL_F_ADDR` is not
  interpreted — with one node, "which node is this address on" has
  only one possible answer regardless of which address is asked about.

## `epoll_create1`, `epoll_ctl`, `epoll_wait`, `epoll_pwait`

```c
#include <sys/epoll.h>
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                int timeout, const sigset_t *sigmask);
```

Found missing getting a real TCP socket example running: .NET's
`Socket`/`TcpClient` use `epoll` internally (`SocketAsyncEngine`) even
for a single synchronous `Connect`/`Send`/`Receive` sequence, and
unlike most of the syscalls in the section above, its absence is
fatal rather than gracefully degraded.

Built on the SAME scan-and-sleep loop `poll()`/`select()` already use
(`poll_core`, `kernel/syscall/sys_poll.c`), not a second readiness
mechanism: an epoll object (`kernel/sync/epoll.c`/`.h`) is just a
stored list of `(fd, events, data)` registrations, and `epoll_wait`
turns that list into exactly the `pollfd[]` array `poll()` builds,
then calls the same core. `struct epoll_event` matches Linux's
x86_64 ABI exactly, including the `__attribute__((packed))` that
removes the padding its natural alignment would otherwise have.

`EPOLLET` (edge-triggered mode) is implemented, and is not optional
in practice: .NET's `SocketAsyncEngine` registers **every** socket
with `EPOLLET|EPOLLOUT|EPOLLIN` (`0x80000005`). NeoOS was
level-triggered only at first, on the reasonable-sounding theory
that level-triggered delivers a superset of edge-triggered and is
therefore safe for any caller. It is not safe for a caller *written
against* edge-triggered: that engine's loop is "epoll_wait → hand
the fd to the thread pool → straight back to epoll_wait", which is
correct only because ET does not report the same readiness twice.
Under LT the work item has not run yet, the fd is still ready, and
the same socket is dispatched again and again until the pool is full
of duplicates and nothing is served — an ASP.NET Core app that
answered sequential requests fine and hung at concurrency 8.

How it works: each registration remembers the readiness mask already
reported (`epoll_entry.last_ready`) and the object's readiness
counter at that moment (`last_seq`). A scan reports
`ready_now & ~last_ready`, so a level that is merely still there is
not re-reported; when the object's counter has moved — every
`poll_head_notify` bumps it, and a TCP stream socket, which has no
poll head, exposes its TCB's through `file_ops.ready_seq` — the
registration is re-armed and whatever it is ready for is news again.
The counter is what makes a drop-and-rise *between* two `epoll_wait`
calls an edge, which comparing "ready now" against "ready last time"
cannot see. `EPOLL_CTL_MOD` and a re-`ADD` re-arm, per epoll(7), so
a reused fd number never inherits a consumed edge.

### Divergences from Linux

- **A re-arm is per object, not per direction.** An `EPOLLET`
  registration is re-armed by *any* readiness change on the object,
  so a bit that is only still level-ready can be reported again when
  something unrelated happens to the same fd — reading a socketpair
  frees buffer space, which re-arms it, and `EPOLLOUT` comes back
  even though writability never lapsed. Linux's ET has the same
  shape (its wakeup callback is per socket too) but is finer in
  places NeoOS is not. The property applications depend on holds:
  a bit is never re-reported *without* some real readiness event on
  that object, so an edge-triggered consumer cannot spin. Anything
  correct under Linux ET — drain until `EAGAIN`, then wait — is
  correct here.
- **An object with no readiness counter degrades to
  level-triggered.** If a file's ops supply neither `poll_head` nor
  `ready_seq`, an `EPOLLET` registration on it is re-armed on every
  scan, which is level-triggered behaviour. Deliberate: a missed
  edge is a hang, a spurious one is a wasted wakeup. Every object a
  program can usefully wait on today (pipes, socketpairs, TCP and
  UDP sockets, ttys, eventfd, evdev) has one.
- **`EPOLLONESHOT`, `EPOLLEXCLUSIVE`, `EPOLLWAKEUP`** — accepted as
  bits in the events mask, not acted on.
- **`epoll_pwait`'s `sigmask` is accepted and not applied** — NeoOS
  has no per-call signal-mask swap for any blocking syscall (the one
  place that need is met today is `rt_sigsuspend`). Every caller in
  this milestone passes a null mask.

## `readlink`, `inotify_init1`/`inotify_add_watch`/`inotify_rm_watch`, `getrusage`, `gettid`

Four more found missing getting a real ASP.NET Core app (Kestrel)
running, past the epoll wall above:

- **`readlink(path, buf, bufsize)`** — the path must resolve (a bad
  path is `-ENOENT`, same as `stat`), but the answer past that is
  always `-EINVAL`: no filesystem NeoOS mounts can represent a
  symlink (the same divergence `lstat` already has), so nothing this
  call could name is ever one. Tolerated gracefully by its caller —
  unlike the other three below, this one was never fatal.
- **`inotify_init1(flags)` / `inotify_add_watch(fd, path, mask)` /
  `inotify_rm_watch(fd, wd)`** — a real, but inert, fd
  (`kernel/sync/inotify.c`/`.h`): `poll`/`select`/`epoll` never
  report it readable, and a blocking `read()` never returns (an
  honest answer — no filesystem change notification exists to
  deliver instead of a fake one). `inotify_add_watch` returns a real,
  distinct watch descriptor that names nothing internally, since no
  event will ever reference it either way. **Fatal without this**:
  ASP.NET Core's generic host creates one unconditionally to watch
  `appsettings.json`, whether or not the app ever reloads
  configuration at runtime.
- **`getrusage(who, usage)`** — every field of `struct rusage` is
  zero except `ru_utime`, which is approximated from wall-clock time
  since boot: NeoOS has no per-process/thread CPU-time, page-fault, or
  context-switch accounting to report exactly instead, and `who`
  (`RUSAGE_SELF`/`CHILDREN`/`THREAD`) makes no difference for the
  same reason. `ru_utime` is deliberately **not** just tick-quantized
  wall-clock, either: it is forced strictly monotonic at microsecond
  granularity (never equal to or less than the previous call's
  answer), because a caller diffing two readings close enough
  together to land in the same 10ms timer tick would otherwise see a
  ZERO delta — indistinguishable from "this thread never ran" — from
  a merely-coarse approximation. **Fatal without this** — called
  during the GC's own startup diagnostics.
- **`gettid()`** — not a new primitive at all: it is exactly
  NeoOS's own `thread_self()` (`SYS_THREAD_SELF`) under Linux's name,
  so the shim maps it there directly rather than adding a second
  kernel syscall number for the same answer. **Its absence was the
  most indirect fatal case found this milestone**: with `gettid()`
  returning `-ENOSYS` where musl expected a real tid, `raise(SIGABRT)`
  inside `abort()` (`tkill(gettid(), SIGABRT)` underneath) failed
  silently instead of delivering the signal, so `raise()` *returned*
  instead of ending the process, and execution fell through into
  musl's own deliberate crash-trap (`a_crash()`, an illegal/privileged
  instruction) right after — surfacing as a `#GP`/`SIGSEGV`, not the
  `SIGABRT` `abort()` actually meant to raise. The lesson generalizes:
  an `-ENOSYS` a caller does not check can corrupt a LATER, unrelated
  call's arguments instead of failing where it was actually made.

## `mremap`

```c
#include <sys/mman.h>
void *mremap(void *old_addr, size_t old_size, size_t new_size, int flags);
```

Deliberately narrow: answers "is this address range mapped", not
Linux's full move/grow-a-mapping machinery. `[old_addr, old_addr +
old_size)` must be covered by one existing mapping; if `[old_addr,
old_addr + new_size)` is *also* covered by that same mapping, this
"succeeds in place" and returns `old_addr` unchanged — nothing is
actually resized, because for the one caller this exists for, nothing
needs to be. `flags` must be `0`; `MREMAP_MAYMOVE`/`MREMAP_FIXED`/
`MREMAP_DONTUNMAP` are all `-EINVAL` (nothing on NeoOS needs an actual
move or shrink-with-hole-punching yet).

That one caller is musl's own `pthread_getattr_np()`, probing the
**main** thread's stack size (a `pthread_create()`'d thread's stack
is a real, separately `mmap()`'d region with its size already known
directly — this path is main-thread-only) by calling `mremap()`
against addresses near `libc.auxv` at shrinking offsets, relying on
it failing with `-ENOMEM` *exactly* at the real boundary. With
`mremap` `-ENOSYS`'d, that probe's own retry loop (`while
(mremap(...) == MAP_FAILED && errno == ENOMEM) ...`) never even
engaged — `ENOSYS` isn't `ENOMEM` — so it silently reported the main
thread's stack as **1 page**, regardless of its real size (2048 pages
after the `USER_STACK_PAGES` fix elsewhere in this doc).

Confirmed as a real, live bug, not a hypothetical: found chasing a
genuine .NET GC `FailFast` (`RaiseFailFastException`, via
`WKS::GCHeap::Promote` → `UnixNativeCodeManager::FindMethodInfo` —
the GC's own stack-walker failing to resolve a return address during
a stack scan) triggered by nothing more than `Thread.Start()` +
`GC.Collect()`. The GC's conservative scan of the main thread's stack,
bounded by the lying 1-page answer, walked past its own self-imposed
limit into stack content it had no business reading yet.

Needed a second, independent fix alongside it: `mremap`'s own "is
this range mapped" check relies on a real VMA existing for the range
in the first place, and the main thread's own stack
(`kernel/sched/proc.c`'s `thread_stack_alloc`) had the exact same gap
`elf_load()`'s `PT_LOAD` segments did before an earlier fix in this
same investigation — pages mapped directly via `paging_map_into()`,
with no `vma_insert()` ever called, so `vma_find()` saw nothing there
at all. Fixed by registering one, the same way `elf_load()`'s segments
are (`vma_register_image_segment`, called after releasing `mm_lock`
to avoid taking it twice).

**Confirmed insufficient alone**: with both fixes in place, `mremap`
no longer returns `-ENOSYS` and the main thread's stack size is now
reported correctly — but the same `GC.Collect()` FailFast still
happens, byte-identical. The remaining cause is not about main-thread
stack bounds; it is most likely about the **worker** thread's own
stack/execution-context (its bounds come from `pthread_create`'s own
tracking, untouched by either fix here) or something in how a
suspended thread's register/stack state is captured for the GC's
stack walk — a question this milestone's tools (static disassembly,
kernel-side syscall tracing) could not resolve further without a live
debugger attached to the guest.

## File descriptors are objects, not vnodes

An fd now carries an operations table rather than pointing straight at
a filesystem vnode, which is what lets a pipe be read and written by
the same `read`/`write`/`close` calls. Two consequences are visible
from userland:

- **A vnode-backed fd is readable regardless of its open mode.**
  `O_WRONLY` does not currently prevent a `read`. This has always been
  true in NeoOS and is recorded here rather than quietly changed:
  tightening it would break existing programs for no benefit yet. Only
  pipes distinguish the two directions.
- **`fork` still copies each descriptor by value**, so the file
  position is not shared between parent and child (see `fork` above).
  What IS shared is the underlying object: both sides hold a reference
  on the same pipe, and the pipe's end counts are what decide EOF and
  `SIGPIPE`, not the number of file descriptors.

## Process startup: `<auxv.h>`, and thread-local storage

A NeoOS process now starts on a real SysV/Linux entry stack. At
`_start`, `RSP` is 16-byte aligned and points at:

```
    argc
    argv[0] .. argv[argc-1], NULL
    envp[0] .. NULL                 (empty for now, but present)
    auxv pairs, terminated by AT_NULL
```

`argv[0]` is the path the program was spawned from. `<auxv.h>` exposes
`getauxval()` and `environ`.

Supplied auxiliary vector entries: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`,
`AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM`. That is exactly the set musl's
`__libc_start_main` requires.

### Thread-local storage

`__thread` works. There is no API: the C runtime allocates each
thread's TLS block from the image's `PT_TLS` template and installs the
thread pointer, for the main thread at startup and for every other
thread inside `thread_create`/`pthread_create`.

The layout is x86-64's variant II — the TLS block sits *below* the
thread pointer, `%fs:0` is a self-pointer — and `%fs` is per-thread,
saved and restored on every context switch. That last part is not
optional on NeoOS: threads migrate between CPUs, so a thread arriving
on a CPU would otherwise inherit whatever thread pointer the previous
occupant left behind.

```c
#include <tls.h>
int arch_prctl(int code, unsigned long addr);   /* ARCH_SET_FS, ARCH_GET_FS */
```

### Divergences from Linux

- **`ARCH_SET_GS` and `ARCH_GET_GS` return `-EINVAL`.** NeoOS uses
  `%gs` for its own per-CPU block: on kernel entry `GS_BASE` holds the
  per-CPU pointer and `KERNEL_GS_BASE` holds userland's, so "set the
  user GS base" means writing the swapped MSR, and getting that subtly
  wrong corrupts `this_cpu()` for every thread on that CPU. No x86-64
  libc uses it.
- **`AT_RANDOM` is a seeded PRNG, not cryptographic.** The sixteen bytes
  are generated from a splitmix64 + xoshiro256** CSPRNG seeded at boot from
  RTC ⊕ TSC ⊕ a stack address ⊕ RDRAND (if available). **Not an entropy pool**:
  the seed is deterministic and the stream is not reseeded — adequate for the
  stack guard canary, not for cryptographic keys or `/dev/random`.
- **No `AT_BASE`, `AT_SECURE`, `AT_HWCAP`, `AT_CLKTCK`, `AT_UID` and
  friends.** `AT_BASE` in particular is absent because there is no
  dynamic linker; a program that finds no `AT_BASE` correctly concludes
  it is static.
- **`envp` is always empty.** Nothing sets an environment yet, so
  `getenv` would always fail. `environ` is a valid, NULL-terminated
  array rather than a null pointer, so code that walks it works.
- **Only the local-exec TLS model.** Executables are static and
  non-PIE, and are built with `-ftls-model=local-exec`. The
  initial-exec, local-dynamic and general-dynamic models need a dynamic
  linker and a DTV, which arrive with dynamic linking. `__tls_get_addr`
  does not exist.
- **`set_thread_area` / `set_tid_address` do not exist.** `arch_prctl`
  is the only way to install a thread pointer.
- **The TLS block is `mmap`ped and never freed** when a thread exits;
  it is reclaimed with the address space at process exit. Threads are
  bounded at 16 per process, so the leak is bounded too.

### `<sys/mman.h>`

`mmap`, `munmap` and `mprotect` now have their POSIX shapes —
`MAP_FAILED` and `-1` rather than a negative errno — alongside the raw
`mmap_raw`/`munmap_raw` the rest of this library's convention uses.
Only anonymous private mappings are supported: a non-negative `fd` or a
non-zero offset returns `MAP_FAILED`.

`mprotect` has POSIX/Linux semantics for page contents: it changes
access only and never discards data, in either direction. A range made
`PROT_READ` keeps everything written to it and becomes read-only; made
`PROT_READ|PROT_WRITE` again it is still there and writable.
`PROT_NONE` is supported and faults on read as well as write, and the
contents survive a `PROT_NONE` round trip. A `PROT_NONE` page keeps its
frame, so a large `PROT_NONE` guard region costs physical memory only
for the pages that were actually touched before it was protected.
`mprotect` on a copy-on-write page (either side of a `fork`) does not
break the sharing itself: the page is left read-only and the first
write takes the ordinary COW fault.

### DIVERGENCE — W^X enforced

`mmap` and `mprotect` reject a `prot` that contains **both**
`PROT_WRITE` and `PROT_EXEC` with `-EINVAL`. Linux permits W+X
mappings (subject to lockdown / SELinux / `MDWE`); NeoOS does not, at
all. A JIT or trampoline generator that needs to both write and
execute a region must keep two mappings of the same pages (one `RW`,
one `RX`) or `mprotect` between the two states. The ELF loader applies
the same rule: a `PT_LOAD` segment that is `W` and `X` in the file is
refused (`execve` / spawn fails). No toolchain emits such a segment.

A program's own `.text` and `.rodata` are mapped read-only, so a stray
write through a code or const pointer faults (`SIGSEGV`) rather than
silently succeeding as it did before the loader honoured `p_flags`.

## Sockets: `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`

```c
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int64_t sendto(int fd, const void *buf, uint64_t len, int flags,
               const struct sockaddr *dest, socklen_t dest_len);
int64_t recvfrom(int fd, void *buf, uint64_t len, int flags,
                 struct sockaddr *src, socklen_t *src_len);
int64_t send(int fd, const void *buf, uint64_t len, int flags);
int64_t recv(int fd, void *buf, uint64_t len, int flags);
int64_t sendmsg(int fd, const struct msghdr *msg, int flags);
int64_t recvmsg(int fd, struct msghdr *msg, int flags);
uint16_t htons(uint16_t); uint32_t htonl(uint32_t);   /* and ntoh* */
uint32_t inet_addr(const char *);
char    *inet_ntoa_r(uint32_t addr_n, char *out);
```

A socket is an ordinary file descriptor: `close` works, `fork`
inherits it, and on a **connected** socket so do `read` and `write`,
which are `recv` and `send` with no address — exactly as POSIX defines
them.

`struct sockaddr_in` is Linux's x86-64 layout byte for byte: 16 bytes,
family at offset 0, port at 2, address at 4, eight bytes of padding.
The kernel asserts those offsets at boot, because getting one wrong is
invisible until a ported program's port lands in the wrong half of a
word.

Underneath is a real IPv4 stack over a loopback device **and a
virtio-net NIC** (D1–D5): every datagram or segment carries an IPv4
header with a verified checksum and a transport header with a verified
pseudo-header checksum, is routed through a real routing table, and --
off the machine -- is framed in Ethernet with the next hop resolved by
ARP. It is not a shortcut between two buffers.

### Divergences, and what is simply absent

- **Two interfaces: loopback and one NIC.** `127.0.0.0/8` routes to
  loopback, the leased subnet is on link, and everything else goes via
  the default gateway. An address with no matching route is
  `-ENETUNREACH`. The routing table is not reachable from userland:
  there is no `route(8)`, no `AF_NETLINK`, and no `ioctl` to change it.
- **`socket(2)` itself is `AF_INET` only** (no `AF_INET6`). `AF_UNIX`
  exists solely via `socketpair()` (see the socketpair section above)
  -- there is no `AF_UNIX` `socket()`/`bind()`/`connect()`, no path-
  based Unix domain sockets.
- **`socket()`'s `protocol` accepts `0`, `IPPROTO_TCP`, or
  `IPPROTO_UDP`** -- anything else is `-EPROTONOSUPPORT`. A
  standards-compliant `getaddrinfo()` fills `ai_protocol` with the real
  protocol number for the type it describes, so a program that passes
  `ai_protocol` straight into `socket()` (as curl does) needs
  `IPPROTO_TCP` to work for a `SOCK_STREAM` result, not just `0`.
- **No `MSG_*` flags at all**, and the header deliberately does not
  define them. `flags` must be 0. In particular there is no
  `MSG_DONTWAIT` and no `MSG_PEEK`, and `MSG_TRUNC` is not available to
  report a truncated datagram — `recvfrom` returns what it delivered,
  and the rest of the message is discarded.
- **`setsockopt`/`getsockopt` exist but are a short list** — see the
  TCP section below for exactly which options, and which are accepted
  and ignored. The UDP receive buffer is 64 KiB per socket and a
  datagram that does not fit is dropped, as UDP permits.
- **`poll`/`select` work on sockets.** `epoll` does not exist.
- **ICMP exists, but not from userland (D3).** The kernel answers echo
  requests — the host can `ping` NeoOS — and generates a port
  unreachable for a datagram sent to a port nobody has bound. Neither
  is reachable from a program: there are still **no raw sockets**, so
  there is no `ping(8)` and a UDP sender cannot see the unreachable
  that its datagram provoked. `sendto` to a closed port still returns
  success, as it does on Linux for the first datagram.
- **Errors are returned directly as negative values**, per this
  library's convention: `socket()` returns `-EAFNOSUPPORT`, not `-1`
  with `errno`.
- **`inet_ntoa` is spelled `inet_ntoa_r`** and takes the output buffer.
  The standard one returns a pointer to a static buffer, which is not
  thread-safe; NeoOS has threads and no reason to reproduce that.
- **`sendmsg`/`recvmsg` work on both `SOCK_DGRAM` and `SOCK_STREAM`**
  (they are implemented by gathering/scattering through `sendto`/
  `recvfrom`, which themselves dispatch by socket type). `msg_control`/
  `msg_controllen` (ancillary data -- `SCM_RIGHTS` fd-passing, packet
  timestamps) are not implemented; a program using them should expect
  `msg_controllen` to always read back 0.
- **`sendto`/`recvfrom` on a connected `SOCK_STREAM` socket** behave
  like `send`/`recv` when `dest`/`src` is `NULL` (Linux's own rule); a
  non-`NULL` `dest` on an already-connected stream socket is
  `-EISCONN`, matching Linux rather than silently accepting it.

### TCP (D5)

```c
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int shutdown(int fd, int how);          /* SHUT_RD, SHUT_WR, SHUT_RDWR */
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
```

`socket(AF_INET, SOCK_STREAM, 0)` works. The state machine is the full
eleven states with Reno congestion control, Nagle, delayed ACK, an
eight-segment reassembly queue, Jacobson/Karels round-trip estimation
with Karn's algorithm, a persist timer for a zero window, and
retransmission with exponential backoff. `connect` blocks, or returns
`EINPROGRESS` under `O_NONBLOCK` with the result readable through
`SO_ERROR` and `poll(POLLOUT)`. `read`/`write` work on a connected
socket, as POSIX says they do.

Options implemented: `SO_REUSEADDR`, `SO_ERROR`, `SO_TYPE`, `SO_SNDBUF`,
`SO_RCVBUF` (all at `SOL_SOCKET`), and `TCP_NODELAY` at `IPPROTO_TCP`.
Anything else returns `-ENOPROTOOPT` rather than succeeding silently: a
program that sets an option and does not get it behaves mysteriously
forever after.

**Divergences, each deliberate:**

- **MSL is 5 seconds, so TIME_WAIT is 10** — not Linux's 60 and 120. A
  two-minute TIME_WAIT cannot be observed inside a boot that also runs
  forty other suites, and the state would then be untested. A program
  that reuses a port sooner than Linux would allow will succeed here.
- **`SO_SNDBUF` and `SO_RCVBUF` are accepted and ignored**, and read
  back as the real fixed sizes (32 KiB each way). The connection table
  is static, so the buffers are a compile-time constant. Failing
  instead would break every program that sets a buffer size out of
  habit.
- **Thirty-two connections, machine-wide.** The table is static so that
  nothing allocates on the receive path: a SYN flood exhausts a fixed
  table and is refused with RST, rather than exhausting the heap. The
  thirty-third connection gets `ECONNREFUSED`, not `EMFILE`.
- **The table and the short TIME_WAIT interact, and the arithmetic is
  worth knowing.** The side that closes first holds its slot for 2×MSL
  — ten seconds here — so the machine sustains at most about thirty-two
  connection *closes* per ten seconds in total. A program that opens and
  closes connections in a tight loop will start seeing `ECONNREFUSED`
  long before anything is actually wrong. On Linux the same arithmetic
  exists with a 120-second TIME_WAIT and tens of thousands of slots,
  which is why it is never noticed there.
- **No window scaling, no SACK, no timestamps, no ECN, no TCP Fast
  Open, no keepalives, and no `SO_LINGER`.** A 32 KiB window needs no
  scaling; the rest are absent rather than stubbed.
- **No `sendmsg`/`recvmsg`, no `socketpair`.**
- **`accept4` honours `SOCK_NONBLOCK`; `SOCK_CLOEXEC` is accepted and
  ignored**, because NeoOS has no exec-time descriptor closing yet.

### Addressing: DHCP runs in the kernel (D4)

**This is a divergence from Linux, and a deliberate one.** On Linux a
DHCP client is a userspace program. On NeoOS it is `kernel/net/dhcp.c`,
on its own kernel thread.

The reason is surface area. A userspace client must receive an offer
addressed to an address it does not have yet, which needs `AF_PACKET`
or raw sockets; it must send to 255.255.255.255, which needs
`SO_BROADCAST`; and it must install a route, which needs a routing API.
That is three pieces of **user-facing** kernel interface — each with
its own ABI obligations under this document — designed, implemented and
frozen before the machine can find out its own address. The kernel
client needs none of them.

The decision is reversible by construction: `dhcp.c` speaks to the same
`net_udp_output` / `net_udp_hook` pair a socket would, so replacing it
with a userspace client later means **adding** raw sockets, not
unpicking the stack.

Two consequences a program can observe:

- **DNS resolution works, but the nameserver is static, not DHCP-learned.**
  `/etc/resolv.conf` (generated at disk-image build time, `nameserver
  10.0.2.3` -- slirp's built-in resolver) is what musl's real resolver
  (`getaddrinfo`/`gethostbyname`, unmodified upstream code) reads;
  DHCP option 6 is still parsed and stored (`dhcp.c`) but nothing
  wires it into `/etc/resolv.conf` yet -- a real divergence, since a
  network with a different DNS server than slirp's would need this
  file hand-edited. `getaddrinfo`/`gethostbyname` needed two real
  kernel-side fixes to work at all (see docs/abi-compatibility.md's
  DNS resolution refresh): `sendmsg`/`recvmsg` didn't exist as
  syscalls, and an unbound/wildcard-bound UDP socket's outgoing
  packets claimed a source address of `127.0.0.1` regardless of
  destination.
- **There is no way for a program to renew, release, or inspect the
  lease.** The address is simply there. `getsockname` on a bound socket
  is the only way to see it.

If no server answers within five seconds the kernel installs a static
**10.0.2.15/24 via 10.0.2.2** — QEMU user-networking's well-known first
lease — and keeps trying in the background. It logs a **distinct** line
when it does, because a broken client and an absent server otherwise
produce an identical routing table.

## MPI: `<mpi.h>`

A subset of MPI-1, in userland, over UDP on 127.0.0.1. Rank *r* listens
on port 20000+*r*, and a message is exactly one datagram.

```c
int MPI_Init(int *argc, char ***argv);        int MPI_Finalize(void);
int MPI_Comm_size(MPI_Comm, int *);           int MPI_Comm_rank(MPI_Comm, int *);
int MPI_Send(const void *, int, MPI_Datatype, int dest, int tag, MPI_Comm);
int MPI_Recv(void *, int, MPI_Datatype, int src, int tag, MPI_Comm, MPI_Status *);
int MPI_Barrier(MPI_Comm);                    int MPI_Bcast(void *, int, MPI_Datatype, int root, MPI_Comm);
int MPI_Reduce(...);                          int MPI_Allreduce(...);
int MPI_Launch(const char *path, int size, int *out_pids);   /* NeoOS-specific */
```

Datatypes: `MPI_BYTE`, `MPI_CHAR`, `MPI_INT`, `MPI_LONG`, `MPI_DOUBLE`.
Operations: `MPI_SUM`, `MPI_PROD`, `MPI_MAX`, `MPI_MIN`.
`MPI_ANY_SOURCE` and `MPI_ANY_TAG` work, and a message that does not
match the current receive is queued rather than dropped — without which
two ranks exchanging messages in opposite orders would deadlock.

Building on sockets rather than on a bespoke kernel "port" object was a
deliberate choice: the transport is one already tested and already
visible to a debugger, it is what a real MPI uses for its TCP path, and
when NeoOS gets a NIC this runs between machines with no change above
the socket calls.

### Divergences, and what is absent

- **`mpirun` is `MPI_Launch()`**, a NeoOS-specific call that spawns the
  ranks with their rank and world size in `argv`. A real MPI passes
  those through the environment; NeoOS has no environment yet.
  `MPI_Init` reads them back and removes them, so a program's own
  `argc`/`argv` look normal afterwards.
- **One communicator.** `MPI_COMM_WORLD` only: no `MPI_Comm_split`, no
  `MPI_Comm_dup`, no groups, no inter-communicators, no Cartesian or
  graph topologies.
- **Blocking point-to-point only.** No `MPI_Isend`/`MPI_Irecv`/`MPI_Wait`,
  no `MPI_Sendrecv`, no persistent requests, no `MPI_Probe`. Note that
  `MPI_Send` here never blocks — a datagram send does not wait for a
  receiver — so ring exchanges that would deadlock on a synchronous
  implementation happen to work. **Do not rely on that**; it is not
  what MPI guarantees.
- **Collectives are O(n) through rank 0**, not trees. With four ranks on
  one machine the difference is unmeasurable, and a correct simple
  collective is a better base to optimise from than a clever one that
  is subtly wrong. No `MPI_Gather`, `MPI_Scatter`, `MPI_Alltoall`,
  `MPI_Scan`, or user-defined operations.
- **`MPI_MAX_MESSAGE` is 8192 bytes.** A message is one datagram and
  there is no segmentation layer, so this is a real limit: a larger
  `MPI_Send` returns `MPI_ERR_COUNT`.
- **At most 16 ranks**, and at most 16 unmatched messages queued per
  rank. A seventeenth unmatched message prints a warning and is
  dropped, which is a real (bounded) failure mode rather than a silent
  one.
- **Tags above `MPI_TAG_UB` (0x3FFFFFFF) are refused.** The library's
  own collective tags live above it, with a per-collective sequence
  number in the low bits — because collectives are not synchronous, and
  a fast rank reaching the *next* collective while the root is still
  gathering the last one will otherwise have its contribution folded
  into the wrong result. (Observed: `MPI_Reduce(SUM)` immediately
  followed by `MPI_Allreduce(MAX)` over 1,2,3,4 gave 8 instead of 10.)
- **No error handlers, no `MPI_Abort`, no `MPI_Wtime`.** Every call
  returns an `MPI_ERR_*` code; nothing is installed to act on one.
- **`MPI_Recv` reports `MPI_ERR_TRUNCATE`** for a message larger than
  the buffer, which is MPI's rule and the opposite of `recvfrom`'s
  silent truncation.

## `<sys/stat.h>`

- `int stat(const char *path, struct stat *st)` — metadata for `path`,
  which may be relative (resolved against the working directory).
- `int lstat(const char *path, struct stat *st)` — identical to `stat`.
- `int fstat(int fd, struct stat *st)` — metadata for an open file.
- `int fstatat(int dirfd, const char *path, struct stat *st, int flags)`
  — `dirfd` must be `AT_FDCWD`; see the divergence below.

All four return 0 or a negative `<errno.h>` code. `struct stat` is
**Linux's x86-64 layout, byte for byte** — 144 bytes, asserted at
compile time in `kernel/fs/stat.h` and again from userland by
`stattest`, because musl compiles its own copy of this struct into
every caller and no shim can correct a wrong offset.

### DIVERGENCE: most of `struct stat` is synthesized

Real: `st_ino`, `st_size`, the file type in `st_mode`, and `st_dev`
(the mount's slot index, +1 so no valid device is 0).

Synthesized, because FAT does not store them and NeoOS has no clock
syscall to have recorded them with:

| Field | Value | Why |
|---|---|---|
| permission bits of `st_mode` | `0755` dirs, `0644` files, `0666` devices | zero would read as "nobody may touch this"; these are what a FAT driver reports on Linux too |
| `st_uid`, `st_gid` | 0 | single-user system, no credentials exist |
| `st_nlink` | 2 for directories, 1 otherwise | FAT has no link count; 2 is the conventional `.`/`..` answer |
| `st_rdev` | 0 | device nodes are not numbered |
| `st_atime`/`st_mtime`/`st_ctime` | 0 (the epoch) | **no clock syscall yet.** Every file looks equally old, so `make` and anything else comparing timestamps will misbehave |

`st_blksize` is 512 and `st_blocks` is the size rounded **up**, as on
Linux.

### DIVERGENCE: `lstat` is `stat`

They differ only on a symbolic link, and no filesystem NeoOS mounts can
represent one — FAT has no such entry type. When symlinks exist, this
must be revisited.

### DIVERGENCE: `fstatat` takes only `AT_FDCWD`

A real directory fd returns `-EBADF`. There is no `openat` family yet,
so nothing in userland can obtain one, and resolving against a dirfd
would need path resolution to start somewhere other than a mount root —
which the working-directory design deliberately avoids. `AT_EMPTY_PATH`
with a real fd works and is equivalent to `fstat`.
`AT_SYMLINK_NOFOLLOW` is accepted and ignored, for the reason above.

`fstat` on a pipe or a socket returns `-EINVAL`: those have no vnode,
and inventing an inode number for them would be worse than refusing.

## musl, and the shim

**musl 1.2.5 is built and linked, and a musl binary runs.**
`userland/musl/hello.c` exercises printf, malloc, stat, open/read,
opendir/readdir, fopen/fgets, clock_gettime, isatty and getpid, and is
part of `make test` as `[musltest]`.

The shim (`third_party/shim/`) does exactly two things, never a third:

1. **Number translation** — Linux's syscall numbers onto NeoOS's.
2. **Argument reshaping** — Linux passes a NUL-terminated `const char *`
   for paths; NeoOS's path syscalls take a (pointer, length) pair, so
   the shim measures the string and shifts the remaining arguments.

It never implements a primitive. An unmapped call returns `-ENOSYS`,
which is the signal that the primitive belongs in the kernel.

### The part that is easy to miss

`arch/x86_64/syscall_arch.h` is **not** the only place musl issues
syscalls. Six hand-written assembly files issue `syscall` themselves
and bypass it completely. Leaving them alone is not a clean failure:
Linux's number lands on whatever NeoOS call shares it — `clone` (56)
would have called `lstat`.

`__set_thread_area.s` was the one that mattered first: without it musl
cannot install a thread pointer, `__init_tp` returns -1, and
`__init_tls` reaches its one `a_crash()` — a `hlt` in ring 3 — before
`main`. The symptom was a process that ran, exited 0, and printed
nothing.

`third_party/musl-README.md` lists all eight replaced files.

### DIVERGENCES the shim records rather than hides

- **`clone` returns `-ENOSYS`.** NeoOS has no `clone`, so musl's
  `pthread_create` fails cleanly instead of corrupting something.
- **`vfork` is a real `fork`.** NeoOS has no `vfork`; fork is the safe
  direction to diverge in, since vfork's contract is a subset.
- **`open`'s and `mkdir`'s `mode` argument is dropped.** NeoOS has no
  permission bits to store it in.
- **Cancellation points are approximate.** `__cp_begin`/`__cp_end` no
  longer bracket the `syscall` instruction itself, so the test pthread
  cancellation uses is not exact. Nothing uses musl's pthreads yet.

## Tier 0: the calls musl makes before `main`

- `int64_t writev(int fd, const struct iovec *iov, int iovcnt)` and
  `readv` — `<sys/uio.h>`. musl's stdio writes **only** through
  `writev`. At most `IOV_MAX` (16) vectors; more is `-EINVAL` rather
  than a silent truncation. A short transfer ends the call, and bytes
  already moved are reported, as on Linux.
- `int ioctl(int fd, unsigned long request, void *arg)` — on
  `/dev/CONSOLE` (a real TTY) it answers `TCGETS`, `TCSETS`, `TCSETSW`,
  `TCSETSF`, `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCGPGRP` and `TIOCSPGRP`
  (see `<termios.h>` below). On a regular file, pipe or socket it
  returns `-ENOTTY`, as Linux does.
- `int isatty(int fd)` — 1 on `/dev/CONSOLE`, 0 elsewhere. musl's
  `isatty` probes with `ioctl(TIOCGWINSZ)`.
- `int clock_gettime(int clk, struct timespec *out)` — see below.
- `int nanosleep(const struct timespec *req, struct timespec *rem)` —
  blocks on a timer waitq until the deadline, rounded **up** to a whole
  10ms tick. Returns `-EINTR` if the thread is killed mid-sleep. `rem`
  is accepted and ignored.
- `int set_tid_address(void *ptr)` — returns the caller's tid.
- `void exit_group(int code)` — ends every thread in the process.

### DIVERGENCE: 10ms resolution, and a fragile wall clock

NeoOS's only fine time source is the 100Hz LAPIC tick, so **resolution
is 10ms**. `CLOCK_REALTIME` is wall time, anchored to the CMOS RTC read
once at boot; `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW` and the two
CPU-time clocks count from boot. All five ids resolve.

If the RTC cannot be read at boot, `CLOCK_REALTIME` silently falls back
to a boot epoch and formats as January 1970 (`rtc_is_real()` reports
which). Either way `stat`'s timestamps are all zero — nothing records
file times yet — so anything comparing a file's mtime against the
clock, `make` above all, will misbehave.

### DIVERGENCE: `set_tid_address`'s pointer is recorded, not acted on

The address is Linux's "clear child tid": the kernel writes 0 there and
futex-wakes it when the thread exits, which is how a joiner notices.
NeoOS joins through `thread_join` instead, so the pointer is stored and
never written. musl's `pthread_join` spins on that word — when
something uses musl's pthreads, the wake belongs here, not in a shim.

## `<termios.h>` and the console TTY

`/dev/CONSOLE` (and its alias `/dev/TTY`) is a line-discipline
terminal, not a raw byte sink. The keyboard IRQ feeds characters
through the input side: canonical-mode line buffering, echo, `ERASE`
and `KILL` editing, `\r`→`\n` translation, and signal generation
(`INTR`→`SIGINT`, `QUIT`→`SIGQUIT`, `SUSP`→`SIGTSTP`) to the
foreground process group. A `read` blocks until a full line is
available in canonical mode, or `VMIN`/`VTIME` are satisfied in raw
mode.

`struct termios` is **musl's** — it has `c_ispeed`/`c_ospeed` and
`c_cc[32]`. The kernel only ever reads or writes the leading 36 bytes
(Linux's kernel `termios`, `NCCS` 19), exactly as Linux does, so the
extra tail is left untouched.

- `tcgetattr`/`tcsetattr` — `TCGETS` / `TCSETS`/`TCSETSW`/`TCSETSF`.
  The optional-actions argument is accepted; there is no output queue
  to drain, so `TCSETSW` and `TCSETSF` behave as `TCSETS`.
- `TIOCGWINSZ`/`TIOCSWINSZ` — `struct winsize`. The default is 80×25;
  a set is remembered and a `SIGWINCH` is **not** sent (no source of
  resize events exists).
- `TIOCGPGRP`/`TIOCSPGRP` — the foreground process group that job
  control tracks; the signal characters deliver to it.

### DIVERGENCE

No `TCXONC` (flow control), no `TCFLSH`, no `TIOCSCTTY`/`TIOCNOTTY`
(the controlling terminal is implicit and permanent), no pseudoterminals.
Baud rate is stored but meaningless — the backing device is a fixed
serial line plus a PS/2 keyboard.

## `<linux/input.h>` and the evdev interface

`/dev/input/event0` is the raw keyboard event device; applications can
read from it to receive key events without the TTY line discipline. The
device is a standard Linux evdev character device with a 256-entry
ring buffer per open file descriptor, returning `struct input_event`
records (24 bytes each: two `int64_t` timestamps, two `uint16_t` type/code,
one `int32_t` value).

- `read(fd, &ev, sizeof(ev))` — copies as many whole `struct input_event`
  records as fit in the buffer and are available in the ring. Returns the
  byte count (a multiple of 24), `-EAGAIN` if the ring is empty (see the
  DIVERGENCE note — this is returned regardless of `O_NONBLOCK`), or
  `-EINVAL` if the buffer is smaller than 24 bytes.
- `ioctl(fd, EVIOCGVERSION, &ver)` — returns `0x010001` in `ver`.
- `ioctl(fd, EVIOCGNAME(len), name)` — copies up to `len` bytes of
  the device name (`"NeoOS AT keyboard"`) and returns the actual name
  length. Returns a negative value if `len` is too small.
- `ioctl(fd, EVIOCGID, &id)` — fills `struct input_id` with
  `{.bustype = BUS_I8042, .vendor = 0x0001, .product = 0x0001, .version = 0x0100}`.
- `ioctl(fd, EVIOCGBIT(EV_KEY, len), buf)` — copies a bitmap of the
  supported KEY_* codes (e.g., `KEY_A`, `KEY_LCTRL`, arrow keys); US
  keyboard subset only.
- `ioctl(fd, EVIOCGBIT(EV_MSC, len), buf)` — returns a bitmap with only
  `MSC_SCAN` set.
- `ioctl(fd, EVIOCGBIT(0, len), buf)` — returns a bitmap with `EV_SYN`,
  `EV_KEY`, and `EV_MSC` set.
- `ioctl(fd, EVIOCGKEY(len), buf)` — returns a bitmap of keys currently
  held down (which KEY_* codes are pressed).
- `ioctl(fd, EVIOCGRAB, 1)` — acquires an exclusive grab: while held,
  keyboard input does not reach the TTY. Only one fd can hold the grab
  at a time; a second grab attempt returns `-EBUSY`. Returns 0 on success.
- `ioctl(fd, EVIOCGRAB, 0)` — releases the grab.
- `poll(fd, ...)` or `select(fd, ...)` — returns `POLLIN` when events
  are available in the buffer, `POLLOUT` always.

### Event format

Each `struct input_event` carries:
- `tv_sec`, `tv_usec`: wall-clock timestamp (from `CLOCK_REALTIME`).
- `type`: one of `EV_MSC`, `EV_KEY`, `EV_SYN` (others are reserved).
- `code`: the key code (e.g., `KEY_A`, `MSC_SCAN`) or sync type
  (e.g., `SYN_REPORT`).
- `value`: key pressed (1), released (0), or sync count (0 for `SYN_REPORT`).

A key event emits three records: `EV_MSC/MSC_SCAN/(raw_scancode)`,
`EV_KEY/(KEY_*)/(1 or 0)`, `EV_SYN/SYN_REPORT/0`.

### Ring buffer and overflow

The buffer holds 256 events per open fd. When full, the oldest event
is dropped (no notification to userland — the `value` field of the last
`EV_SYN/SYN_REPORT` is incremented by 1 each time an event is discarded,
to make the drop discoverable by monitoring it).

### DIVERGENCE

- **`read` never blocks** — a `read` of an empty ring returns `-EAGAIN`
  whether or not `O_NONBLOCK` is set, so a blocking reader must poll.
  Linux blocks until an event arrives. The evdev ring is guarded by the
  input lock, which ranks above the wait-queue lock, so the reader
  cannot sleep on the queue without a lock-rank inversion; giving each
  client its own ring lock is the fix, deferred to a later milestone.
  Use `poll`/`select` (which report `POLLIN` correctly) to wait.
- **US keyboard layout only** — the decoder produces only Set-1 PC keyboard
  codes. Non-US layouts are not supported.
- **No `EVIOCSCLOCKID`** — timestamps are always `CLOCK_REALTIME`; the
  ioctl is not implemented and returns `-EINVAL`.
- **No events other than keyboard** — `/dev/input/event0` is the sole
  device, and it speaks only `EV_SYN`, `EV_KEY`, and `EV_MSC/MSC_SCAN`.
- The "name" and "phys" strings are fixed. `EVIOCGPHYS` and `EVIOCGUNIQ`
  return `-ENOENT`.

## The working directory

**Every process has a working directory from the moment it is created.**
`proc_alloc` sets it to `/` before any creation path runs, `fork` and
`spawn` inherit the caller's, and the first process keeps `/`. No
path-taking syscall ever has to cope with a process that has none.

Every syscall that takes a path — `open`, `mkdir`, `unlink`, `spawn`,
`spawnv`, `exec`, `mount`, `umount` — resolves it against the caller's
working directory, so a relative path means the same thing everywhere.

### DIVERGENCE: `..` is resolved textually

NeoOS canonicalises a path *before* walking it: the string is joined
onto the working directory and `.` and `..` are removed lexically, then
the result is walked from a mount root. Linux walks first and resolves
`..` against the directory it actually reached.

The two differ only where a symlink is involved, and NeoOS has no
symlinks — FAT cannot represent one. The reason for choosing the
textual form is `getcwd`: it returns a stored string, and FAT offers no
way to map a directory back to its name, so a walk-based `..` would
leave no way to report the path afterwards.

`..` at the root stays at the root, as on Linux.

### DIVERGENCE: no `openat` family, and no `dirfd`

There is no `openat`, `unlinkat`, `mkdirat` or `AT_FDCWD`. musl's
`stat`/`lstat` reach for the plain forms on x86_64 when the path is
absolute or the directory is `AT_FDCWD`, which is why the *at* family
is not yet needed; anything resolving against a real directory fd is
not supported. See `docs/porting-coreutils.md`.

### Limits

A path is bounded by `VFS_MAX_PATH` (512 bytes) after joining, and a
single component by `VFS_NAME_MAX` (256 — a VFAT long name of 255 plus
NUL). Exceeding the first gives `-ENAMETOOLONG`; a component longer than
the second is truncated by the path walker.

## `spawnv` and `fcntl`

```c
int spawnv(const char *path, char *const argv[]);   /* <unistd.h> */
int fcntl(int fd, int cmd, int arg);                /* <fcntl.h>  */
```

## `reboot` and PID 1

```c
#include <sys/reboot.h>
int reboot(int cmd);
```

`cmd` is one of `LINUX_REBOOT_CMD_POWER_OFF`, `LINUX_REBOOT_CMD_HALT`,
`LINUX_REBOOT_CMD_RESTART` — the command words carry Linux's magic-2
values so a program compiled against `<sys/reboot.h>` elsewhere passes
the same constants. `POWER_OFF` performs an ACPI soft-off (QEMU exits),
`HALT` masks interrupts and parks the CPU, `RESTART` pulses the 8042
reset line and falls back to a triple fault. None of the three return.

**DIVERGENCE from Linux:** Linux gates `reboot(2)` on `CAP_SYS_BOOT`.
NeoOS has no uids or capabilities, so it gates on **caller is PID 1** —
any other caller gets `-1`. An unknown `cmd` returns `-1`
(kernel `-EINVAL`). NeoOS also has no `LINUX_REBOOT_CMD_CAD_ON/OFF`,
no `SW_SUSPEND`, and no `kexec`.

### `/sbin/init.nex` and `/etc/inittab`

The kernel starts `/sbin/init.nex` as PID 1 and nothing else. INIT reads
`/etc/inittab` — one `<mode> <path>` entry per line, `#` comments and
blank lines ignored — and:

| mode | behaviour |
|---|---|
| `spawn` | launch, do not wait |
| `wait` | launch and block until it exits before the next entry |
| `respawn` | launch, and relaunch each time it exits |

INIT then reaps children and reparented orphans in a `wait4(-1)` loop.
When that returns `-ECHILD` — every launched process and every orphan
has exited — INIT calls `reboot(LINUX_REBOOT_CMD_POWER_OFF)`.

**DIVERGENCE:** shutdown does **not** first send `SIGTERM`/`SIGKILL` to
surviving processes the way a real service manager does — INIT only
powers off once nothing is left to reap. A ported daemon that expects a
shutdown signal will not get one. If PID 1 itself exits, the kernel
panics (there is nothing to reap the machine or power it off).

The pty pool holds up to **256** concurrent ptys (was 16), and grows
into that: a slot's `struct pty` — which contains a whole `struct tty`,
three 1 KiB buffers — is built the first time that slot is used, so an
idle machine pays for a pointer array rather than sixteen ttys. Slot
structs are reused but never freed, deliberately: a pty is torn down
right after waking every reader blocked on it, and those threads wake up
inside it. `/dev/pts/N` now goes to three digits.

A process may hold up to **1024 threads** (was 16). The number comes
from the thread-stack layout: each thread's stack sits one
`THREAD_STACK_STRIDE` below the last, downward from `USER_STACK_TOP`,
and 1024 of them occupy 20 MiB of a 32 TiB gap — so what bounds it is
the per-process bitmap, not the address space. `thread_create` also had
a *userland* ceiling of 16 concurrent calls, which is gone.

PIDs are allocated from a bitmap with a **rising cursor that wraps**, so
a freed pid is not handed out again until the cursor comes back round to
it. Two consequences worth knowing when porting: pid numbers are not
dense (they climb, and a short-lived process does not immediately donate
its number to the next one), and after ~1M process creations they begin
to repeat. The old allocator did neither — it reused the most recently
freed pid *first*, and stopped allocating entirely at 2^20.

**`/proc` is synthetic, read-only, and minimal.** It provides
`/proc/<pid>/stat` and `/proc/<pid>/cmdline` and nothing else — no
`/proc/self`, no `meminfo`, no `mounts`. That is not an oversight: `ps`
is the only thing that has asked for `/proc`, and it asked by name, so
this provides what `ps` reads and stops. Divergences worth knowing:

- **`stat` reports `st_size` 0** for every `/proc` file, as Linux does.
  The content is rendered at *read* time.
- **Fields NeoOS does not track are `0`, not omitted.** `stat`'s format
  is positional, so a missing field would silently shift every later
  field into the wrong slot. `tty_nr`, the time and fault counters and
  the priority fields are all 0; `pid`, `comm`, state, `ppid`, `pgrp`
  and `session` are real.
- **Writes return `-EPERM`** rather than succeeding silently.
- **A process that exits between `open` and `read` reads as empty**
  (EOF) rather than erroring.

`TIOCSCTTY` makes a tty the caller's controlling terminal, and requires
the caller to be a session leader as on Linux. It is what lets a shell
take over a pty it inherited across `fork`: `pts_devfs_open` records the
session of whoever opened the slave *first*, which in the usual
arrangement is the parent that set the pty up. `setsid()` followed by
`ioctl(fd, TIOCSCTTY, 0)` on the inherited descriptor is the standard
sequence, and without it `ash` sees a foreground process group that is
not its own and signals itself with `SIGTTIN` until it stops.

**Path lookup is CASE-SENSITIVE.** `Foo` and `foo` are different names,
on every filesystem including FAT. FAT itself is a case-insensitive
format, and NeoOS used to inherit that; the VFS's semantics are NeoOS's,
and the case-sensitive filesystems planned later should not have to
fight a rule taken from FAT.

Two consequences:

- **FAT can only store one of `foo` and `FOO`.** They collide in the
  same 8.3 slot, so creating the second on a FAT volume fails even
  though the VFS considers them distinct names. That is FAT's limit
  surfacing through a VFS that no longer hides it.
- **VFAT's case flags are honoured on read.** A lowercase name that fits
  8.3 is stored uppercase with two bits of the NT-reserved byte marking
  it (`0x08` base, `0x10` extension) and *no* long-name entry — that is
  what `mcopy` writes. Ignoring those bits would make every lowercase
  path read back uppercase and fail to resolve. Names NeoOS creates take
  the long-name path instead, since `fits_83` rejects lowercase, so case
  round-trips by a single mechanism.

**Executables are `.nex`, with magic `\x7fNOX`** — ELF's shape with
three characters changed; everything from `e_ident[4]` on is unchanged
ELF64. The loader accepts **both** `ELF` and `NOX` and records which it
saw on the process, so `NOX` is a marker meaning "built for NeoOS"
rather than a rename. `tools/nexify.sh` stamps a copy at disk-image
time; the linker output in `build/` stays valid ELF so `objdump`,
`readelf` and `gdb` keep working.

**`/dev/tty` is the caller's controlling terminal**, not an alias for
the console. A session leader claims one with `TIOCSCTTY`; it is
inherited across `fork` and `spawn` and kept across `exec`. A process
with none gets `-ENXIO` from `open("/dev/tty")`, as on Linux.

**The filesystem layout** is `/bin` `/sbin` `/etc` `/dev` `/proc` `/tmp`
`/mnt` `/home` `/root` `/usr/tests` `/usr/share/test` `/var/tmp`. The
root holds directories only. `/tmp` is a ramfs, so tests that must write
to a real filesystem use `/var/tmp`.

**A pty's output has flow control.** A write into a full master queue
blocks until the master drains, and returns the partial count if
interrupted. It used to DISCARD what did not fit and report success for
it, so a program painting a screen faster than the terminal read it lost
bytes — and a byte lost inside an escape sequence turns the rest of that
sequence into visible text. Found by running `3d-ascii-viewer`, which
writes a full colour frame at a time.

**`nsh` is the NeoOS shell** (`/bin/nsh.nex`): a prompt, one line at a
time split on whitespace, the builtins `cd` `pwd` `echo` `env` `help`
`exit`, and `PATH` lookup for everything else. It reports a non-zero
exit status and survives an unknown command.

It deliberately has **no quoting, pipes, redirection, variables,
globbing or job control** — an argument containing a space cannot be
written. `busybox sh` is one word away when a session needs a real
shell, and a bad imitation of `ash` would be worse than none.

`PATH` lookup tries the bare name first and then the name with **`.nex`
appended**, so `busybox` finds `/bin/busybox.nex`. The bare name wins,
which is what `./prog` requires.

**`/etc/nshrc`** is read before the first prompt. Every non-blank,
non-comment line is a command nsh runs — that is the entire format, in
the spirit of `.bashrc`. "A list of commands" already covers showing a
banner or changing directory, so a second configuration syntax would be
one more thing to learn and to get wrong.

It ships printing `/etc/nsh.logo`, which the build generates from
`shared/neoos_logo.h` — the same art the kernel banner draws — so
editing the logo updates the boot banner and the shell greeting
together. Because the logo is ordinary shell output rather than
something painted outside the terminal, `clear` removes it like any
other output.

**Login and sessions.** `init` runs `/sbin/login.nex` on the terminal as
`god`; it authenticates against `/etc/passwd`, drops to the account's
`gid` then `uid`, sets `USER`/`HOME`/`SHELL`, and execs the shell. The
entry is `respawn`, so leaving the shell returns a fresh login prompt
rather than powering the machine off.

`/etc/passwd` is `name:uid:gid:home:shell:hash` — Linux's field order
for the first five, with the hash in the same file rather than a
separate `/etc/shadow`. There is no privilege boundary yet that a second
file would enforce, so splitting them would be security theatre. It
ships with `god` (uid 0) and `neo` (uid 1000).

Hashes are **`$6$` SHA-512-crypt**, verified by musl's own `crypt()`.
Nothing was ported and nothing hand-rolled: musl already ships
SHA-256-crypt, SHA-512-crypt, MD5 and bcrypt back-ends, all pure
computation with no files and no randomness. That is a rounds-based KDF
rather than the bare hash an earlier draft of this spec proposed.

Two properties worth stating, because both are easy to get wrong and
invisible when you do: the password is **never echoed** (login verifies
the terminal actually disabled echo and refuses to prompt if it did
not), and an **unknown user is refused exactly as a wrong password is**,
hashing against a fixed string so the two take the same path — answering
"no such user" faster tells an attacker which accounts exist.

**`/dev/urandom`, `/dev/random` and `getrandom(2)`** all draw from the
same kernel CSPRNG (`kernel/lib/rand.c`), and none of them ever block.
The two devices are identical, as they are on Linux since 5.6 — once the
pool is seeded `/dev/random` no longer blocks and returns what
`/dev/urandom` returns, so making them differ would invent a distinction
Linux has removed. Writes to either are accepted and discarded: there is
no pool to mix into, and failing the common `> /dev/urandom` idiom would
buy nothing.

`getrandom` **rejects unknown flags** rather than ignoring them — a
caller passing a flag this kernel does not implement is asking for a
guarantee it would not be getting. `GRND_NONBLOCK`, `GRND_RANDOM` and
`GRND_INSECURE` are accepted and have nothing to select between.

**Divergence worth knowing:** this is a CSPRNG seeded **once** at boot
(RTC, TSC, a stack address, and `RDRAND` where available), not an
entropy pool that reseeds. It is suitable for salts, cookies, stack
guards and ASLR offsets; it is not a source for long-term key material.

**Processes carry a `uid` and `gid`, and the superuser is `god`, uid 0.**
The name is NeoOS's; the number is Unix's, because every `uid == 0` test
in ported software depends on it. `getuid`/`geteuid`/`getgid`/`getegid`
report the real values — they answered a hardcoded 0 for everyone before
N3, so a program that reads them now gets a different answer.

There is **no `euid`/`egid` and no saved-uid**: nothing elevates
privilege yet, and a saved-uid model with no user of it is speculative.
`geteuid` is therefore `getuid`. Credentials are inherited across `fork`
and `spawn` and kept across `exec`.

`setuid`/`setgid`: **only god may change identity, and only downward.**
Anything else is `-EPERM` rather than a half-implemented approximation
of Linux's real/effective/saved juggling. Call `setgid` *before*
`setuid` — after the uid is dropped, the right to change the group is
gone too.

**Signals are permission-checked.** god may signal anything; anyone else
only processes with their own uid. That one rule is what makes "an
ordinary process cannot kill init" true, with no special case naming PID
1 — init runs as god. The check covers `sig == 0` too, so a caller that
may not signal a process cannot use the existence probe to learn whether
it exists. A broadcast (`kill(-1)`) *skips* what it may not signal
rather than failing, which is what that call means.

**`reboot` is god's**, not PID 1's. Linux gates it on privilege rather
than on being init, and the old rule is why nothing but init could power
the machine off.

`poll` and `select` are reachable from musl. They had never been mapped
in the shim at all, so every musl program that waited on a descriptor
got `-ENOSYS` — which is how an interactive BusyBox `ash` came to echo
the commands typed at it and then do nothing with them.

`getppid`, `uname` and `brk` exist (syscalls 73–75), added because
BusyBox measurably asked for them and nothing else did.

**Divergence: `uname` reports `sysname = "NeoOS"`, not `"Linux"`.** The
struct layout is Linux's exactly — six 65-byte NUL-terminated fields, no
padding — because a program compiled against Linux's `<sys/utsname.h>`
indexes into it directly. The *contents* are honest: a program that
switches on `sysname` takes its non-Linux path, which is the correct
outcome, since NeoOS is Linux-shaped rather than Linux and a configure
script told otherwise would choose paths this kernel does not implement.

**Divergence: `brk` never grows the break.** It returns the current
break for every request, which is exactly how Linux reports "the heap
cannot be extended", so callers fall back to `mmap` — which NeoOS
implements properly. musl's allocator does precisely this. A real `brk`
would be a second heap mechanism beside `mmap`, with its own vma, for an
interface Linux itself treats as legacy; if something turns up that
genuinely needs a growable break it should get one, and nothing has yet.

`poll()` and `select()` register on **each polled object** rather than
on one global queue, so a readiness change wakes only the callers that
asked about the object it happened to. This used to be a broadcast:
every poller in the system woke on any readiness change anywhere and
re-scanned. Measured over one boot, before and after, same suite:

| | broadcasts | poll wakeups | wakeups that found nothing |
|---|---|---|---|
| before | 5894 | 507 | 496 (96%) |
| after | 5998 | 8 | **0** |

Pipes, sockets, ttys, ptys, VTs and evdev clients each carry their own
poll head. Objects whose readiness never changes — regular files,
`/dev/null`, the framebuffer — share one head that is never notified.
An object with no head at all still works: its poller stays on the
global broadcast, which is now woken only for those callers.

`poll()` accepts up to `FD_TABLE_MAX` (16,384) descriptors — the size
of the process's descriptor table, since polling more than it can hold
open is meaningless. It previously refused anything over **16** with
`-EINVAL`; `select()` accepted them and silently dropped every ready
descriptor past the sixteenth. Both are fixed, and both are covered by
`polltrunc`.

File descriptors are allocated **lowest-available from 0**, as POSIX
requires — including 0, 1 and 2 once the process has closed them. This
is what makes `close(0); open(file)` (which is what shell input
redirection compiles down to) put the file on stdin. It was previously
lowest-available *from 3*, scanned from a per-bucket hint, so both the
redirection idiom and the reuse of a freed low descriptor returned the
wrong number.

`spawnv` is `spawn` with an argument vector, and `execv`/`execve` are
`exec` with one. Both copy the vector into the kernel before the new
address space is built — they must, because building it is what stops
the caller's pointers meaning anything, and in `exec`'s case the pages
holding those very strings are freed partway through.

The ceilings are shared by both, and are **refusals, not truncations**:

| limit | value | Linux analogue |
|---|---|---|
| arguments | 1024 | `MAX_ARG_STRINGS` (Linux: 2^31) |
| one argument | 4096 bytes | `MAX_ARG_STRLEN` (Linux: 32 pages) |
| whole vector | 256 KiB | `ARG_MAX` (Linux: typically 2 MiB) |

Exceeding any of them fails with `-E2BIG`, leaving the caller running.
This is a deliberate change from the earlier behaviour, which capped the
vector at 8 arguments of 128 bytes and silently DROPPED the rest: a
shell handed back a command line with its arguments quietly removed runs
the wrong command, which is worse than a failure it can report.

`spawn(path)` and `exec(path)` are the same calls with
`argv = {path, NULL}`.

`execve(path, argv, envp)` carries the environment, as does `spawnve`
(BB3). The entry stack is the full SysV shape — `argc`, `argv[]`, NULL,
`envp[]`, NULL, auxv — and the same three ceilings that bound argv bound
the environment. `execv`/`spawnv` pass an empty environment rather than
inheriting one, exactly as their names promise; a child that should
inherit needs `execve(path, argv, environ)`.

The environment itself originates in **init**, which is the only place
it can: nothing above PID 1 has one to pass down. It supplies `PATH`,
`HOME`, `TERM` and `PS1`, and every process inherits from there.

`fcntl` implements `F_GETFL`, `F_SETFL`, `F_DUPFD` and
`F_DUPFD_CLOEXEC`. `F_GETFD`/`F_SETFD` are accepted and ignored, and so
is `F_DUPFD_CLOEXEC`'s close-on-exec half, since nothing walks the
descriptor table at `exec` yet. The locking commands still return
`-EINVAL` rather than a silent success, because a caller that asked for
a lock and got one would act on a guarantee it never received.

`F_DUPFD` is not optional for a shell: BusyBox's `ash` moves the
terminal out of a script's way with `fcntl(fd, F_DUPFD_CLOEXEC, 10)`,
and when that returned `-EINVAL` it gave up on job control entirely
("can't access tty; job control turned off"). `F_GETFL` reports only
`O_NONBLOCK`; the access mode is not tracked per descriptor.

`O_NONBLOCK` is per-DESCRIPTOR, which is where POSIX puts it: two
descriptors on one pipe can disagree about it.

## `dup`, `dup2`, `dup3`

```c
int dup(int oldfd);                     /* <unistd.h> */
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);
```

Standard POSIX semantics: the new descriptor shares the open file
object (and its offset) with `oldfd`. `dup` returns the lowest free fd
**≥ 3**; `dup2`/`dup3` use the exact `newfd` given, closing whatever it
held first. `dup2(fd, fd)` returns `fd` unchanged if `fd` is valid;
`dup3` rejects `oldfd == newfd` with `-EINVAL` and accepts only
`O_CLOEXEC` in `flags` (recorded, not acted on — NeoOS does not close
fds at `exec` yet).

**Why these matter on NeoOS:** `open()` never returns fd 0, 1, or 2 —
the standard streams are bound once at process creation and the
allocator skips them (`docs/abi-compatibility.md` §3). So the only way
to rebind stdin/stdout/stderr — shell redirection, a terminal wiring a
child onto a pty slave — is `close(fd)` *is not enough*; you need
`dup2(newsrc, fd)`.

## Test Hooks (headless testing only)

NeoOS test builds (compiled with `-DNEOOS_TEST_HOOKS`, automatically set
by `make test`) expose two syscalls for deterministic testing in headless
environments:

```c
#include <neoos_test.h>

int neoos_test_inject_key(unsigned keycode, int pressed);
long neoos_test_migration_count(void);
int  neoos_test_parent_pid(int pid);
```

These are **not part of the stable ABI** and are **test-only**. They do
not exist in production builds, where both functions return `-ENOSYS`.

- `neoos_test_inject_key(keycode, pressed)` — injects a keyboard event
  as if it came from hardware. `keycode` is a Linux `KEY_*` constant;
  `pressed` is 1 (make) or 0 (break). The event fans out to open evdev
  clients and, if no grab is held, to the TTY. Returns 0 on success.

- `neoos_test_migration_count()` — returns the total number of user-thread
  migrations across all CPUs since boot. Used to verify the kernel's work-stealing
  scheduler is exercised. Returns -ENOSYS in production builds.

- `neoos_test_parent_pid(pid)` — returns the `parent_pid` of `pid`, or
  `-ESRCH` if there is no such process. Used by `orphantest` to observe
  orphan reparenting. Returns -ENOSYS in production builds.

## M1a: framebuffer, `poll`/`select`, and pseudo-terminals

The console-plumbing milestone (`docs/superpowers/specs/2026-08-31-m1a-console-plumbing-design.md`).

### `/dev/fb0` and `<linux/fb.h>`

A linear 32-bpp framebuffer, requested from GRUB via a Multiboot2 tag.
On a machine that refuses the mode, `open("/dev/fb0")` returns `-ENODEV`
and the kernel console stays in VGA text mode.

```c
int fd = open("/dev/fb0", O_RDWR);
struct fb_var_screeninfo v; ioctl(fd, FBIOGET_VSCREENINFO, &v);  // xres, yres, bits_per_pixel, red/green/blue bitfields
struct fb_fix_screeninfo f; ioctl(fd, FBIOGET_FSCREENINFO, &f);  // smem_start, smem_len, line_length
void *p = mmap(0, f.line_length * v.yres, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
```

`struct fb_var_screeninfo` / `struct fb_fix_screeninfo` match Linux's
x86-64 layout. Byte `read`/`write`/`lseek` on the fd also work.

**DIVERGENCE.**
- **No mode setting.** `FBIOPUT_VSCREENINFO` returns `-EINVAL` unless
  the requested mode equals the current one. The mode is fixed at boot.
- **32-bpp packed RGB only.** No 8/16/24-bpp, no palette, no panning,
  no acceleration ioctls.
- **Claiming the screen.** A program that wants the framebuffer to
  itself sets `KDSETMODE`/`KD_GRAPHICS` on its `/dev/ttyN` — Linux's
  mechanism, at Linux's ioctl numbers — after which the kernel console
  stops painting that VT and repaints it in full on the return to
  `KD_TEXT`.

  **DIVERGENCE: the claim is released by the kernel, not negotiated
  with the process.** Linux uses `VT_SETMODE`/`VT_PROCESS` and a signal
  handshake, so an application is *told* when it loses the screen.
  NeoOS instead:

  - restores `KD_TEXT` when the last fd that made the claim is released
    — including when the process died without tidying up, which is the
    case that otherwise leaves the machine with no console; and
  - refuses `write()` and `mmap()` on `/dev/fb0` with **`-EBUSY`** when
    the caller may not paint. A process that claimed a VT may paint only
    while that VT is on display; a process that claimed nothing may
    paint only while nobody else owns the screen (which is what keeps
    programs that never call `KDSETMODE` working).

  `VT_SETMODE` and `VT_RELDISP` are accepted for ABI compatibility but
  the process-mode handshake is not implemented, so an application is
  never signalled.

  **LIMITATION:** the gate applies to the `mmap` *call*, not to later
  faults on an established mapping. A process that mapped the
  framebuffer while it owned the screen keeps a usable mapping across a
  VT switch. Revoking that would require the switch path to unmap every
  framebuffer mapping; for now a compositor is expected to cooperate by
  stopping when it loses focus.

### `/dev/input/event0` and `/dev/input/event1`

Linux evdev character devices, 24-byte `struct input_event` in Linux's
x86-64 layout. `event0` is the AT keyboard; **`event1` is the PS/2
mouse**, reporting `EV_REL` (`REL_X`, `REL_Y`, and `REL_WHEEL` when the
IntelliMouse negotiation succeeds) and `EV_KEY` (`BTN_LEFT`,
`BTN_RIGHT`, `BTN_MIDDLE`). Both sign conventions follow Linux: `REL_Y`
is positive downward, and `REL_WHEEL` is positive upward, each the
opposite of what the PS/2 wire carries.

Supported ioctls: `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`,
`EVIOCGKEY`, `EVIOCGRAB`, and `EVIOCGBIT` for `0`, `EV_KEY`, `EV_REL`
and `EV_MSC`.

A blocking `read()` blocks until an event arrives; `O_NONBLOCK` returns
`-EAGAIN`. `poll`/`epoll` work on both nodes.

**DIVERGENCES:**

- `EVIOCGBIT(EV_KEY)` reports the keys currently **held down**, not the
  set of keys the device is capable of reporting. An application using
  it to decide "is this a keyboard" will get an empty bitmap from an
  idle device.
- No `EVIOCGABS`/`EV_ABS` and no `EVIOCGPHYS`/`EVIOCGUNIQ` (both return
  `-ENOENT`): there is no absolute pointing device and no topology to
  report.
- `EVIOCSCLOCKID` returns `-EINVAL`; timestamps are boot-epoch based.
- A grab (`EVIOCGRAB`) is per device. Grabbing the keyboard stops
  keystrokes reaching the tty; it has no effect on the mouse.

### `poll` / `select`

```c
#include <poll.h>
int poll(struct pollfd *fds, unsigned long nfds, int timeout_ms);
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);
```

`struct pollfd` and the `POLL*` values are Linux's. Both block until a
polled fd is ready or the timeout elapses, waking on any system-wide
readiness change.

**DIVERGENCE.**
- **Flag subset:** `POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP`, `POLLNVAL`.
  No `POLLPRI`, `POLLRDHUP`, `POLLRDNORM`/`POLLWRNORM`.
- **No `epoll`.** No `ppoll` / `pselect6` signal mask (the arg is
  accepted and ignored).
- **`nfds` caps:** `poll` at 16, returning `EINVAL` past that — a
  **divergence from Linux**, which has no such limit. CS4 removes it.
  `select` has **no cap below `FD_SETSIZE`** as of CS2: it counts the
  set bits the caller described, allocates a descriptor array for
  exactly that many, and reports every ready one, which is Linux's
  behaviour.
  Until CS2 it collected into a fixed 16-entry array and *silently
  dropped* everything past the sixteenth interesting fd — no error, no
  truncation flag. That was a correctness bug, not a documented limit;
  `userland/polltrunc.c` is its regression test.
- **Timeout resolution is one 10 ms tick.**
- **The wake is a global broadcast:** every `poll`/`select` caller wakes
  on *any* pipe/socket/tty/evdev readiness change and re-scans its own
  fds. Correct, and free at NeoOS's process count; a scaling concern a
  later milestone can address with per-object registration.

### Pseudo-terminals: `/dev/ptmx`, `/dev/pts/N`

```c
int m = posix_openpt(O_RDWR);           // == open("/dev/ptmx", O_RDWR)
grantpt(m); unlockpt(m);                // no-ops, return 0
int n; ptsname_r(m, buf, len);          // "/dev/pts/<n>", n from ioctl(m, TIOCGPTN)
int s = open(buf, O_RDWR);              // the slave: a full line-discipline tty
```

The master (`m`) is a raw byte stream: `read(m)` returns what a program
running on the slave printed (with `ONLCR` applied), `write(m)` feeds
the slave's line discipline as if typed (echo, canonical assembly, the
signal characters). `TCGETS`/`TCSETS`/`TIOCGWINSZ` on the slave behave
as on the console tty; on a pipe they return `-ENOTTY`, so `isatty()` is
correct.

**DIVERGENCE.**
- **`grantpt` / `unlockpt` are no-ops.** NeoOS has no pts permission or
  lock model; the slave is openable immediately.
- **`TIOCSWINSZ` stores the size but sends no `SIGWINCH`.** The M1a
  terminal is fixed-size; M2 wires session hang-up and resize signals.
- **`close(master)` sends no `SIGHUP`** to the slave's foreground group
  — a blocked slave `read` just returns EOF. Again M2's job.
- **16 ptys**, allocated from a fixed pool; the 17th `open("/dev/ptmx")`
  returns `-ENFILE`.
- **`/dev/pts` is dynamic devfs, not a `devpts` mount.**

The evdev "`read` never blocks" note above is now backed by working
`poll`/`select` — a reader waits with those.

### `NEOOS_TIOCSACTIVE` — the active terminal (M1b)

```c
ioctl(master_fd, NEOOS_TIOCSACTIVE, (void *)1);   /* claim */
ioctl(master_fd, NEOOS_TIOCSACTIVE, (void *)0);   /* release */
```

A **NeoOS extension, no Linux equivalent** (the number `0x4E454F01` is
deliberately outside Linux's `0x54xx` TIOC range). On a pty **master**,
claiming does two things: cooked keyboard input is routed to this
master's slave line discipline instead of `/dev/CONSOLE`, and the
kernel stops painting the framebuffer for its own console output
(serial output is unaffected). A userland terminal (`/bin/term.nex`) claims
it on startup so keystrokes reach the shell it hosts and it owns the
pixels.

Released automatically when the **last** master fd is closed (a
`fork()` duplicates the fd; the child closing its inherited copy does
**not** release). A kernel panic forcibly reclaims the framebuffer and
the console before its dump, so a fault always paints over the
terminal's grid.

**DIVERGENCE:** there is exactly one active *userland* terminal at a
time. A second claim while another master holds it simply moves the
claim. This is independent of the kernel virtual terminals below: a
claim suppresses the kernel's own painting, and `Alt+Fn` still switches
VTs underneath it.

## Virtual terminals: `/dev/tty1` … `/dev/tty6` (M1c-3)

Six kernel virtual terminals, one visible at a time. Each is a full
terminal — its own text grid, scrollback, line discipline and
`termios` — so `TCGETS`, `TIOCGWINSZ`, canonical editing and the signal
characters all work on any of them, foreground or background.

| Path | What it is |
|---|---|
| `/dev/tty1` … `/dev/tty6` | the six VTs, addressed individually |
| `/dev/tty0` | whichever VT is active *right now*, re-resolved per call |
| `/dev/CONSOLE`, `/dev/TTY` | also follow the active VT |

Writing to a background VT is legal and silent: the text lands in that
VT's grid and appears when it is switched to. The kernel log and the
panic dump land on VT 1, and a fault forcibly switches to it.

**Keys** (consumed by the kernel — they never reach an evdev client or a
line discipline, even one holding an `EVIOCGRAB`):

- `Alt+F1` … `Alt+F6` — switch to that VT.
- `Shift+PageUp` / `Shift+PageDown` — scroll the active VT's scrollback
  by half a screen.

### `VT_*` / `KD*` ioctls

Linux's numbers and semantics, on any `/dev/ttyN` fd (`/dev/tty0` is the
usual one). Anything outside this set falls through to the ordinary
terminal ioctls on the same fd.

| ioctl | Number | Behaviour |
|---|---|---|
| `VT_ACTIVATE` | `0x5606` | arg 1..6; switches immediately |
| `VT_WAITACTIVE` | `0x5607` | blocks until that VT is active |
| `VT_GETSTATE` | `0x5603` | fills `struct vt_stat` |
| `VT_OPENQRY` | `0x5600` | writes an `int` |
| `VT_GETMODE` / `VT_SETMODE` / `VT_RELDISP` | `0x5601`/`0x5602`/`0x5605` | accepted, inert |
| `KDSETMODE` / `KDGETMODE` | `0x4B3A`/`0x4B3B` | `KD_TEXT` (0) / `KD_GRAPHICS` (1) |

`KD_GRAPHICS` stops the kernel painting that VT; its grid still updates,
and `KD_TEXT` repaints it. That is how a userland display server will
take the screen without the kernel drawing over it.

**DIVERGENCE.**
- **6 VTs, not Linux's 63.** `VT_ACTIVATE` outside 1..6 is `-EINVAL`.
- **`VT_SETMODE` / `VT_RELDISP` are accepted and do nothing.** Every
  switch is `VT_AUTO` and instant; the `VT_PROCESS` handshake (a process
  that asks to be signalled before a switch and acknowledges it) is not
  implemented, so `SIGUSR1`/`SIGUSR2` are never sent on a switch and
  `v_signal` in `struct vt_stat` is always 0. A display server therefore
  cannot yet veto or defer a console switch.
- **`VT_OPENQRY` never fails.** There is no pool of free VTs to allocate
  from: all six exist from boot, so it reports the active one rather
  than "the first unused".
- **`v_state`** reports all six as allocated, always.
- **No raw or medium-raw keyboard modes.** `KDSKBMODE`, `K_RAW`,
  `K_MEDIUMRAW` and the `KDGKBENT` keymap ioctls do not exist; a program
  wanting scancodes reads `/dev/input/event0`.
- **No `/dev/vcs*` / `/dev/vcsa*`** screen-content devices.
- **`/dev/CONSOLE` follows the active VT; Linux's `/dev/console` does
  not.** On Linux `/dev/console` is *bound* at boot to one device and
  only `/dev/tty0` tracks the foreground VT. Here both track it, which
  means a process holding a `/dev/CONSOLE` fd across a VT switch is
  afterwards talking to a **different terminal** — its `termios` and its
  output both move. (Seen for real: a test that set `ICANON` off through
  `/dev/CONSOLE` and read it back after another process called
  `VT_ACTIVATE` got two different terminals and the setting appeared not
  to stick.) A program that wants a stable terminal must open a specific
  `/dev/ttyN`, not `/dev/CONSOLE`.

## Audio: `/dev/snd/controlC0`, `/dev/snd/pcmC0D0p` (AC97)

NeoOS's one audio backend is an AC97 controller (QEMU's `-device
AC97`, Intel 82801AA emulation). The userland surface is ALSA-shaped:
real Linux `SNDRV_*` ioctl numbers and `struct snd_pcm_hw_params`/
`snd_pcm_sw_params`/`snd_ctl_card_info` layouts (verified against
upstream Linux's `include/uapi/sound/asound.h`), reached through the
existing `open`/`write`/`ioctl` syscalls — no new syscall numbers.

- `open("/dev/snd/controlC0", ...)` — the card's control device.
  `ioctl(fd, SNDRV_CTL_IOCTL_PVERSION, &ver)` returns `0x02000108`.
  `ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info)` fills `struct
  snd_ctl_card_info` with `id="AC97"`, `driver="neoos-ac97"`,
  `name="AC97 (NeoOS)"`. Every other control ioctl is `-ENOTTY`.
- `open("/dev/snd/pcmC0D0p", ...)` — the playback PCM device.
  - `ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp)` — accepts exactly one
    configuration (`SNDRV_PCM_FORMAT_S16_LE`, 2 channels, 48000 Hz) and
    narrows every field to that fixed value on success; any request
    outside that range returns `-EINVAL`.
  - `ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp)` — always succeeds; the
    buffer/period sizes are fixed, so there is nothing to negotiate.
  - `ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0)` — resets the write cursor.
  - `ioctl(fd, SNDRV_PCM_IOCTL_START, 0)` / `SNDRV_PCM_IOCTL_DROP` —
    marks the stream started/stopped (bookkeeping only; the DMA engine
    itself runs continuously once `ac97_init()` brings the device up).
  - `write(fd, pcm_data, len)` — copies raw interleaved 16-bit stereo
    PCM frames into the next free DMA segment, blocking until space is
    available if the ring is full.

### DIVERGENCE

- **Fixed format only.** `SNDRV_PCM_IOCTL_HW_PARAMS` accepts exactly
  one configuration — 16-bit signed LE, stereo, 48000 Hz — and returns
  `-EINVAL` for anything else. A real driver reports a richer (but
  still hardware-limited) capability set; NeoOS's AC97 driver reports
  the narrowest possible one. An application that queries capabilities
  before asking for this exact format works correctly; one that
  assumes a wider format set gets `-EINVAL` where Linux might have
  accepted its request.
- **Playback only.** `/dev/snd/pcmC0D0c` (capture) does not exist —
  opening it is `-ENOENT`, the same as any other absent device, not a
  special-cased divergence.
- **No mixer tree.** `/dev/snd/controlC0` answers `PVERSION` and
  `CARD_INFO` only; every other control ioctl is `-ENOTTY`. A real
  ALSA mixer (volume controls, jack detection, etc.) does not exist.
- **No `mmap`-based playback.** Real ALSA clients commonly prefer
  mmap'd ring-buffer access; only the `write()` path is supported here.
- **QEMU-emulated hardware only.** This driver has never been run
  against real AC97 silicon — only QEMU's `-device AC97` emulation,
  the same validation story as every other NeoOS driver.
