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
  or a negative `<errno.h>` code on failure.
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
- `EMFILE` (24) — the process's file descriptor table is full (16
  entries, of which 0/1/2 are the standard streams, so 13 files at
  once, maximum).
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

## SSE/SSE2/SSE3/SSE4

User-mode programs may freely use SSE, SSE2, SSE3, SSSE3, SSE4.1, and
SSE4.2 floating-point and vector instructions (including via GCC's
`<xmmintrin.h>`/`<emmintrin.h>`/`<smmintrin.h>` intrinsic headers) --
there is no library function to call for this, it's a CPU/build
capability, not an API. Each process's FPU/SSE register state is
saved and restored across context switches automatically. MMX and
AVX/AVX2/AVX-512 are not supported.
