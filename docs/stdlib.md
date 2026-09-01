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
- `void yield(void)` — voluntarily gives up the remaining CPU time
  slice to the scheduler.
- `int spawn(const char *path)` — builds a fresh process directly from
  the ELF executable at `path` (NUL-terminated) and returns its PID,
  or `-1` on failure. NeoOS-specific: not `fork`+`exec`.
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
- `int exec(const char *path)` — replaces the calling process's
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
- **No `sched_setaffinity`.** There is no way to pin a thread to a CPU
  yet, so a program cannot make `sched_getcpu` stable.

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

- **Only `FUTEX_WAIT` and `FUTEX_WAKE` exist.** `FUTEX_REQUEUE` /
  `FUTEX_CMP_REQUEUE` are absent, so `pthread_cond_broadcast` wakes
  every waiter instead of moving them to the mutex's queue — correct,
  but a thundering herd. The `*_BITSET` operations, `FUTEX_WAKE_OP`,
  and priority-inheritance futexes (`FUTEX_LOCK_PI` and friends) are
  absent too. Anything else returns `-ENOSYS`.
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

Underneath is a real IPv4/UDP path over a loopback device: every
datagram carries an IPv4 header with a verified checksum and a UDP
header with a verified pseudo-header checksum, and is demultiplexed by
port. It is not a shortcut between two buffers.

### Divergences, and what is simply absent

- **One interface, and it is loopback.** Only `127.0.0.0/8` and
  `INADDR_ANY` have a route; anything else is `-ENETUNREACH`. There is
  no NIC driver, no link layer, no ARP, no routing table.
- **No TCP.** `socket(AF_INET, SOCK_STREAM, ...)` returns
  `-EPROTONOSUPPORT` rather than quietly giving a datagram socket: a
  program handed message boundaries where it expects a stream corrupts
  its own protocol, and a clean refusal is far better than that. So
  there is no `listen`, `accept` or `shutdown` either.
- **AF_INET only.** No `AF_UNIX`, no `AF_INET6`.
- **No `MSG_*` flags at all**, and the header deliberately does not
  define them. `flags` must be 0. In particular there is no
  `MSG_DONTWAIT` and no `MSG_PEEK`, and `MSG_TRUNC` is not available to
  report a truncated datagram — `recvfrom` returns what it delivered,
  and the rest of the message is discarded.
- **No `setsockopt`/`getsockopt`**, so no `SO_REUSEADDR`, no
  `SO_RCVBUF`, no timeouts. The receive buffer is 64KiB per socket and
  a datagram that does not fit is dropped, as UDP permits.
- **No `select`/`poll`/`epoll`.** A socket can only be read by
  blocking in `recvfrom`, so a program that must wait on several at
  once needs a thread per socket.
- **No ICMP.** There is no `ping` and no port-unreachable reply: a
  datagram sent to a port nobody has bound is silently dropped.
- **No raw sockets**, which is also why the absent ICMP cannot be
  noticed from userland.
- **Errors are returned directly as negative values**, per this
  library's convention: `socket()` returns `-EAFNOSUPPORT`, not `-1`
  with `errno`.
- **`inet_ntoa` is spelled `inet_ntoa_r`** and takes the output buffer.
  The standard one returns a pointer to a static buffer, which is not
  thread-safe; NeoOS has threads and no reason to reproduce that.

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

### `/SBIN/INIT` and `/ETC/INITTAB`

The kernel starts `/SBIN/INIT` as PID 1 and nothing else. INIT reads
`/ETC/INITTAB` — one `<mode> <path>` entry per line, `#` comments and
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

`spawnv` is `spawn` with an argument vector: at most **8 arguments of
128 bytes each**, copied into the kernel before the new address space
is built. `spawn(path)` is the same thing with `argv = {path, NULL}`.
`exec` still has no argument vector — a program `exec`s into
`argv[0] = path` and nothing else.

`fcntl` implements `F_GETFL` and `F_SETFL`, which is enough to turn
`O_NONBLOCK` on and off. `F_GETFD`/`F_SETFD` are accepted and ignored,
since nothing closes descriptors at `exec` yet. Everything else —
`F_DUPFD`, the locking commands — returns `-EINVAL` rather than a
silent success, because a caller that asked for a duplicate descriptor
and got one would use fd -1 as if it were open. `F_GETFL` reports only
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
- **The framebuffer is shared with the in-kernel console.** Until M1b's
  userspace terminal takes over, `fbcon` is actively drawing on it, so a
  userland program that mmaps `/dev/fb0` and the kernel will both write
  pixels. M1b resolves this by moving all rendering to userspace.

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
- **`nfds` caps:** `poll` at 16; `select` translates at most 16 set
  bits. `EINVAL` past that. (Enough for the M1b terminal, which polls
  two.)
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
(serial output is unaffected). A userland terminal (`/BIN/TERM`) claims
it on startup so keystrokes reach the shell it hosts and it owns the
pixels.

Released automatically when the **last** master fd is closed (a
`fork()` duplicates the fd; the child closing its inherited copy does
**not** release). A kernel panic forcibly reclaims the framebuffer and
the console before its dump, so a fault always paints over the
terminal's grid.

**DIVERGENCE:** there is exactly one active terminal at a time and no
virtual-console switching (`chvt`, `VT_ACTIVATE`). A second claim while
another master holds it simply moves the claim.
