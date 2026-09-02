# BusyBox track — revised plan

**Supersedes the BusyBox half of**
`docs/superpowers/specs/2026-09-01-busybox-and-dynamic-linking-roadmap.md`.
That file's decomposition still stands; this one revises the **order**,
drops what has since landed, and adds a blocker it did not know about.

**Goal (unchanged):** an interactive BusyBox `ash` on the framebuffer
terminal — `cd`/`ls`/`cat`/pipes/redirection/job control.

**Change of course:** the roadmap sequenced dynamic linking (DL1–DL3)
ahead of BusyBox. The user has chosen to take **BusyBox first**; DL is
deferred, not cancelled. This is coherent — the roadmap already records
that BusyBox is built **static**, so it never depended on DL.

---

## What changed since the roadmap was written

### 1. A blocker the roadmap did not know about — `fork` loses the vma list

`fork_task()` duplicates page tables but **never copies `parent->vmas`,
and never copies `p->mmap_next`**. `proc_alloc()` leaves the child with
an empty vma list and `mmap_next = MMAP_BASE`.

Verified on hardware-equivalent QEMU, 2026-09-01, with a temporary probe
in `mmaptest`:

```
[probe] child after fork on an untouched mmap page: 88 (71=ok, 88=SIGSEGV)
[probe] child fresh mmap RESTARTED AT MMAP_BASE (parent's mmap_next was far above)
```

So, after `fork()`, in the child:

- a fault on any mmap'd page the **parent had not already touched** finds
  no vma and raises **SIGSEGV**;
- the next `mmap()` hands back `MMAP_BASE` again, which may already be
  occupied by an inherited mapping — a silent alias;
- `munmap()` of an inherited region returns 0 having unmapped nothing
  (the frames leak until exit);
- `mprotect()` of an inherited region is a silent no-op.

Existing tests miss this because they only touch pages the parent
touched first. **`ash` forks for every external command, and musl's
mallocng gets its heap from `mmap`** — a forked shell that allocates
anything the parent had not already faulted dies immediately. Nothing
else in the track matters until this is fixed, so it becomes **BB0**.

### 2. Roadmap items that are already done

- **`O_CREAT` `0x100` → `0x40`** — already fixed (Phase 14).
  `kernel/syscall/syscall_internal.h` and `lib/include/fcntl.h` both say
  `0x0040`. Delete this from BB1.
- **`dup`/`dup2`/`dup3`** — landed as syscalls 70–72 in M1b-3. BB1 keeps
  only `fcntl(F_DUPFD)` on top of them.
- **`mprotect` preserving page contents** and **`PROT_NONE`** — fixed
  today (`7313b77`). A shell's allocator depends on both.

### 3. Measured syscall inventory

73 numbers in use (0–72); `SYS_MAX` 73. Present and relevant: `fork`,
`exec` (no argv), `spawnv` (argv, no envp), `wait4`, `setpgid`/`getpgid`/
`setsid`/`getsid`, `fcntl` (F_GETFL/F_SETFL only), `dup`/`dup2`/`dup3`,
`poll`/`select`, `stat` family, `chdir`/`getcwd`, `ioctl`,
`clock_gettime`, `nanosleep`, `reboot`.

**Absent, and on BusyBox's path:** `getppid`, `getuid`/`geteuid`/
`getgid`/`getegid`, `umask`, `uname`, `rename`, `rmdir`, `ftruncate`,
`access`/`faccessat`, `readlink`, `symlink`/`link`, `chmod`/`chown`,
`utimensat`, `getrandom`, `clock_nanosleep`, `sync`/`fsync`,
`gethostname`/`sethostname`, `openat`.

`SIGTTIN`/`SIGTTOU` are defined and treated as stop signals, and
`struct tty` already carries `fg_pgid` — but **nothing generates them**,
which is BB2's job.

---

## Revised order

```
BB0.  fork duplicates the address-space bookkeeping   <- BLOCKER, first
BB1.  Build-and-run spike: /bin/busybox to first -ENOSYS   <- DONE
BB2.  The measured syscall set (was BB1 + BB4)             <- DONE
BB3.  execve(path, argv, envp)                            <- DONE
BB4.  Job control                                     (was BB2)  <- DONE
BB5.  Minimal /proc                                   (was BB3)  <- DONE
BB6.  Interactive shell bring-up                          <- DONE
```

