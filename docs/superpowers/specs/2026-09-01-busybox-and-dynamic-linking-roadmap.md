# BusyBox + dynamic linking — roadmap

**Status:** planning index. Design-only until M1b (framebuffer terminal)
ships. Each sub-milestone below gets its own full design spec
(`docs/superpowers/specs/`) and implementation plan
(`docs/superpowers/plans/`) when it is reached — this file records the
decomposition, the ordering, and the decisions that are already
settled so a later spec does not relitigate them.

**Goal:** an interactive BusyBox `ash` on the framebuffer terminal —
`cd`/`ls`/`cat`/pipes/redirection/job control all working — reached by
first making NeoOS able to run a dynamically-linked ELF. Feeds the
CLAUDE.md long-term goal: run real Linux applications without patching
them.

**Origin:** brainstormed 2026-09-01 (session
`session_01CNR4gEkyMq6qhFWfxt3KXE`), immediately after M2 (init as PID
1). BusyBox itself is built **static** — dynamic linking is a
separate, higher-leverage milestone the user chose to sequence ahead
of it, not a dependency BusyBox waits on.

---

## Track order

```
M2 (done)
  └─ M1b.  Framebuffer terminal            ← NEXT, already in the pipeline
           xterm-ish VT + scrollback + NokiaPure-as-PSF; kernel out of
           the printing business.

  ── Dynamic linking ──────────────────────────────────────────────
  ├─ DL1.  File-backed mmap                 ← prerequisite for everything DL
  ├─ DL2.  PT_INTERP + full auxv
  └─ DL3.  Dynamic TLS + dlopen

  ── BusyBox ──────────────────────────────────────────────────────
  ├─ BB1.  execve + shell-critical ABI
  ├─ BB2.  Job control
  ├─ BB3.  Minimal /proc
  ├─ BB4.  Tier-2 filesystem syscalls
  ├─ BB5.  BusyBox build integration
  └─ BB6.  Interactive shell bring-up
```

BB1–BB4 are independent of each other and can be reordered or
parallelised; BB5 needs BB1–BB4 landed; BB6 needs BB5 and M1b.

---

## Dynamic linking

### DL1 — file-backed `mmap`

Today NeoOS `mmap` is anonymous-only (`docs/abi-compatibility.md` §3).
`ld.so` and every `.so` need `mmap(MAP_PRIVATE, fd, offset)`.

- `mmap` with a real `fd`: `MAP_PRIVATE` (file pages, copy-on-write on
  first write), `MAP_FIXED` (ld.so places segments at chosen
  addresses), `MAP_SHARED` on a regular file may stay `-ENOSYS` for
  now (nothing in the musl dynamic path needs it — `/dev/fb0` already
  has its own `MAP_SHARED` path from M1a).
- Demand-paged through the M1a `vma_fault` path: a new `VMA_FILE` flag
  alongside `VMA_PHYS`, the fault handler reads the backing page from
  the vnode. `PAGE_COW` (from the NX/W^X milestone) marks the private
  pages.
- `MAP_DENYWRITE` / `MAP_EXECUTABLE` / `MAP_NORESERVE`: accept and
  ignore.
- The mapped fd is ref-held by the VMA for the mapping's lifetime
  (fd objects are already refcounted — M1a `file.c`).
- **Test:** `mmap(MAP_PRIVATE)` a file, read through the mapping,
  write through it and confirm the file on disk is unchanged and a
  second mapping still sees the original bytes.

### DL2 — `PT_INTERP` + full auxv

- ELF loader detects a `PT_INTERP` segment, loads the named
  interpreter (`/lib/ld-musl-x86_64.so.1`) at a base address, and sets
  the process entry point to the interpreter's `e_entry` (plus base).
  The main executable is mapped but not jumped to; ld.so relocates it
  and jumps itself.
- `build_initial_stack` hands ld.so a **complete** auxv:
  `AT_BASE` (interpreter load base), `AT_PHDR` / `AT_PHENT` /
  `AT_PHNUM` (of the **main** executable, not the interpreter),
  `AT_ENTRY` (main exe), `AT_EXECFN`, `AT_SECURE` (0), `AT_PAGESZ`,
  `AT_RANDOM`, `AT_CLKTCK`, `AT_HWCAP` (conservative). Today only
  `AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/RANDOM` are supplied
  (`docs/abi-compatibility.md` §5).
