# NeoOS Linux ABI Compatibility Report

**Generated:** 2026-08-31, close of Phase 14 (the input subsystem: keyboard
decoder, evdev device, Linux ABI ioctl surface, test-hook syscall).
**Previous:** 2026-08-31, close of Phase 13 (the coreutils ABI surface:
musl, the `stat` family, the working directory, VFAT long names, TTY,
RTC, Tier-0 syscalls).
**Refreshed:** at the end of every milestone, per `CLAUDE.md`.
Most recent: M1a (console plumbing — `/dev/fb0`, `poll`/`select`, PTY)
and M2 (init as PID 1, orphan reparenting, `reboot(2)`), 2026-09-01.

**Phase 13.5 / 13.6 (SMP hardening — no ABI impact).** The SMP race
fixes and the object-lifetime / lock-detangle milestone changed no
struct that crosses to userland and no syscall semantics.
`thread_join`, `kill`, `tgkill`/`tkill` and `wait4` behave exactly as
before — they are now *correct* under concurrent access rather than
returning a spurious `-ESRCH` or racing a free. `proc_list`, the global
`proc_lock`, and the kernel's dead RCU are gone; all internal.

**Phase 14 (input subsystem).** Added keyboard support, the evdev
character device (`/dev/input/event0`), and Linux ABI-compatible ioctl
surface (`EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGBIT`,
`EVIOCGKEY`, `EVIOCGRAB`). The input subsystem has exclusive grab
semantics: when grabbed, keyboard input does not reach the TTY. New
userland test syscall `SYS_TEST_HOOK` (66) with subcommands for
deterministic event injection (`TESTHOOK_INJECT_KEY`) and kernel
statistics (`TESTHOOK_MIG_COUNT`); compiled only with `-DNEOOS_TEST_HOOKS`,
returns `-ENOSYS` in production. See `docs/stdlib.md` §8b for evdev
details and divergences.

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

## 2. What changed since Phase 10

Seven of the nine items on the previous report's "what a real ported
application hits, in order" list are now closed:

| Was #1 | **auxv is missing** | **CLOSED.** A real SysV entry stack with argc/argv/envp and AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/RANDOM. |
| Was #2 | **No TLS, no `arch_prctl`** | **CLOSED.** `arch_prctl(ARCH_SET_FS)`, per-thread FS base restored on every context switch, `__thread` works across migration. |
| Was #3 | **No `stat` family** | **CLOSED.** `stat`/`lstat`/`fstat`/`newfstatat`, Linux's 144-byte `struct stat`. Field values still synthesized — §4, and `docs/stdlib.md`. |
| Was #4 | **`struct dirent` layout** | **CLOSED.** Linux's `getdents64` record, matching `DT_*`. |
| Was #5 | **`O_CREAT` value diverges** | **CLOSED** — fixed in Phase 14. |
| Was #7 | **No `clone`/`futex`** | **HALF CLOSED.** `futex` exists with Linux semantics, and pthread mutexes, condvars and POSIX semaphores are built on it. There is still no `clone`. |
| Was #8 | **No `clock_gettime`/`nanosleep`** | **MOSTLY CLOSED.** Both exist; resolution is 10ms and there are no absolute timeouts — §8a. |
| Was #9 | **13-character filenames** | **CLOSED** (a FAT constraint, not an ABI one): VFAT long names, read and write. |

Since this report was last written, a per-process **working directory**
landed: `chdir`/`getcwd`, inherited by fork and spawn, with every
path-taking syscall resolving through it. That was the largest single
item in `docs/porting-coreutils.md`'s Tier 1, because it changed
`struct process` rather than adding a call.

New since Phase 10: futex, pipes, AF_INET datagram sockets over a
loopback stack, thread-local storage, the auxiliary vector, `spawnv`,
`fcntl`, an MPI subset, **musl 1.2.5 statically linked**, the `stat`
family, the working directory, VFAT long names, a line-discipline TTY,
a CMOS RTC behind `CLOCK_REALTIME`, and Tier 0 of the coreutils surface
(`writev`/`readv`, `ioctl`, `clock_gettime`, `nanosleep`,
`set_tid_address`, `exit_group`).