**The one real change of method: BB1 comes before the syscall work.**
The roadmap listed ~25 syscalls BusyBox was *predicted* to need, and
sequenced BB5 (the build) last, after all of them. That is backwards for
a port: the cheapest way to learn what a binary needs is to run it. A
spike that links `busybox` and runs it until the first `-ENOSYS` costs a
day and replaces the prediction with a list. Everything after BB1 is
then work we know is required, in the order the binary actually demands
it.

The risk of the roadmap's order is not that the list is wrong in
principle — it is that we would implement `utimensat` and `sethostname`
before discovering that, say, `ash`'s startup needs something not on the
list at all.

---

## BB0 — `fork` duplicates the address-space bookkeeping — **DONE** (`080ce08`)

Landed with a second half the survey had not predicted: `exec_task` had
the mirror-image bug, freeing the old address space while leaving
`p->vmas` describing it, so a fault at a dead address was answered with
a fresh zero page instead of SIGSEGV. Both are covered by `mmaptest`,
and both tests were confirmed to fail before the fix.

Two pieces of collateral, each its own commit: `vtswitchtest` became a
`wait` entry (`3a3b7dd`) because the active VT is global state and
`/dev/CONSOLE` follows it, which made `ttytest` flaky; and `init` now
reports how much of `/etc/inittab` it actually parsed (`8982891`), after
a run booted a silent prefix of the system on a short read.

- Copy the vma list in `fork_task()`: allocate a `struct vma` per
  parent entry, in order, and copy `start`/`end`/`prot`/`flags`. Copy
  `p->mmap_next` too.
- Failure is a rollback, not a partial child: the existing failure paths
  free `child_pml4_phys` and return 0, and the vma copy must join them.
- `VMA_PHYS` mappings (the framebuffer) are already copied verbatim into
  the child's page tables by `fork_duplicate_user_pages`; their vmas
  must be copied with `VMA_PHYS` intact so the child's `munmap` does not
  try to free device frames.
- When file-backed vmas arrive (DL1), this is where the vnode reference
  is taken. Leave the hook obvious.

**Test (`mmaptest`):** the probe above, promoted to a real check — a
child must read an inherited-but-untouched mmap page, write it, `munmap`
it, and `mprotect` it; and a child's fresh `mmap` must not alias a
region it inherited.

**Exit:** `make test` green, gauntlet green.

## BB1 — build-and-run spike

- `third_party/busybox` as a submodule; `third_party/busybox-config/`
  with the NeoOS `.config`, `apply.sh`, and any port patches — mirroring
  `third_party/shim/`, so the submodule stays a pristine checkout.
- Static, against `third_party/musl/lib/libc.a` + the shim,
  `-mcmodel=large`, linked at the userland convention base.
- `CONFIG_FEATURE_SH_STANDALONE` + `CONFIG_FEATURE_PREFER_APPLETS`: one
  `/bin/busybox`, applets dispatched internally. FAT16 has no symlinks
  and the boot disk is 32 MiB, so the multi-binary layout is not
  available. One static busybox with ~30 applets is ~1 MiB — it fits.
- **Deliverable is the failure list, not a working shell.** Run
  `busybox true`, then `busybox echo`, then `busybox sh -c :`, and
  record every `-ENOSYS` and every wrong-answer. The shim returns
  `-ENOSYS` for unmapped numbers already, so an unmapped call is a clean
  diagnosis rather than a crash.

**Exit:** a written list of what BusyBox actually calls, checked into
this file as BB2's scope. No marker required — this milestone's output
is knowledge.

### BB1 result — **DONE** (2026-09-02)

BusyBox 1.37.0, static against NeoOS's musl, 324 KB, 76 config options,
`/bin/busybox.nex`. `userland/bbspike.c` runs nine invocations at boot
and prints how each ended. **All nine now succeed:**

| invocation | result |
|---|---|
| `busybox true` | exit 0 |
| `busybox echo ...` | prints, exit 0 |
| `busybox uname -a` | exit 0, prints a **blank line** |
| `busybox pwd` | prints `/`, exit 0 |
| `busybox ls /` | lists the volume, exit 0 |
| `busybox cat /etc/inittab` | prints the file, exit 0 |
| `busybox sh -c :` | exit 0 |
| `busybox sh -c 'echo shell works'` | prints, exit 0 |
| `busybox sh -c 'busybox echo external'` | prints, exit 0 — **ash forks and execs** |