- Build musl a second time as shared (`libc.so`) — or reuse the
  configure output that already produces it — and install
  `/lib/libc.so` and `/lib/ld-musl-x86_64.so.1` onto the disk image.
  VFAT long names are in (lfntest), so the `.so.1` name is fine.
- `user.ld` / build flags for a dynamic userland target (PIE or a
  fixed-base dynamic exe at `0x200000000000`, matching the static
  large-model convention).
- **Test:** a one-line dynamically-linked "hello" prints and exits 0;
  the loader messages (if any) do not appear on a clean run.

### DL3 — dynamic TLS + `dlopen`

- General-dynamic / local-dynamic TLS: `__tls_get_addr` and the
  dynamic thread vector (DTV), so a `.so` with `__thread` data works.
  Initial-exec TLS already works (Phase: TLS + auxv).
- `dlopen` / `dlsym` / `dlclose`: musl does the linking; verify the
  kernel needs nothing past DL1's file-backed `mmap`. If the DTV work
  proves large it may split into its own sub-milestone, but the
  brainstorm chose to attempt it in one pass.
- **Test:** `dlopen` a small `.so`, `dlsym` a function, call it, get
  the right answer; `dlclose` and confirm a later `dlsym` fails.

---

## BusyBox

### BB1 — `execve` + shell-critical ABI

- **`execve(path, argv, envp)`** — extend `exec` (syscall 13) in place
  to take an argv **and an envp**. NeoOS's numbers are its own, so
  reshaping 13 is allowed; `spawn` (4) and `spawnv` (51) stay for
  `lib/`. `build_initial_stack` learns to lay down a real environment
  (today `envp[0] = NULL`). Sits on top of DL2's auxv rework — no
  conflict, just ordering. Shim maps `LX_EXECVE` → `NEO_EXEC` with the
  usual path `char*` → `ptr,len` reshape and the argv/envp arrays
  forwarded.
- **`O_CREAT` `0x100` → `0x40`** — the long-outstanding constant
  (`docs/porting-coreutils.md` §5.4). Fixed in the kernel and in
  `lib/include/fcntl.h`; every in-tree userland user audited in the
  same commit.
- **`dup` / `dup2` / `dup3` + `F_DUPFD`** — fd-table dup over the
  already-refcounted fd objects. This is what `>`, `<`, `2>&1` and
  pipelines compile to. `dup2(x, x)` is a no-op returning `x`;
  `dup3(x, x, …)` is `-EINVAL` (Linux semantics).
- **`getppid`** — returns `parent_pid` (already tracked; M2 reparents
  it to 1 on orphan).
- **`getuid` / `geteuid` / `getgid` / `getegid`** — return 0.
  Single-user system; 0 is the honest answer.
- **`umask`** — per-process value, stored and reported; honoured
  nowhere because FAT has no mode bits (recorded divergence).
- **`uname`** + `struct utsname` — the x86_64 Linux 65-byte-field
  layout; sysname `"NeoOS"`, machine `"x86_64"`, release a version
  string.
- **Test:** `execve` replaces the image and the new image sees the
  passed argv+envp; `dup2`-based redirection round-trips through a
  pipe; `getppid` matches.

### BB2 — job control

- Real `tcsetpgrp` / `tcgetpgrp` enforcement on `struct tty`
  (`fg_pgid` exists from M1a). A write from a process not in the
  foreground pgrp, or a read from a background pgrp, is gated.
- Generate `SIGTTIN` (bg read) and `SIGTTOU` (bg write when
  `TOSTOP` set) on the controlling tty — the M2 stdlib note says
  these "are never generated"; this milestone changes that and updates
  the note.
- Controlling-terminal model: `TIOCSCTTY`, session leader acquires the
  tty as its controlling terminal, `/dev/tty` resolves to it, tty
  tracks its session id (`sid` field exists from M1a).
- Orphaned-process-group handling: when a pgrp becomes orphaned and
  has stopped members, deliver `SIGHUP` then `SIGCONT` — the gap M2's
  `docs/stdlib.md` explicitly flagged.
