# NeoOS Linux ABI Compatibility Report

**Generated:** 2026-08-30, at the close of Phase 10 (SMP & concurrency).
**Refreshed:** at the end of every milestone, per `CLAUDE.md`.

## 1. Scope

Internals are ours; the ABI is not. Kernel data structures, internal
calling conventions, lock ranks, NeoOS's own syscall numbers, scheduler
and memory internals may be reshaped freely.

But the long-term goal is to run **real Linux applications unpatched**.
So wherever a primitive is *observable from a user-mode program* it must
be Linux-shaped: struct layouts, flag and constant values, semantics,
and the ELF entry / auxv / TLS / signal-frame contract. The syscall
*numbers* stay NeoOS's own — a shim translates those. It is the shapes
and semantics behind the numbers that cannot diverge, because no shim
can retrofit a struct layout an application was compiled against.

This report is the honest inventory. Anything below marked **DIVERGES**
is a known gap, not a design claim.

## 2. Syscalls

42 syscalls, numbered 0–41 in NeoOS's own space (`kernel/syscall.c`).
Linux x86_64 numbers are unrelated and are expected to be mapped by the
musl shim when musl is integrated.

| NeoOS | Name | Linux analogue | Status |
|-------|------|----------------|--------|
| 0 | exit | `exit` | implemented |
| 1 | write | `write` | implemented |
| 2 | yield | `sched_yield` | implemented |
| 3 | getpid | `getpid` | implemented |
| 4 | spawn | *(none — NeoOS extension)* | implemented |
| 5 | wait | *(NeoOS wait-by-pid)* | implemented |
| 6 | read | `read` | implemented |
| 7 | open | `open` | implemented, flags diverge (§4) |
| 8 | close | `close` | implemented |
| 9 | mkdir | `mkdir` | implemented |
| 10 | unlink | `unlink` | implemented |
| 11 | lseek | `lseek` | implemented |
| 12 | fork | `fork` | implemented (COW) |
| 13 | exec | `execve` | implemented |
| 14/15 | mount/umount | `mount`/`umount2` | implemented, NeoOS-shaped |
| 16 | getdents | `getdents64` | implemented, **struct DIVERGES** (§3) |
| 17–20 | thread create/exit/join/self | `clone`/`exit`/`futex`/`gettid` | NeoOS-shaped, not `clone` |
| 21–28 | rt_sigaction … sigaltstack | same names | implemented |
| 29–31 | kill / tkill / tgkill | same names | implemented |
| 32 | wait4 | `wait4` | implemented |
| 33–36 | setpgid/getpgid/setsid/getsid | same names | implemented |
| 37–39 | mmap / munmap / mprotect | same names | implemented |
| 40–41 | cpu_count / getcpu | `sysconf`/`getcpu` | implemented (Phase 10) |

**Absent, and reached early by a real program:** `stat`, `fstat`,
`lstat`, `newfstatat`, `ioctl`, `fcntl`, `dup`/`dup2`, `pipe`,
`readv`/`writev`, `brk`, `clock_gettime`, `nanosleep`, `futex`,
`set_tid_address`, `set_robust_list`, `getuid`/`geteuid`/`getgid`,
`uname`, `poll`/`select`, `socket`. There is **no `clone`**: threads are
created through a NeoOS-shaped call, so a pthreads implementation cannot
sit directly on top yet.

## 3. Struct layouts crossing the boundary

| Struct | Verdict |
|--------|---------|
| `struct k_sigaction` | **Matches** Linux's kernel `sigaction`: handler, flags, restorer, mask, in that order. |
| `struct siginfo` | **Matches** in size — 128 bytes, as Linux. Only the fields NeoOS produces are named; the rest is explicit padding. |
| `struct dirent` | **DIVERGES completely.** |
| `struct stat` | **Absent.** No stat family exists, so there is nothing to compare. This is the single largest gap. |
| `struct timespec` | **Absent.** No `clock_gettime`/`nanosleep`. |
| `struct utsname` | **Absent.** No `uname`. |

### `struct dirent` — the divergence in detail

NeoOS (`lib/include/dirent.h`):

```c
struct dirent { char name[13]; uint8_t type; };
```

Linux `getdents64`:

```c
struct linux_dirent64 {
    ino64_t  d_ino; off64_t d_off; unsigned short d_reclen;
    unsigned char d_type; char d_name[];
};
```