So `ash` runs, and runs external commands. That last row failed at first
with `sh: busybox: Function not implemented`, and the diagnosis was
wrong twice before it was right: the `execve` shim mapping had been
written hours earlier and was correct, but `$(MUSL_LIB)` depended only
on its own absence, so `libc.a` was never rebuilt and every musl program
kept calling the old library. The Makefile dependency is fixed, and that
is the finding worth keeping from BB1 — not a missing syscall but a
build that could not tell you the truth about which syscalls it had.

**The measured missing set.** The shim now reports every unmapped number
to `/dev/kmsg` once (`[shim] ENOSYS <n>`) instead of silently returning
`-ENOSYS`, which is what turned guesswork into a list. Across the whole
suite plus all nine BusyBox invocations, exactly **three** numbers came
up:

| Linux nr | call | why BusyBox wants it |
|---|---|---|
| 12 | `brk` | musl's allocator probes it before falling back to `mmap` |
| 63 | `uname` | `uname -a`, which is why it printed a blank line |
| 110 | `getppid` | `ash` startup |

That is a far shorter list than the roadmap's predicted ~25, and it is
measured rather than predicted — which was the whole argument for taking
BB1 before BB2. The predicted list (`rename`, `rmdir`, `access`,
`readlink`, `chmod`, `utimensat`, `getrandom`, `openat`, ...) is not
wrong so much as *not yet reached*: nothing exercised so far needs any
of it. BB2 implements the three that are real, and the rest when a
session demands them.

## BB2 — the measured syscall set

Whatever BB1 found, in the order BusyBox hits it. The roadmap's
predicted list is the starting point and the fallback:

- `getppid`; `getuid`/`geteuid`/`getgid`/`getegid` → 0 (single-user
  system, 0 is the honest answer); `umask` stored and reported,
  honoured nowhere (FAT has no mode bits — recorded divergence);
  `uname` with Linux's 65-byte-field `struct utsname`.
- `fcntl(F_DUPFD)` on the existing `fd_table_dup2`.
- `rename`, `rmdir`, `ftruncate` — real, on FAT.
- `access`/`faccessat` — existence plus always-true rwx.
- `readlink` → `-EINVAL`; `symlink`/`link` → `-EPERM` (FAT cannot).
- `chmod`/`fchmod`/`chown`/`fchown`/`utimensat` — **accept and return
  0**. `-EPERM` would make `cp -p`, `install` and `tar -x` fail
  spuriously. Recorded divergence.
- `getrandom` from the kernel RNG; `clock_nanosleep`; `sync`/`fsync`/
  `fdatasync`; `gethostname`/`sethostname` (default `"neoos"`);
  `openat` with at least `AT_FDCWD`.

Every one gets a shim row (or a `lib/` wrapper if NeoOS-native) and a
`docs/stdlib.md` entry or divergence note, per CLAUDE.md.

**Test:** a `tier2test` covering the real ones and asserting the
accept-and-ignore stubs return 0.

## BB3 — `execve(path, argv, envp)`

- Extend `exec` (13) in place to take argv **and envp**; NeoOS's numbers
  are its own. `spawn` (4) and `spawnv` (51) stay for `lib/`.
- `build_initial_stack` learns to lay down a real environment
  (`envp[0]` is `NULL` today).
- The sibling-thread termination `exec` needs is **already done**
  (`7313b77`).
- Shim maps `LX_EXECVE` → `NEO_EXEC` with the usual path reshape and the
  argv/envp arrays forwarded.

**Test:** `execve` replaces the image and the new image sees the passed
argv and envp; `dup2`-based redirection round-trips through a pipe.

## BB4 — job control

- Enforce `tcsetpgrp`/`tcgetpgrp` on `struct tty` (`fg_pgid` exists):
  gate a background-pgrp read, and a background write when `TOSTOP`.
- **Generate** `SIGTTIN`/`SIGTTOU` — the stdlib note currently says they
  never are; this milestone changes that and the note.
- Controlling terminal: `TIOCSCTTY`, session leader claims it,
  `/dev/tty` resolves to it, the tty tracks its `sid`.
- Orphaned process groups with stopped members get `SIGHUP` then
  `SIGCONT`.