## 3. Syscalls

73 syscalls, numbered 0–72 in NeoOS's own space
(`kernel/syscall/syscall_nr.h`). Linux x86_64 numbers are unrelated;
the musl shim (`third_party/shim/`) maps them. Dispatch is a table
indexed by the number; an unimplemented number returns `-ENOSYS`, as on
Linux. (Number 66, `SYS_TEST_HOOK`, exists only in `-DNEOOS_TEST_HOOKS`
builds; it is not part of the stable ABI.)

| NeoOS | Name | Linux analogue | Status |
|-------|------|----------------|--------|
| 0 | exit | `exit` | implemented |
| 1/6 | write/read | `write`/`read` | implemented, now via a file-ops table |
| 2 | yield | `sched_yield` | implemented |
| 3 | getpid | `getpid` | implemented |
| 4 | spawn | *(none — NeoOS extension)* | implemented |
| 5 | wait | *(NeoOS wait-by-pid)* | implemented |
| 7 | open | `open` | implemented, **`O_CREAT` value diverges** (§5) |
| 8–11 | close/mkdir/unlink/lseek | same names | implemented |
| 12/13 | fork/exec | `fork`/`execve` | implemented (COW); **exec takes no argv**. Like `execve`, it terminates every other thread of the process first (and waits for them) before releasing the old address space |
| 14/15 | mount/umount | `mount`/`umount2` | implemented, NeoOS-shaped |
| 16 | getdents | `getdents64` | implemented, **struct now MATCHES** (§4) |
| 17–20 | thread create/exit/join/self | `clone`/`exit`/`futex`/`gettid` | NeoOS-shaped, **not `clone`** |
| 21–28 | rt_sigaction … sigaltstack | same names | implemented |
| 29–31 | kill / tkill / tgkill | same names | implemented |
| 32 | wait4 | `wait4` | implemented |
| 33–36 | setpgid/getpgid/setsid/getsid | same names | implemented |
| 37–39 | mmap / munmap / mprotect | same names | implemented, anonymous only; `mprotect` preserves page contents in both directions and supports `PROT_NONE` |
| 40–41 | cpu_count / getcpu | `sysconf`/`getcpu` | implemented |
| **42** | **futex** | `futex` | **NEW.** WAIT and WAKE only (§6) |
| **43** | **pipe2** | `pipe2` | **NEW.** `pipe()` is library code over it |
| **44** | **arch_prctl** | `arch_prctl` | **NEW.** SET_FS/GET_FS; GS codes refused |
| **45–50** | **socket/bind/connect/sendto/recvfrom/getsockname** | same names | **NEW.** AF_INET SOCK_DGRAM only (§7) |
| **51** | **spawnv** | *(what `execve`'s argv will become)* | **NEW** |
| **52** | **fcntl** | `fcntl` | **NEW.** F_GETFL/F_SETFL; F_GETFD/F_SETFD are no-ops; F_DUPFD refused |
| **53–54** | **chdir / getcwd** | same names | **NEW.** Per-process cwd; `..` resolved textually (§5) |
| **55–58** | **stat / lstat / fstat / newfstatat** | same names | **NEW.** Linux's 144-byte `struct stat`; most fields synthesized (§4) |
| **59–65** | **set_tid_address / exit_group / writev / readv / ioctl / clock_gettime / nanosleep** | same names | **NEW.** Tier 0: what musl needs before `main`. `ioctl` on a terminal answers `TCGETS`/`TCSETS`/`TIOCGWINSZ` (§8b); `CLOCK_REALTIME` is anchored to the CMOS RTC read at boot (§8a) |
| **66** | **test_hook** | *(test-only, not part of stable ABI)* | **NEW.** `-DNEOOS_TEST_HOOKS` only: injects key events, exposes the user-migration counter, and reads a pid's parent for deterministic headless testing. Returns `-ENOSYS` in production. |
| **67–68** | **poll / select** | same names | **NEW (M1a).** A subset — see §9; fd-set is 16×u64, `nfds` ≤ 16. |
| **69** | **reboot** | `reboot` | **NEW (M2).** `POWER_OFF`/`HALT`/`RESTART`; **PID-1-only** instead of `CAP_SYS_BOOT` (§8d). |
| **70–72** | **dup / dup2 / dup3** | same names | **NEW (M1b-3).** Standard semantics; the only way to rebind fds 0/1/2, which `open()` never returns. `dup3` accepts only `O_CLOEXEC`. |

**musl now runs.** The shim (`third_party/shim/`) translates Linux's
numbers onto these, and `[musltest]` in `make test` is a real musl
binary using printf, malloc, stat, opendir and stdio.

**Absent, and reached early by a real program:** `dup`/`dup2`,
`set_robust_list`, `getuid`/`geteuid`/`getgid`, `uname`, `umask`,
`chmod`, `rename`, `rmdir`, `ftruncate`,
`poll`/`select`/`epoll`, `clone`, `listen`/`accept`, `setsockopt`, and
the whole *at* family (`openat`, `unlinkat`, ...). `brk` is absent and
that is fine -- musl's mallocng falls back to `mmap`. See
`docs/porting-coreutils.md` for which of these actually block a port.

## 4. Struct layouts crossing the boundary

| Struct | Verdict |
|--------|---------|
| `struct k_sigaction` | **Matches** Linux's kernel `sigaction`. |
| `struct siginfo` | **Matches** in size — 128 bytes. |
| `struct sockaddr_in` | **Matches** — 16 bytes, family/port/addr/zero at 0/2/4/8. Asserted at boot by `socket_selftest`. |
| `struct timespec` | **Matches** — two 64-bit signed fields. Defined; nothing consumes it from userland yet except futex timeouts. |
| The auxv | **Matches** — pairs of `unsigned long`, `AT_NULL`-terminated, at the documented place on the entry stack. |
| `struct dirent` | **Matches** — Linux's `getdents64` record: d_ino/d_off/d_reclen/d_type and a variable-length d_name at offset 19, 8-byte aligned. `DT_*` are Linux's values. Names carry the full VFAT long name (up to 255 characters). |
| `struct stat` | **Matches** — Linux's x86-64 144-byte layout, asserted at compile time and from userland. Field *values* diverge: mode bits, owner and link count are synthesized, and all three timestamps are 0, because FAT stores none of them (`CLOCK_REALTIME` now exists but nothing records file times with it). See `docs/stdlib.md`. |
| `struct termios` | **Matches** Linux's 36-byte kernel `termios` (NCCS 19), a prefix of musl's larger userland struct — the TTY writes only that prefix. `struct winsize` matches. c_cc indices, `I*`/`O*`/`C*`/`L*` flags and the `TC*`/`TIOC*` ioctl numbers are Linux's. |
| `struct input_event` | **Matches** — 24 bytes: two `int64_t` timestamps, two `uint16_t` type/code, one `int32_t` value. Linux x86-64 layout. Asserted in userland and kernel. |
| `struct input_id` | **Matches** — 8 bytes: four `uint16_t` (bustype, vendor, product, version). |
| `struct utsname` | **Absent.** No `uname`. |
| `pthread_mutex_t`, `sem_t` | **DIVERGE**, and deliberately: they are NeoOS's own definitions, and musl replaces them wholesale. Nothing is compiled against them yet, so there is no compatibility to preserve. |

## 5. Constants and flags

| Family | Verdict |
|--------|---------|
| `errno` | **Matches** Linux x86_64, now including `EFAULT` 14, `EPIPE` 32, `ESPIPE` 29, `EMSGSIZE` 90, `EAFNOSUPPORT` 97, `EADDRINUSE` 98, `EADDRNOTAVAIL` 99, `ENETUNREACH` 101, `ENOTCONN` 107, `ERANGE` 34, `ENAMETOOLONG` 36. |
| Signal numbers | **Matches**: SIGKILL 9, SIGSEGV 11, SIGPIPE 13, NSIG 65. |
| `PROT_*`, `MAP_*` | **Match** for the subset implemented. **DIVERGES on W^X:** `mmap`/`mprotect` reject `PROT_WRITE\|PROT_EXEC` with `-EINVAL`, and the ELF loader refuses a `W`+`X` `PT_LOAD` segment. Linux permits W+X (subject to lockdown/SELinux). See §5a. |
| `AF_INET` (2), `SOCK_DGRAM` (2), `SOCK_STREAM` (1), `INADDR_*` | **Match.** |
| `AT_*` (auxv) | **Match**: `AT_PHDR` 3, `AT_PHENT` 4, `AT_PHNUM` 5, `AT_PAGESZ` 6, `AT_ENTRY` 9, `AT_RANDOM` 25. |
| `ARCH_SET_FS` (0x1002), `ARCH_GET_FS` (0x1003) | **Match.** |
| `FUTEX_WAIT`/`FUTEX_WAKE`/`FUTEX_PRIVATE_FLAG` | **Match** (0, 1, 128). |
| `F_GETFL`/`F_SETFL`, `O_NONBLOCK` (0x800), `O_CLOEXEC` (0x80000) | **Match.** |
| `O_*` (open) | **Match** (`O_RDONLY` 0, `O_WRONLY` 1, `O_RDWR` 2, `O_CREAT` 0x40, `O_EXCL` 0x80, `O_TRUNC` 0x200, `O_APPEND` 0x400, `O_NONBLOCK` 0x800, `O_DIRECTORY` 0x10000, `O_CLOEXEC` 0x80000). |
| `CLOCK_*` | **Match** (`CLOCK_REALTIME` 0, `CLOCK_MONOTONIC` 1, ...). All ids resolve; `CLOCK_REALTIME` is wall time anchored to the CMOS RTC, the rest count from boot. Resolution is one 10ms tick (§8a). |
| `TCGETS`/`TCSETS`/`TIOCGWINSZ` etc. | **Match** — Linux's ioctl numbers (§8b). |
| `VT_*` / `KD*` ioctl numbers | **Match** — `VT_OPENQRY` 0x5600, `VT_GETMODE` 0x5601, `VT_SETMODE` 0x5602, `VT_GETSTATE` 0x5603, `VT_RELDISP` 0x5605, `VT_ACTIVATE` 0x5606, `VT_WAITACTIVE` 0x5607, `KDSETMODE` 0x4B3A, `KDGETMODE` 0x4B3B; `KD_TEXT` 0, `KD_GRAPHICS` 1. `struct vt_stat` is Linux's three `unsigned short`s. **Diverges:** 6 VTs not 63, and `VT_SETMODE`/`VT_RELDISP` are inert — see §8e. |
| `EV_*` (input event types) | **Match** (`EV_SYN` 0, `EV_KEY` 1, `EV_MSC` 4, etc.). `EV_VERSION` is 0x010001. |
| `KEY_*` (keyboard codes) | **Match** Linux x86-64 values for the US keyboard subset (Set-1 AT scancode mapping). |
| `MSC_*` (miscellaneous codes) | **Match** (`MSC_SCAN` 4). |
| `SYN_*` (sync codes) | **Match** (`SYN_REPORT` 0). |
| `BUS_*` (bus type) | **Match** (`BUS_I8042` 0x11 for the AT keyboard). |
| `EVIOC*` ioctl numbers | **Match** Linux's `<linux/input.h>`: `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGBIT`, `EVIOCGKEY`, `EVIOCGRAB`, etc. One device only, `/dev/input/event0`. |


### 5a. W^X — a deliberate divergence

NeoOS enforces "no page is both writable and executable", where Linux
only discourages it:

- **ELF loader** honours `p_flags`: a read-only segment (`.text`,
  `.rodata`) is mapped without `PAGE_WRITABLE`, so a stray write faults
  (`SIGSEGV`) instead of silently succeeding. A `PT_LOAD` segment that
  is `W` and `X` in the file is refused and `execve` / spawn fails. No
  toolchain emits such a segment.
- **`mmap` / `mprotect`** return `-EINVAL` for any `prot` containing
  both `PROT_WRITE` and `PROT_EXEC`.
- **Kernel address space**: `.text` is read-only + executable,
  everything else (`.rodata`, `.data`, `.bss`, the physmap) is
  non-executable. Asserted at boot by `[wxorx] kernel selftest`.

**What a ported application hits:** a JIT or trampoline generator that
`mmap`s a single `RWX` region will get `-EINVAL`. The fix on the
application side is the same one modern Linux hardening already forces:
keep two mappings of the same pages (one `RW`, one `RX`), or `mprotect`
between `RW` and `RX` around each code-generation step. Recorded in
`docs/stdlib.md`.

## 6. futex, and what is built on it

`futex(uaddr, op, val, timeout)` with Linux's operation numbers,
argument order and return values. **`FUTEX_WAIT` and `FUTEX_WAKE`
only.** `FUTEX_PRIVATE_FLAG` is accepted and ignored, because NeoOS
keys every futex by physical address — correct for private and shared
alike, so the hint has nothing to change.

**DIVERGES:** no `FUTEX_REQUEUE`/`CMP_REQUEUE`, so a condition
variable's broadcast wakes every waiter instead of moving them to the
mutex's queue. Correct, but a thundering herd, and musl's
`pthread_cond_broadcast` will take the slower path. No `*_BITSET`, no
`FUTEX_WAKE_OP`, no priority-inheritance futexes. Timeouts are relative
only and rounded up to a 10ms tick.

`<semaphore.h>` and `<pthread.h>` (mutexes, condvars, and a small
threads subset) are built on it in `lib/` and are placeholders: musl
supplies all of them, on this same syscall, unchanged.

## 7. Sockets and the network stack

`socket`/`bind`/`connect`/`sendto`/`recvfrom`/`getsockname`, with
Linux's `sockaddr_in` layout, over a real IPv4 and UDP path on a
loopback device — real headers, real checksums, real port demux. A
socket is an ordinary file descriptor, so `close`, `fork` inheritance,
and `read`/`write` on a connected socket all work.

**DIVERGES / absent:** one interface and it is loopback; **no TCP**
(`SOCK_STREAM` returns `-EPROTONOSUPPORT` rather than silently giving
datagram semantics); no `AF_UNIX`, no IPv6; no `MSG_*` flags at all,
including `MSG_DONTWAIT` and `MSG_TRUNC`; no `setsockopt`/`getsockopt`;
no `select`/`poll`; no ICMP and no raw sockets.

## 8. Process startup contract

| Item | State |
|------|-------|
| ELF loading | static ELF64 only; no `PT_INTERP`, no dynamic linker |
| **auxv** | **Supplied.** AT_PHDR/PHENT/PHNUM/PAGESZ/ENTRY/RANDOM. |
| Initial stack | **argc/argv/envp/auxv, SysV layout, 16-byte aligned.** `argv[0]` is the spawn path. |
| **envp** | present and NULL-terminated, but always **empty** — nothing sets an environment |
| **TLS** | **Working.** `arch_prctl(ARCH_SET_FS)`, variant-II layout, `%fs:0` self-pointer, per-thread and restored across migration. Local-exec model only. |
| **AT_RANDOM** | **PRNG, not cryptographic.** Seeded from RTC ⊕ TSC ⊕ stack address ⊕ RDRAND (if available); generated via splitmix64 + xoshiro256**. Not an entropy pool — deterministic seed, no reseeding. Adequate for the stack guard canary, not for keys. |
| Signal frame | NeoOS-shaped; **not** Linux's `rt_sigframe` |
| Stack alignment | SysV 16-byte at entry — matches |

### 8a. The clock — DIVERGES in precision, not in shape

`clock_gettime` and `nanosleep` exist with Linux's argument order.
NeoOS's only time source is the 100Hz LAPIC tick, so **resolution is
10ms**. `CLOCK_REALTIME` is anchored to the CMOS RTC read once at boot;
`CLOCK_MONOTONIC` and the CPU-time clocks count from boot. If the RTC
cannot be read, `CLOCK_REALTIME` silently falls back to a boot epoch
and formats as January 1970. `nanosleep` rounds up to a whole tick and
ignores the remaining-time argument (nothing interrupts a sleep
partway). `stat`'s timestamps are still 0 — nothing records file times.

### 8b. The terminal

`/dev/CONSOLE` is a real line-discipline TTY: canonical mode with echo
and line editing, `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`,
`TIOCGWINSZ`/`TIOCSWINSZ`, `TIOCGPGRP`/`TIOCSPGRP`, and `SIGINT`/
`SIGQUIT`/`SIGTSTP` generated from `c_cc`. `struct termios` is Linux's
36-byte kernel layout. `isatty` on it returns 1; on a file, pipe or
socket `ioctl` returns `-ENOTTY` as Linux does.

### 8c. evdev — raw keyboard input

`/dev/input/event0` is a Linux evdev character device (a single input
device for the AT keyboard). It exports `struct input_event` records
(24 bytes, Linux x86-64 layout) with wall-clock timestamps, event types
(`EV_MSC`, `EV_KEY`, `EV_SYN`), and key codes (`KEY_*`) for the US
keyboard layout (Set-1 AT scancodes).

Supported `ioctl`s: `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`, `EVIOCGBIT`,
`EVIOCGKEY`, `EVIOCGRAB`. Ring buffer holds 256 events per open fd; on
overflow the oldest event is dropped (value field of `SYN_REPORT`
incremented to signal it). **Diverges:** US keyboard only, no other
input devices, no `EVIOCSCLOCKID`, ring overflow behavior differs from
Linux (oldest-drop with a counter vs. a kernel-wide shared buffer), and
`read` on an empty ring always returns `-EAGAIN` — it never blocks, so a
reader must `poll`/`select` (both report `POLLIN` correctly) rather than
rely on a blocking `read`. See `docs/stdlib.md` for the lock-rank reason.

### 8d. init and shutdown (M2)

The kernel starts `/SBIN/INIT` as PID 1 and nothing else. INIT reads
`/ETC/INITTAB` (`spawn`/`wait`/`respawn` entries), launches the
workload, and reaps children and reparented orphans in a `wait4(-1)`
loop until `-ECHILD`, then calls `reboot(LINUX_REBOOT_CMD_POWER_OFF)`.

- **Orphan reparenting matches Linux**: a dying process's live children
  are handed to PID 1. No subreaper mechanism.
- **`reboot(2)` is PID-1-only** — NeoOS has no uids or `CAP_SYS_BOOT`.
  `POWER_OFF`/`HALT`/`RESTART` command words carry Linux's magic-2
  values.
- **Shutdown sends no `SIGTERM`/`SIGKILL`** to survivors; a ported
  service manager would notice. A stopped orphan reparented to PID 1 is
  not woken (no orphaned-process-group `SIGHUP`+`SIGCONT`), so PID 1's
  `wait4` would block on it forever.
- If PID 1 exits, the kernel panics.

### 8e. Virtual terminals (M1c-3)

`/dev/tty1`..`/dev/tty6` are six independent kernel VTs, each a full
line-discipline terminal with its own grid and scrollback; `/dev/tty0`
(and `/dev/CONSOLE`) resolve to whichever is active. `Alt+F1`..`Alt+F6`
switch, `Shift+PageUp`/`PageDown` scroll, and both are consumed by the
kernel before evdev or any tty sees them. The `VT_*`/`KD*` ioctls are
Linux's numbers with Linux's meanings (table in §5).

**What a ported console app hits.** `chvt`, `openvt -s`, `deallocvt` and
`fgconsole` work as far as `VT_ACTIVATE`/`VT_GETSTATE` take them. What
does not work is the **`VT_PROCESS` handshake**: `VT_SETMODE` is accepted
and ignored, so a process cannot ask to be signalled before a switch and
cannot acknowledge one with `VT_RELDISP`. Every switch is `VT_AUTO` and
happens immediately, whatever is on screen. An X server, a Wayland
compositor or `svgalib` — anything that must save and restore the video
state around a switch — therefore cannot cooperate with console
switching yet. `KDSETMODE(KD_GRAPHICS)` does work, so such a program can
at least stop the kernel drawing over it. Raw keyboard modes
(`KDSKBMODE`, `K_RAW`, `K_MEDIUMRAW`), the keymap ioctls (`KDGKBENT`
&c.) and `/dev/vcs*` are absent; a program wanting scancodes uses
`/dev/input/event0` (§8c).

## 9. What a real ported application hits, in order

1. **`O_CREAT` value** — silent failure to create files. One constant,
   called out since Phase 10.
2. **No `errno` variable** — every error check reads the wrong thing.
   Fixed by the musl shim, not the kernel.
3. **No `clone`** — threads exist, but not through the call a libc
   makes, so musl's pthreads cannot sit on them.
4. **10ms clock resolution and no absolute timeouts** — why
   `sem_timedwait` and `pthread_cond_timedwait` diverge into
   relative-timeout spellings, and why `stat` times are all 0.
5. **`poll`/`select` are a subset** (M1a): `POLLIN/OUT/ERR/HUP/NVAL`
   only, no `epoll`, no `ppoll`/`pselect6` sigmask, `nfds` capped at 16,
   10 ms timeout resolution. Enough for a two-fd event loop, not for a
   server.
6. **No `dup`/`dup2`, no `F_DUPFD`** — shell redirection.
7. **No `getuid`/`uname`/`umask`/`chmod`/`rename`/`rmdir`/`ftruncate`**
   — Tier 2 of `docs/porting-coreutils.md`: makes the tools honest
   rather than merely running.
8. **`exec` takes no argv** — `spawnv` (51) is a separate call with
   different semantics; a real `execve(argv, envp)` is still owed.
9. **No shutdown signal** (M2): PID 1 powers off once nothing is left to
   reap, without first `SIGTERM`ing anyone; no sessions, no `/proc`, no
   orphaned-process-group handling.

**Closed since Phase 10:** the `stat` family, `struct dirent`'s layout,
the working directory, `writev`/`ioctl`/`clock_gettime`, TLS and the
auxv, and — a FAT constraint rather than an ABI one — the 13-character
filename limit, now VFAT long names.

**New in M1a (console plumbing):** a Linux-shaped `/dev/fb0` (`mmap` +
`FBIOGET_VSCREENINFO`/`FSCREENINFO`, no mode setting), a `poll`/`select`
subset (above), and a PTY subsystem (`/dev/ptmx`, `/dev/pts/N`,
`TIOCGPTN`; `grantpt`/`unlockpt` no-ops; `TIOCSWINSZ` stored but no
`SIGWINCH`; no `SIGHUP` on master close — session hang-up and job
control are M2). Full detail and every divergence in `docs/stdlib.md`.

**New in M2 (init):** `/SBIN/INIT` runs as PID 1 from `/ETC/INITTAB`,
orphans reparent to it, and `reboot(2)` (PID-1-only) is the shutdown
path (§8d). Job control and session hang-up are still owed.

## 10. Summary

The **process startup contract is now largely correct**: a program gets
a real entry stack, a working auxiliary vector, and thread-local
storage, which were the top two blockers at the close of Phase 10. The
**synchronisation substrate is Linux-shaped** — futex, with everything
POSIX built on it — so musl's pthreads will land on it without
emulation. The **file-metadata surface** — the `stat` family, `struct
dirent`, the working directory — is now in place, and a static musl
binary runs. The **error space, signal numbers, `sockaddr_in`, the
auxv, `struct stat`, `struct dirent` and `struct termios` all match**.

What stands between NeoOS and an unmodified multi-threaded binary is now
**`clone`** (musl's pthreads) and **`select`/`poll`**. The `O_CREAT`
constant remains the cheapest outstanding fix. Field *values* in
`struct stat` — timestamps especially — are the remaining semantic
divergence, recorded in `docs/stdlib.md`.
