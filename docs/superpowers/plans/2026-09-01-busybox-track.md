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
BB1.  Build-and-run spike: /bin/busybox to first -ENOSYS
BB2.  The measured syscall set (was BB1 + BB4)
BB3.  execve(path, argv, envp)
BB4.  Job control                                     (was BB2)
BB5.  Minimal /proc                                   (was BB3)
BB6.  Interactive shell bring-up
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

## BB0 — `fork` duplicates the address-space bookkeeping

**Blocker. Nothing else in the track is testable until this lands.**

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

- `/ETC/INITTAB` gains a shell entry. With M1c-3's VTs in place the
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
  something.
- **DL1–DL3** are deferred, not dropped. Nothing in this track needs
  them; the `fork` fix in BB0 is where file-backed vmas will hook in.