- Verify the stop path wakes `child_waiters` and encodes `WIFSTOPPED`
  for `WUNTRACED`/`WCONTINUED`.

**Test:** `jobctltest` — a child stopped by `SIGTSTP`, parent sees
`WIFSTOPPED`, `SIGCONT`s it, sees `WIFCONTINUED`; a background read
raises `SIGTTIN`.

## BB5 — minimal `/proc`

Only what `ps`/`top`/`free` read, and only if BB1 shows the chosen
applet set wants it. Synthetic `procfs` on the devfs dynamic-entry
pattern: `/proc/<pid>/{stat,cmdline,status}`, `/proc/self`,
`/proc/{meminfo,uptime,mounts,cpuinfo,stat}`. Read-only; writes
`-EROFS`. Field order and units are Linux's.

**Test:** parse `/proc/self/stat`, check pid/ppid against
`getpid`/`getppid`; `/proc/self/cmdline` matches argv.

## BB6 — interactive shell bring-up

- `/etc/inittab` gains a shell entry. With M1c-3's VTs in place the
  natural home is a VT: init runs the shell on `/dev/tty1`. The
  PTY-hosted `TERM` remains the alternative.
- init sets `PATH=/bin`, `HOME=/`, `TERM=`, `PS1` and `execve`s the
  shell (needs `spawnve` in `lib/`, or `fork` + `execve`).
- The shell becomes a session leader with a controlling terminal (BB4).
- `/etc/passwd` one-liner, optional `/etc/profile`.

**Test:** `neoos_test_inject_key` feeds a scripted keystroke sequence and
the serial log is scraped for the prompts and command output —
`[busyboxtest] interactive ALL PASSED`. Feel is checked by the user with
`make run`.

---

## Cross-cutting constraints (unchanged)

- **The shim translates, never emulates.** A shim row that would have to
  fake a primitive means the primitive belongs in the kernel.
- **Every observable struct and constant stays Linux-shaped** —
  `struct utsname`, `AT_*`, `/proc/<pid>/stat` field order and units.
- **Each sub-milestone ends `make test`-green and gauntlet-green**, with
  a userland or kernel selftest proving the feature from the far side of
  the syscall boundary.
- **Gauntlet reds are not automatically regressions.** A measured
  baseline at `5922fb2` is 3 hard failures / 45 runs at `CONC=2`, in
  three known signatures (`musltest` heap corruption, `mpitest` and
  `sigtest` wall-clock starvation). Compare rates over 45+ runs before
  calling anything a regression.
- **Work on `main`**, one commit per task, standard trailer.
- **At each close**, refresh `docs/abi-compatibility.md` and
  `docs/stdlib.md`, and tick this file.

## Still open

- **Applet set.** ~25–30: `ash ls cat echo cp mv rm mkdir rmdir pwd test
  true false env printenv head tail wc grep sleep kill mount umount
  clear`, plus `ps`/`top`/`free` only if BB5 lands. Anything needing
  netlink, real `/sys`, or absent ioctls is excluded, each exclusion
  noted. Settle this in BB1, against what actually builds.
- **Where the shell lives** — `/dev/tty1` or the PTY-hosted `TERM`.
  Decide at BB6, once BB4 has made a controlling terminal mean
  something. **It must not be `/dev/CONSOLE`**: that follows the active
  VT, so a shell holding it would start talking to a different terminal
  the moment anything switched VTs (recorded in `docs/stdlib.md`; it
  already made `ttytest` flaky when `vtswitchtest` ran beside it).
  Whether `/dev/CONSOLE` should instead be *bound* to VT 1, as Linux
  binds `/dev/console`, is a live question for BB6 — it would be the
  Linux-shaped answer, and it would make `/dev/CONSOLE` usable as a
  program's terminal again.
- **DL1–DL3** are deferred, not dropped. Nothing in this track needs
  them; the `fork` fix in BB0 is where file-backed vmas will hook in.

## BB3 result — **DONE** (2026-09-02)

The entry stack is now the full SysV shape: `argc`, `argv[]`, NULL,
`envp[]`, NULL, auxv. `execve` and a new `spawnve` both carry the
environment; `execv`/`spawnv` pass an empty one, as their names say.
init supplies the base set (`PATH=/BIN`, `HOME=/`, `TERM=linux`, `PS1`),
since nothing above PID 1 has one to hand down.