- `SIGCHLD`-on-stop and `WUNTRACED` / `WCONTINUED` reach the shell
  cleanly (wait4 already carries the flags — verify the stop path
  wakes `child_waiters` and encodes `WIFSTOPPED`).
- **Test:** `jobctltest` — a child is stopped by `SIGTSTP`, the parent
  sees `WIFSTOPPED`, continues it with `SIGCONT`, sees `WIFCONTINUED`,
  and a background read raises `SIGTTIN`.

### BB3 — minimal `/proc`

- A synthetic `procfs`, mounted at `/proc`. The M1a devfs
  dynamic-entry pattern (`devfs_register` / `dyn[]` table) is the
  precedent for the per-pid directories.
- Files: `/proc/<pid>/stat` (the fields `ps` and `top` read — pid,
  comm, state, ppid, pgrp, session, utime/stime as 0, starttime,
  rss), `/proc/<pid>/cmdline` (NUL-separated argv), `/proc/<pid>/status`
  (human-readable superset), `/proc/<pid>/fd/` (symlink-ish entries or
  a plain listing). `/proc/self` → the caller's pid.
  `/proc/meminfo`, `/proc/uptime`, `/proc/mounts`, `/proc/cpuinfo`
  (from the SMP topology), `/proc/stat` (boot time, ctxt — best
  effort).
- Read-only. Writes return `-EROFS`.
- **Test:** open `/proc/self/stat`, parse it, check pid and ppid match
  `getpid` / `getppid`; `/proc/self/cmdline` matches argv.

### BB4 — Tier-2 filesystem syscalls

`docs/porting-coreutils.md` §6, plus what BusyBox specifically calls:

- `rename`, `rmdir`, `ftruncate` — real, on the FAT backend.
- `access` / `faccessat` — existence + the always-true rwx for a
  single-user no-perms system.
- `readlink` / `readlinkat` — `-EINVAL` (FAT has no symlinks; this is
  the correct errno).
- `symlink` / `link` — `-EPERM` (FAT genuinely cannot).
- `chmod` / `fchmod` / `chown` / `fchown` — **accept and return 0**.
  FAT stores nothing; `-EPERM` would make `cp -p`, `install`, and
  `tar -x` fail spuriously. Recorded as a divergence.
- `utimensat` / `futimens` — accept and return 0 (no timestamps to
  set).
- `getrandom` — real, from the kernel RNG (seeded at boot; `AT_RANDOM`
  already uses it).
- `clock_nanosleep` — `CLOCK_MONOTONIC` / `CLOCK_REALTIME`, absolute
  and relative; on top of the existing `nanosleep`.
- `sync` / `fsync` / `fdatasync` — flush the block cache; `sync`
  returns void/0.
- `gethostname` / `sethostname` — a stored hostname, default
  `"neoos"`.
- `openat` — at minimum `AT_FDCWD` (real dirfds wait for an openat
  family, as `newfstatat` already documents).
- Shim rows for every one of these.
- **Test:** extend an existing fs test, or a new `tier2test`, covering
  rename/rmdir/ftruncate/access/getrandom/clock_nanosleep and the
  accept-ignore stubs.

### BB5 — BusyBox build integration

