# Porting GNU coreutils to NeoOS

**Generated:** 2026-08-31, after the close of Phase 12 (IPC, TLS, networking).
**Companion to:** `docs/abi-compatibility.md`, which inventories the ABI as
a whole. This file answers a narrower question: what stands between
NeoOS and an unmodified `cat`, `ls`, `cp`, `rm`.

Every claim below about what musl calls was checked against the
vendored tree (`third_party/musl`, 1.2.5) and is cited by file and
line. Where an earlier assumption turned out wrong, the correction is
stated rather than quietly dropped — §3 has three of them.

## 1. Where the port actually stands

**The shim exists and a musl binary runs.** `third_party/shim/` holds
it, `third_party/shim/apply.sh` installs it into the submodule, the
Makefile builds musl and links `userland/musl/hello.c` against it, and
`[musltest] ALL PASSED` is a required marker of `make test`.

That program uses printf, malloc, stat, open/read, opendir/readdir,
fopen/fgets, clock_gettime, isatty and getpid — all through musl, none
of it NeoOS-aware.

Tier 0 and Tier 1 below are complete, and so is the long-filename
obstacle that used to head §8 — VFAT long names now read and write.
What remains is Tier 2 and the `-mcmodel=large` build requirement
(§8).

## 2. What a coreutils binary actually calls

A static musl binary reaches `main` through
`_start` -> `__libc_start_main` -> `__init_libc` (auxv) -> `__init_tls`
-> `__init_tp`. NeoOS already supplies the two pieces that used to be
the blockers: a real SysV entry stack with auxv, and
`arch_prctl(ARCH_SET_FS)` for the thread pointer.

What it then hits, in call order:

| musl call site | syscall | NeoOS |
|---|---|---|
| `src/env/__init_tls.c:22` | `set_tid_address` | present (pointer recorded, not acted on) |
| `src/stdio/__stdio_write.c:15` | `writev` | present, `IOV_MAX` 16 |
| `src/stdio/__stdout_write.c:8` | `ioctl(TIOCGWINSZ)` | present — `/dev/CONSOLE` is a real TTY |
| `src/exit/_Exit.c:6` | `exit_group` | present |
| `src/stat/fstatat.c` | `stat` / `lstat` / `fstat` / `newfstatat` | present |
| `src/dirent/readdir.c:15` | `getdents64` | present, record matches |
| `src/unistd/getcwd.c:17`, `chdir.c:6` | `getcwd`, `chdir` | present |
| `src/fcntl/open.c:18` | `fcntl(F_SETFD, FD_CLOEXEC)` | present, accepted as a no-op |
| `src/malloc/mallocng/malloc.c:63` | `brk` | absent, and **that is fine** (§3) |

Everything in this table is in place. The row that blocked `main` was
`__set_thread_area` (§3), not any of these.

## 3. Corrections worth recording

These contradict reasonable first guesses, and each one changes what
gets built.

**`statx` is not needed on x86_64.** `src/stat/fstatat.c` calls
`fstatat_statx` first, but only inside
`if (sizeof((struct kstat){0}.st_atime_sec) < sizeof(time_t))`. On
x86_64 `st_atime_sec` is `long` and `time_t` is 64-bit, so the
condition is false and the statx path is never taken. That branch
exists for 32-bit time64 architectures. Implementing `statx` would be
wasted work.

**musl usually calls plain `stat`/`lstat`, not `newfstatat`.** Inside
`fstatat_kstat`, when the path is absolute or `fd == AT_FDCWD`, musl
dispatches to `SYS_stat`, `SYS_lstat` or `SYS_fstat` directly.
`newfstatat` is reached only for a path relative to a real directory
fd. So the cheapest useful subset is `stat` + `lstat` + `fstat`, and
`newfstatat` can follow with the rest of the *at* family.

**`syscall_arch.h` is not the only place musl issues syscalls.** Six
hand-written x86-64 assembly files issue `syscall` themselves:
`syscall_cp.s` (every cancellable call — read, write, open, close),
`__set_thread_area.s`, `__unmapself.s`, `clone.s`, `restore.s` and
`vfork.s`. A shim that only replaces the header is silently incomplete,
and the failure is not clean: Linux's number lands on whatever NeoOS
call shares it. Linux `clone` is 56; NeoOS's 56 is `lstat`.

**`brk` is optional.** mallocng uses it only to grow its metadata area
(`src/malloc/mallocng/malloc.c:58-72`), and handles failure explicitly:
`ctx.brk = -1` and it falls back to `mmap`. Returning `-ENOSYS` is a
correct and sufficient answer. Anonymous `mmap` already works, so
NeoOS needs no heap syscall at all.

## 4. Tier 0 — DONE

| Syscall | State |
|---|---|
| `set_tid_address` | implemented; pointer recorded, not acted on (see `docs/stdlib.md`) |
| `exit_group` | implemented |
| `writev` / `readv` | implemented, `IOV_MAX` 16 |
| `ioctl` | implemented; `/dev/CONSOLE` answers the `TC*`/`TIOC*` set, other fds get `-ENOTTY` |
| `clock_gettime` / `nanosleep` | implemented; **resolution 10ms**; `CLOCK_REALTIME` anchored to the CMOS RTC, others count from boot |