`tlstest` had asserted `environ[0] == 0` — an empty environment, which
was correct before this and is now wrong. It asserts the vector is
well-formed instead (present, every entry `NAME=...`, NULL-terminated);
*which* variables exist is init's business, not the entry contract's.

**A wrong conclusion, corrected in BB6.** BB3 originally recorded that a
command launched by `ash` saw no environment at all, and concluded the
loss was "inside ash's own launching of a child" because four tagged
probes showed both kernel paths carrying it correctly.

That was wrong, and the reasoning was the trap: the probes proved the
kernel's `spawnve` and `execve` paths were fine, and I read that as
proof the kernel was fine. What they did not cover was the path ash
actually uses — fork WITHOUT exec — and fork was zeroing the child's
thread pointer (see BB6). Every one of those children was dying or
running with broken thread-local storage.

With the fork bug fixed, the same probes read:

| probe | result |
|---|---|
| `ash` → `busybox sh -c 'echo $PATH'` | `PATH=/BIN` ✓ |
| `ash` → `export FOO=bar; busybox sh -c ...` | `FOO=bar HOME=/ PATH=/BIN` ✓ |

The lesson worth keeping: proving the two paths you thought of are
correct says nothing about the third.

## BB6 result — **DONE** (2026-09-02)

`userland/bbsh.c` runs BusyBox `sh -i` on a pty: the shell on the slave
in its own session, commands written into the master, output read back.
It answers variable expansion, arithmetic, `pwd`, a pipeline, and a
redirection round trip through a file. That is the milestone's goal —
an interactive `ash` on a terminal — reached over a pty rather than the
framebuffer, which `/bin/term.nex` already renders.

Two kernel bugs stood in the way, both invisible to every existing test:
fork left the child with no thread pointer, and a pty hung up when a
forked child closed its inherited master fd. Both are described in the
commit and in the code at the point of the fix.

## BB4 result — **DONE** (2026-09-02)

`sh: can't access tty; job control turned off` is gone, and `bbsh`
asserts its absence — the message is the only evidence, since ash runs
perfectly well without job control and would otherwise degrade silently.
A background job (`sleep 5 & jobs`) is exercised too.

Two things were missing, both found by reading what ash actually does
rather than by predicting:

- **`fcntl(F_DUPFD)` / `F_DUPFD_CLOEXEC`** returned `-EINVAL`. ash does
  `fcntl(fd, F_DUPFD_CLOEXEC, 10)` to move the terminal out of the way,
  and gives up on job control the moment it fails. This is the first
  caller of a new `fd_table_alloc_from`.
- **`TIOCSCTTY`**, so a session leader can claim a tty it inherited.
  The slave is opened by the parent that sets the pty up, so the pty
  records the parent's session; without a way to take it over, ash saw a
  foreground group that was not its own and signalled itself with
  SIGTTIN until it stopped. Removing the `TIOCSCTTY` call from bbsh
  reproduces exactly that: the shell answers nothing at all.

`SIGTTIN`/`SIGTTOU` are still not *generated* by background reads and
writes; nothing exercised so far needs that, and it is the next thing to
add if something does.

## BB5 result — **DONE** (2026-09-02)

`ps` was the only thing that ever asked for `/proc`, and it asked by
name — `ps: can't open '/proc': No such file or directory` — which is
why this was measured rather than assumed before being built.

`kernel/fs/procfs.c` is a synthetic read-only filesystem mounted at
`/proc` at boot, providing `/proc/<pid>/stat` and `/proc/<pid>/cmdline`
and nothing else. `busybox ps` now lists real processes by pid and name;
`bbspike` asserts it through `busybox ps | grep -q INIT.ELF`, because
`ps` exits 0 whether or not it found anything and only the pipeline's
status distinguishes a working `/proc` from an empty one.

A `comm` field was added to `struct process` for it — the basename of
the spawned or exec'd path, Linux's 16 bytes — set at spawn, reset at
exec, inherited across fork.

One thing had to be learned by panicking: the first version rendered
each file in `read_inode` to report a real `st_size`, and `read_inode`
runs with the VFS's locks held while the process-table walk takes a
lock ranked below them. The rank checker caught it on the first boot.
Reporting 0 is both what Linux does and what the lock order allows.
