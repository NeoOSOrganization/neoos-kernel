# Desktop M0: kernel prerequisites (K1–K3) — implementation plan

**Spec:** `docs/superpowers/specs/2026-09-23-desktop-00-roadmap.md` ("Kernel
prerequisites"), with detail in spec 01 (K1), 08 (K2), 06 (K3) and 02
(Persian file names).

**Goal:** the Linux-shaped kernel primitives the desktop needs before any
desktop code: POSIX record locks, `O_EXCL`, `pwrite`, `rename`, UTF-8 long
file names on FAT, richer procfs, and init power control by signal.

## Global constraints

- Work on `main`, commit per task (project convention).
- Linux-shaped where observable: x86_64 `struct flock` layout; `F_GETLK=5`,
  `F_SETLK=6`, `F_SETLKW=7`; `F_RDLCK=0`, `F_WRLCK=1`, `F_UNLCK=2`; Linux
  syscall numbers `pwrite64=18`, `rename=82`, `renameat=264`,
  `renameat2=316`; errno values `EEXIST=17`, `EXDEV=18`, `EAGAIN=11`,
  `EDEADLK=35`, `ENOTEMPTY=39`.
- New NeoOS syscall numbers continue from 138 (`SYS_MAX` today).
- Every shim change: rebuild neoos-musl, copy `libc.a` into the hosted
  sysroot, regenerate neoos-hosted-gcc's `008-neoos_syscall.diff`.
- Verification per task: `make iso` warning-free for touched files;
  `tools/gauntlet.sh 15` = 15/15 with **zero retries** (nothing else
  building); `make hrtest` still passes; the task's own test target passes.
- No timing hacks; tests wait on real conditions.
- Every divergence recorded in `docs/stdlib.md`; `abi-compatibility.md`
  refreshed at the end (Task 7).

## Tasks

### Task 1: `O_EXCL` and `pwrite`
- `sys_open`: `O_CREAT|O_EXCL` on an existing path → `-EEXIST` (checked
  under `fs_lock`, so two racing creators cannot both succeed).
- `sys_pwrite` (NeoOS 138): positional write that does not move the file
  position, mirroring `sys_pread`; shim `LX_PWRITE64 18`.
- Test `userland/m0test.c` (musl, hosted toolchain, `make m0test`,
  booted alone like hrtest): `excl` and `pwrite` checks.

### Task 2: `rename`
- VFS: `int (*rename)(struct vnode *olddir, const char *oldname,
  struct vnode *newdir, const char *newname)` in `struct vfs_ops`;
  every driver gets it (stubs return `-EPERM`/`-EROFS` where truthful).
- fatfs: create the new entry (`fat_create_named` with the old entry's
  attr/cluster/size), erase the old run (clusters kept); an existing
  target file is replaced (its clusters freed); a directory moved to a
  new parent gets its `..` entry updated; renaming a directory onto an
  existing one → `-EEXIST`/`-ENOTEMPTY` per Linux; a directory into its
  own subtree → `-EINVAL`.
- Cached vnodes: fatfs inode ids are the dirent location, so a rename
  re-keys any live vnode (`vfs_vnode_rekey`) and updates its
  `fatfs_inode` location — an open fd keeps working after the rename.
- ramfs: in-memory rename.
- Cross-mount → `-EXDEV`.
- `sys_rename` (139), `sys_renameat2` (140; `flags` 0 only, else
  `-EINVAL`; `AT_FDCWD` or a directory fd); shim maps `rename`,
  `renameat`, `renameat2`.
- m0test: same-dir, cross-dir, replace-existing, open-fd-survives,
  EXDEV, EINVAL-into-subtree.

### Task 3: UTF-8 long file names on FAT
- LFN slots store UTF-16; today each UTF-8 *byte* is stored as one
  UTF-16 unit. Encode UTF-8 → UTF-16 (with surrogate pairs) on create
  and decode on read, so Persian names are correct on disk for any OS.
- Names written by older kernels (byte-per-unit) still read back:
  units ≤ 0xFF that do not form valid UTF-8 when re-encoded are passed
  through as they always were (compatibility, recorded).
- m0test: create "فایل‌ها.txt" (with ZWNJ), readdir returns it
  byte-identical; the host-side check extracts the image with `mtools`
  and confirms the UTF-16 on disk (the FAT-bug lesson: check the disk).

### Task 4: POSIX record locks (`fcntl` `F_GETLK`/`F_SETLK`/`F_SETLKW`)
- Per-vnode lock list (owner = process, type, `[start, end]`), under a
  new lock rank; `F_SETLKW` sleeps on a per-vnode waitq, interruptible;
  `F_SETLK` conflict → `-EAGAIN`; `F_GETLK` reports the first conflict
  (`l_pid` of the holder) or `F_UNLCK`.
- Merge/split of ranges on lock/unlock (POSIX semantics).
- Closing ANY fd of a process for an inode releases ALL that process's
  locks on it; process exit releases everything.
- Deadlock detection between `F_SETLKW` waiters (`-EDEADLK`): simple
  wait-for walk.
- m0test (fork-based): exclusive vs shared, conflict pid, blocking wait
  woken by unlock, close-releases-all, exit-releases, split on partial
  unlock, EDEADLK.

### Task 5: procfs (K2)
- `/proc/<pid>/stat`: real state letter, `utime` (ticks, 100 Hz) from
  the scheduler's runtime, `num_threads`, `starttime`, `vsize`, `rss`.
- `/proc/<pid>/status`, `/proc/meminfo`, `/proc/uptime`,
  `/proc/self` → the caller's pid dir, per-CPU `cpuN` lines in
  `/proc/stat`.
- m0test: a spinning child's utime grows; `Threads:` matches;
  meminfo/uptime parse.

### Task 6: init power control (K3)
- init: `SIGUSR2` → power off, `SIGTERM` → reboot (BusyBox convention):
  stop respawn entries, `SIGTERM` every process, wait for them to exit
  (bounded by a real deadline, then `SIGKILL`), `sync`, `reboot()`.
- Verify `reboot(LINUX_REBOOT_CMD_RESTART)` resets the machine; add the
  reset (keyboard-controller pulse / ACPI reset register) if not.
- `make powertest`: a program sends `SIGUSR2` to PID 1; the boot log
  shows the shutdown sequence and QEMU exits; a second scenario sends
  `SIGTERM` and the harness sees a second boot banner.

### Task 7: documentation
- `docs/stdlib.md`: `rename*`, `pwrite`, `O_EXCL`, record locks (with
  POSIX close semantics spelled out), UTF-8 LFN, procfs files, init
  signals.
- `docs/abi-compatibility.md`: refresh section.