Nothing matches: no inode number, no offset, no record length, and a
**13-byte fixed name** (FAT 8.3 plus NUL) where Linux has a variable
one. Any program that reads directories — `ls`, a shell glob, a build
tool — needs this replaced with the Linux layout. A shim cannot paper
over it, because the record length is what lets a caller walk the
buffer.

## 4. Constants and flags

| Family | Verdict |
|--------|---------|
| `errno` | **Matches** Linux x86_64: EPERM 1, ENOENT 2, ESRCH 3, EINTR 4, EBADF 9, ECHILD 10, EAGAIN 11, ENOMEM 12, EBUSY 16, EEXIST 17, ENODEV 19, ENOTDIR 20, EISDIR 21, EDEADLK 35, ENOSYS 38, ETIMEDOUT 110. |
| Signal numbers | **Matches**: SIGKILL 9, SIGSEGV 11, NSIG 65 (1–64 usable). |
| `PROT_*`, `MAP_*` | **Match** for the subset implemented (`PROT_READ` 1, `PROT_WRITE` 2, `MAP_PRIVATE` 0x02, `MAP_ANONYMOUS` 0x20). |
| `_SC_NPROCESSORS_*` | **Match** (83, 84). |
| `O_*` | **`O_CREAT` DIVERGES**: NeoOS 0x100, Linux 0x40. `O_RDONLY`/`O_WRONLY`/`O_RDWR` (0/1/2), `O_TRUNC` (0x200) and `O_APPEND` (0x400) match. |
| `AT_*`, `CLOCK_*` | **Absent.** |

**`O_CREAT` is a live bug**, not a design choice: a program compiled
against Linux headers passes 0x40, which NeoOS does not recognise as
create — so the open silently fails to create the file. It costs one
constant to fix and should be fixed before anything is ported.

## 5. Error-reporting convention — DIVERGES

NeoOS's library returns negative error codes **directly** from
`open`/`read`/`write`/`close`/`lseek`/`mkdir`/`unlink`
(`lib/include/errno.h`). There is no settable `errno` variable.

POSIX requires `-1` plus `errno`. Every ported program checks `errno`,
so this must change when musl lands: the kernel keeps returning negative
codes (Linux-shaped), and musl's syscall wrappers do the `-1`/`errno`
translation, which is exactly the translation-not-emulation split
`CLAUDE.md` describes. No kernel change is needed — only the shim.

## 6. Process startup contract

| Item | State |
|------|-------|
| ELF loading | static ELF64 only; no dynamic linker, no `PT_INTERP` |
| **auxv** | **Not supplied.** musl's `__libc_start_main` reads `AT_PHDR`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM`; without them it cannot start. |
| Initial stack | `argc`/`argv` placed by `crt0.asm`; envp and auxv absent |
| TLS | no `fs_base` setup, no `set_thread_area`; `%fs`-relative TLS is unavailable |
| Signal frame | NeoOS-shaped, delivered via a `restorer` trampoline; the layout is **not** Linux's `rt_sigframe` |
| Stack alignment | SysV 16-byte at entry — matches |

## 7. What a real ported application hits, in order

1. **auxv is missing** — musl's startup aborts before `main`. Nothing
   runs until this exists.
2. **No TLS / no `set_thread_area`** — any libc using `%fs` for errno or
   thread state faults immediately.
3. **No `stat` family** — the second or third call of almost any tool.
4. **`struct dirent` layout** — anything listing a directory.
5. **`O_CREAT` value** — silent failure to create files.
6. **No `errno` variable** — every error check reads the wrong thing
   (fixed by the musl shim, not the kernel).
7. **No `clone`/`futex`** — no pthreads.
8. **No `clock_gettime`/`nanosleep`** — no timing, no sleeping.
9. **13-character filenames** — a FAT limit that reaches userland
   through `dirent`; not a Linux-ABI issue as such, but a porting one.

## 8. Phase 10's own additions

`sysconf(_SC_NPROCESSORS_ONLN/CONF)` and `sched_getcpu()`, both
Linux-shaped, with divergences recorded in `docs/stdlib.md`: `CONF`
always equals `ONLN` (no hotplug), no NUMA node is reported, and there
is no `sched_setaffinity` to make the result stable.

## 9. Summary

The **error space is Linux-compatible** (errno values, signal numbers,
`PROT_*`/`MAP_*`) and the **signal disposition structs match**. The
**process startup contract and the file-metadata surface are the real
work**: auxv, TLS, the `stat` family, and `struct dirent` stand between
NeoOS and running an unmodified binary. Two are cheap and worth doing
immediately — the `O_CREAT` value, and auxv.