- `third_party/busybox` as a git submodule (mirrors
  `third_party/musl`). `third_party/busybox-config/` holds the NeoOS
  `.config`, an `apply.sh` (mirrors the musl shim's), and any port
  patches.
- Static build against NeoOS's musl (`third_party/musl/lib/libc.a`) +
  the shim, `-mcmodel=large`, linked at the userland convention base.
- **No symlinks on FAT16, and 30× a ~1 MB binary will not fit a 32 MB
  disk** → build with `CONFIG_FEATURE_SH_STANDALONE` +
  `CONFIG_FEATURE_PREFER_APPLETS`: one `/bin/busybox`, and the shell
  dispatches applets internally or by re-exec. This is the initramfs
  pattern.
- Applet set (~25–30): `ash`, `ls cat echo cp mv rm mkdir rmdir pwd
  test true false env printenv head tail wc grep sleep kill mount
  umount ps top free clear` plus whatever comes free. Applets needing
  netlink, real `/sys`, or ioctls NeoOS lacks are left out; each
  exclusion noted.
- `make test`: a new `[busyboxtest]` target runs `busybox sh
  /ETC/TEST.SH` (a script exercising cd, ls, cat, a pipe, a
  redirection, `test`, `$?`) and greps for `[busyboxtest] script
  ALL PASSED`.
- **Test:** the headless script above.

### BB6 — interactive shell bring-up

- `/ETC/INITTAB` gains a shell entry. If M1b's terminal is a
  PTY-hosted userland VT, init launches the terminal and the terminal
  starts the shell on its PTY slave; otherwise init runs
  `respawn /bin/busybox sh` on `/dev/CONSOLE` as the fallback.
- init sets the initial environment — `PATH=/bin`, `HOME=/`,
  `TERM=…`, `PS1` — and `execve`s the shell. Needs a `spawnve` in
  `lib/` (spawn + env), or init does `fork` + `execve` directly.
- The shell becomes a session leader with a controlling terminal
  (BB2's `TIOCSCTTY` path).
- `/etc/passwd` one-liner (`root:x:0:0:root:/:/bin/sh`), optional
  `/etc/profile`.
- Headless interactive test: `neoos_test_inject_key` feeds a scripted
  keystroke sequence to the console line discipline and the serial log
  is scraped for the expected prompts and command output —
  `[busyboxtest] interactive ALL PASSED`. Feel is verified by the user
  with `make run`.
- **Test:** the key-injection session above.

---

## Cross-cutting constraints

- **musl stays reached through the translation-only shim**
  (`third_party/shim/`). Every new syscall gets either a shim row or,
  for a NeoOS-native call, a `lib/` wrapper — and a `docs/stdlib.md`
  entry or an explicit divergence note. Per CLAUDE.md, the shim never
  emulates: if a shim row would have to fake a primitive, the
  primitive goes in the kernel instead.
- **Every observable struct and constant stays Linux-shaped**:
  `struct utsname`, `mmap` flags, `AT_*`, `dlfcn` return values,
  `/proc/<pid>/stat` field order and units.
- **Each sub-milestone ends gauntlet-green** (`pgauntlet.sh` →
  `PGAUNTLET PASSED: N/N`) and `make test`-green, with a userland or
  kernel selftest proving the feature from the far side of the syscall
  boundary. No host unit tests — this is bare metal.
- **Work on `main`**, one logical commit per task, standard trailer.
- **At each sub-milestone close**, refresh `docs/abi-compatibility.md`
  and `docs/stdlib.md`, and tick this file's track table.
- **`third_party/musl` and `third_party/busybox` submodules**: the
  shim/config lives outside the submodule and is applied by a script,
  so the submodule tree itself stays clean.

## Decisions already settled (do not relitigate in sub-specs)

| Question | Decision |
|---|---|
| DL before or after BusyBox | Before. Higher leverage on "run real Linux apps". |
| BusyBox static or dynamic | **Static** — the canonical form; DL is proven by separate dynamic test programs. |
| DL scope, first pass | Full: DL1 + DL2 + DL3 (dlopen included). |
| Order vs M1b | **M1b → DL → BB.** |
| `execve` — new number or extend | Extend `exec` (13) in place with argv + envp. |
| `O_CREAT` | Fix to `0x40` in BB1, audit all userland in the same commit. |
| `chmod`/`chown`/`utimensat` | Accept and return 0 (FAT stores nothing); recorded divergence. |
| `symlink`/`link` | `-EPERM`. `readlink` `-EINVAL`. |
| BusyBox invocation without symlinks | `CONFIG_FEATURE_SH_STANDALONE` + `PREFER_APPLETS`; one `/bin/busybox`. |
| Applet set size | Shell + ~25–30 core coreutils; `ps`/`top`/`free` enabled by BB3's `/proc`. |
| BusyBox source | Git submodule + checked-in config/patches, `apply.sh` like the musl shim. |
| Job control | Implemented properly (BB2), not stubbed. |
| Shell's home | M1b's PTY-hosted terminal; `/dev/CONSOLE` is the fallback. |
