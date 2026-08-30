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
  original code.
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
  end of directory. `d->name` is an 8.3 name and `d->type` is
  `DT_REG`, `DT_DIR`, or `DT_CHR`. The returned pointer is into the
  `DIR`'s own buffer and is invalidated by the next `readdir` or
  `closedir` on that `DIR`.
- `int closedir(DIR *d)` — closes the directory. Returns 0, or a
  negative `<errno.h>` code.
- `int getdents(int fd, struct dirent *buf, int count)` — the raw
  syscall the three functions above are built on. Fills up to `count`
  entries from a directory fd, returning how many were written, `0` at
  end of directory, or `-EBADF`/`-ENOTDIR`.

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

### Divergences from POSIX

- **`wait` remains NeoOS-native** (one pid, a bare exit code) and lives
  **beside** `wait4` rather than being replaced by it. Neither
  supersedes the other.
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
- **`AT_RANDOM` is not random.** There is no entropy source; the
  sixteen bytes are derived from the tick counter and the addresses at
  hand, so they differ between processes and are worthless against an
  attacker. musl uses them to seed its stack guard, which is therefore
  guessable. A real RNG is the fix and is not written yet.
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