The one that actually blocked `main` was none of these: it was
`__set_thread_area`, hand-written assembly that issues `arch_prctl`
directly and so never reached the shim. See §3.

Note musl 1.2.5's stdio probes with `TIOCGWINSZ`, not `TCGETS` — it
dropped the `TCGETS` probe — and `isatty` (`src/unistd/isatty.c:9`)
uses `TIOCGWINSZ` too. The console TTY answers both, along with the
rest of the `TC*`/`TIOC*` set a shell's job control needs.

## 5. Tier 1 — the file-metadata surface

This is the real wall, and it is where `ls`, `cat`, `cp` and `rm` live.

1. ~~**`stat` / `lstat` / `fstat`, with Linux's `struct stat`.**~~
   **DONE.** All four (plus `newfstatat` for `AT_FDCWD`) exist, with
   the exact 144-byte layout asserted at compile time and from
   userland. Field *values* are largely synthesized — see
   `docs/stdlib.md`. The timestamps being 0 is the one that will bite:
   anything comparing file times sees every file as equally old.
2. ~~**`struct dirent` in Linux's `getdents64` layout.**~~ **DONE.**
   Records are Linux's, `DT_*` are Linux's values, and `getdents`
   counts bytes. The filesystem drivers still fill a fixed-size
   kernel-internal struct; the ABI record is formatted in one place
   (`kernel/fs/file.c`), so the backends never see the wire format.
3. ~~**`chdir` + `getcwd`.**~~ **DONE.** Every process now carries a
   cwd from `proc_alloc` onward, `fork` and `spawn` inherit it, and
   every path-taking syscall resolves through it. `..` is resolved
   textually rather than by walking; see `docs/stdlib.md` for why, and
   for the *at*-family gap that remains.
4. **`O_CREAT` = `0x40`.** Still 0x100. Still one constant. Called out
   at the close of Phase 10 and again at Phase 12.
5. **`F_DUPFD`, and `dup`/`dup2`.** `F_GETFD`/`F_SETFD` already exist
   as accepted no-ops, which is enough for the `fcntl(F_SETFD,
   FD_CLOEXEC)` that `open()` itself issues whenever `O_CLOEXEC` is
   passed. `F_DUPFD` is refused with `-EINVAL`, deliberately: a silent
   success would hand back fd -1 as if it were open.
6. **`readlink`.** Returning `-EINVAL` on FAT is correct and enough.

### `struct stat` against a FAT backend

`struct vnode` (`kernel/fs/vfs.h`) carries only
`{ type, size (uint32_t), inode_id }`. There is no mode, uid, gid,
nlink, or timestamp anywhere, and FAT has no owners or permissions to
read them from. `stat` must therefore **synthesize** most of the
struct. That is acceptable, but per `CLAUDE.md` it is a deliberate
divergence and must be recorded in `docs/stdlib.md` with its reasoning
— including that `st_size` is currently bounded by a 32-bit vnode
field.

## 6. Tier 2 — makes the tools honest rather than merely running

`getuid`/`geteuid`/`getgid`/`getegid` (returning 0 is fine and is what
a single-user system should say), `uname`, `umask`, `chmod`, `rename`,
`rmdir`, `ftruncate`, `faccessat`, `utimensat`, `nanosleep`, and
`execve` taking argv/envp — today's `exec` (13) takes no argv, and
`spawnv` (51) is a different call with different semantics.

## 7. What is NOT needed, despite ranking high elsewhere

`clone`, `poll`/`select`, `epoll`, TCP, `AF_UNIX`, `setsockopt`.

`docs/abi-compatibility.md` ranks `clone` fifth on its list of what a
ported application hits. For **coreutils** it is close to irrelevant:
these tools are single-threaded and do not multiplex. `clone` matters
for musl's pthreads, not for `cat`. Worth keeping the two lists
distinct rather than working the general one top-down.

## 8. One obstacle that is not a syscall

~~**13-character filenames.**~~ **DONE.** `VFS_NAME_MAX` is now 256 and
the FAT driver reads and writes VFAT long names (up to 255 characters),
checked by `lfntest` against a name written externally with `mcopy`.
This was the one item here a user noticed immediately.

**`-mcmodel=large` at `0x200000000000`.** `userland/user.ld` avoids the
low 4GiB because that is `PML4[0]`, the kernel's identity map. Every
object — musl's and coreutils' — must be built in the same code model.
coreutils' build system will not do this on its own, and the failure
surfaces as relocation errors that read like a broken toolchain.

## 9. Shortest path to a real `cat`

1. ~~The shim, plus one musl-linked userland target.~~ **Done.**
2. ~~Tier 0 (§4).~~ **Done** — a musl binary starts and prints.
3. ~~`stat`/`lstat`/`fstat` with the exact `kstat` layout.~~ **Done.**
4. `fcntl` `F_SETFD` is **done**; `O_CREAT` = 0x40 is the **one
   constant still outstanding**.

Steps 1–3 and `getdents64`, `chdir`/`getcwd` and VFAT long names are
all in place, so unmodified `cat`, `echo`, `wc` and `ls` are down to
the `O_CREAT` value and Tier 2 (§6). The `-mcmodel=large` build
requirement (§8) is the remaining non-syscall obstacle.
