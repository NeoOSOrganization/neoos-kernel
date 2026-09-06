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
| 12/13 | fork/exec | `fork`/`execve` | implemented (COW); argv crosses exec (a3), `envp` does not. Like `execve`, it terminates every other thread of the process first (and waits for them) before releasing the old address space |
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
| **51** | **spawnv** | *(no Linux analogue: spawn with argv)* | **NEW** |
| **52** | **fcntl** | `fcntl` | **NEW.** F_GETFL/F_SETFL; F_GETFD/F_SETFD are no-ops; F_DUPFD refused |
| **53–54** | **chdir / getcwd** | same names | **NEW.** Per-process cwd; `..` resolved textually (§5) |
| **55–58** | **stat / lstat / fstat / newfstatat** | same names | **NEW.** Linux's 144-byte `struct stat`; most fields synthesized (§4) |
| **59–65** | **set_tid_address / exit_group / writev / readv / ioctl / clock_gettime / nanosleep** | same names | **NEW.** Tier 0: what musl needs before `main`. `ioctl` on a terminal answers `TCGETS`/`TCSETS`/`TIOCGWINSZ` (§8b); `CLOCK_REALTIME` is anchored to the CMOS RTC read at boot (§8a) |
| **66** | **test_hook** | *(test-only, not part of stable ABI)* | **NEW.** `-DNEOOS_TEST_HOOKS` only: injects key events, exposes the user-migration counter, and reads a pid's parent for deterministic headless testing. Returns `-ENOSYS` in production. |
| **67–68** | **poll / select** | same names | **NEW (M1a).** A subset — see §9; fd-set is 16×u64. `select` has no cap below `FD_SETSIZE` (CS2); `poll` is still `nfds` ≤ 16. |
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

*(Superseded by the D2–D5 refresh at the end of this document: there is
now a NIC, a link layer, ARP, routing, ICMP, DHCP and TCP. The
divergences that survive are listed there.)*

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

The kernel starts `/sbin/init.nex` as PID 1 and nothing else. INIT reads
`/etc/inittab` (`spawn`/`wait`/`respawn` entries), launches the
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
5. **`poll`/`select` are a subset** (M1a, narrowed by CS2):
   `POLLIN/OUT/ERR/HUP/NVAL` only, no `epoll`, no `ppoll`/`pselect6`
   sigmask, 10 ms timeout resolution. **`select` no longer has an
   `nfds` cap below `FD_SETSIZE`** — it sizes its descriptor array to
   the caller's request and reports every ready fd, matching Linux;
   until CS2 it collected into a fixed 16-entry array and *silently
   dropped* the rest, which was a correctness bug rather than a
   documented limit. **`poll` is still capped at 16**, returning
   `EINVAL` above that — a live divergence CS4 removes.
6. **No `dup`/`dup2`, no `F_DUPFD`** — shell redirection.
7. **No `getuid`/`uname`/`umask`/`chmod`/`rename`/`rmdir`/`ftruncate`**
   — Tier 2 of `docs/porting-coreutils.md`: makes the tools honest
   rather than merely running.
8. **No `SIGTTIN`/`SIGTTOU` generation.** They are defined, treated as
   stop signals, and `TIOCSCTTY`/`TIOCSPGRP` let a shell own a terminal
   and run job control — but a *background* process reading or writing
   the terminal is not signalled. Nothing exercised so far needs it.
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

**New in M2 (init):** `/sbin/init.nex` runs as PID 1 from `/etc/inittab`,
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

## Refresh — end of the concurrency and BusyBox milestones (2026-09-02)

**Closed since the last refresh:**

- `execve`/`spawnve` carry **argv and envp** (1024 arguments, 4096 bytes
  each, 256 KiB total, `-E2BIG` past any of them). `exec` previously
  discarded the argument vector entirely.
- The entry stack is the full SysV shape: `argc`, `argv[]`, NULL,
  `envp[]`, NULL, auxv.
- `getppid`, `uname`, `brk`, `getuid`/`geteuid`/`getgid`/`getegid`,
  `poll` and `select` (the last two had never been mapped in the shim at
  all), `fcntl(F_DUPFD`/`F_DUPFD_CLOEXEC)`, `TIOCSCTTY`.
- File descriptors are allocated **lowest-available from 0**, so shell
  redirection works.
- `poll()` accepts up to `FD_TABLE_MAX` descriptors, not 16.
- 1024 threads per process, 256 ptys, PID wraparound.
- A minimal read-only **`/proc`** (`<pid>/stat`, `<pid>/cmdline`).

**Two ABI-visible bugs fixed, both found by running BusyBox:**

- `fork()` left the child with **no thread pointer** — the trampoline's
  `mov fs, dx` zeroes `IA32_FS_BASE`. Any libc using thread-local
  storage died on its first instruction in the child. This is the single
  most important compatibility fix in the milestone: it is the
  difference between "fork+exec works" and "fork works".
- A pty hung up when a **forked child closed its inherited master fd**,
  delivering EOF to a shell before anything was typed.

**What a ported application would still hit:** no dynamic linking (DL is
deferred, and everything here is static); no `SIGTTIN`/`SIGTTOU`
generation; `brk` never grows; `uname` reports `NeoOS`; no users, modes
or ownership; `/proc` has only the two files `ps` reads; FAT has no
symlinks. Each is recorded with its reasoning in `docs/stdlib.md`.

## Refresh — end of the network track, D0–D5 (2026-09-05)

The stack went from "IPv4 and UDP over loopback" to "a machine on a
network". PCI enumeration (D0), a virtio-net driver (D1), Ethernet and
ARP (D2), ICMP (D3), UDP on the wire with a DHCP client (D4), and TCP
(D5).

**What of the socket ABI is now implemented**

| Call | State |
|---|---|
| `socket`, `bind`, `connect`, `sendto`, `recvfrom`, `getsockname` | implemented, `SOCK_DGRAM` and `SOCK_STREAM` |
| `listen`, `accept`, `accept4`, `shutdown`, `getpeername` | implemented (D5) |
| `setsockopt`, `getsockopt` | implemented for a short, explicit option list |
| `read`, `write`, `close`, `poll`, `select`, `fcntl(O_NONBLOCK)` on a socket | implemented |
| `sendmsg`, `recvmsg`, `socketpair`, `sendfile` | absent |

`SOCK_STREAM` is a real TCP: all eleven states, Reno congestion
control, Nagle, delayed ACK, out-of-order reassembly,
Jacobson/Karels RTO with Karn's algorithm, a persist timer, and
retransmission with exponential backoff. It has been driven against a
peer this kernel does not control — a host-side echo server reached
through the NIC — and against deliberately injected 1-in-8 packet loss.

**Constants** all carry Linux's values: `SOL_SOCKET`, `SO_REUSEADDR`,
`SO_ERROR`, `SO_TYPE`, `SO_SNDBUF`, `SO_RCVBUF`, `IPPROTO_TCP`,
`TCP_NODELAY`, `SHUT_RD`/`SHUT_WR`/`SHUT_RDWR`, `SOCK_NONBLOCK`,
`SOCK_CLOEXEC`, and the errnos the stack returns (`ECONNREFUSED` 111,
`ECONNRESET` 104, `EINPROGRESS` 115, `EALREADY` 114, `ENOPROTOOPT` 92,
`EHOSTUNREACH` 113, `ETIMEDOUT` 110).

**Semantics matched deliberately, where an application can tell**

- `connect` to a closed port returns `ECONNREFUSED`, not `ECONNRESET`.
  A RST answering our SYN means the port is shut, not that a connection
  broke — telling a program otherwise breaks every retry loop written
  against Linux.
- `bind` to a non-local address returns `EADDRNOTAVAIL`. This was a bug
  until D2: the check asked whether the address was *routable*, which
  is the same question as *local* only while loopback is the sole
  interface. The moment a default route existed, `bind(8.8.8.8)`
  succeeded.
- A UDP datagram to a port with no socket provokes an ICMP port
  unreachable, as on Linux. A *connected* socket filtering out a
  stranger does not: the port is open, it is merely not listening to
  them.
- Reading `SO_ERROR` clears it, so a non-blocking connect reports its
  result exactly once.
- `poll` on a listening socket reports `POLLIN` when a connection is
  waiting to be accepted; on a connected one, EOF counts as readable
  and a failed connection is writable, so a poll loop learns a verdict
  rather than waiting for one forever.

**Divergences a ported application could observe**

- **MSL is 5 seconds** (TIME_WAIT 10, not 120). A port becomes reusable
  sooner than on Linux.
- **Thirty-two TCP connections machine-wide**, from a static table, so
  that nothing allocates on the receive path. The thirty-third gets
  `ECONNREFUSED`, not `EMFILE`. Combined with the short TIME_WAIT this
  caps sustained connection churn at roughly thirty-two closes per ten
  seconds; a program that loops open/close will see `ECONNREFUSED` with
  nothing actually broken.
- **`SO_SNDBUF`/`SO_RCVBUF` are accepted and ignored**, reading back the
  real fixed 32 KiB.
- **DHCP runs in the kernel**, so there is no client to signal, no lease
  to inspect or renew, and no `/etc/resolv.conf`. DHCP option 6 is
  parsed and stored and nothing reads it.
- **No resolver at all**: no `gethostbyname`, no `getaddrinfo`. Every
  address is numeric.
- **The routing table is not reachable from userland** — no `route(8)`,
  no `AF_NETLINK`, no `SIOCADDRT`.
- **No raw sockets and no `AF_PACKET`**, so ICMP exists but `ping(8)`
  cannot: the kernel answers and originates echo requests, and userland
  has no way to.

**What a real ported application still hits**

No IPv6. No `AF_UNIX` sockets, which a surprising amount of software
assumes for local IPC. No `sendmsg`/`recvmsg`, so anything passing file
descriptors or using scatter-gather I/O on a socket fails to build. No
window scaling, SACK, or timestamps, so throughput over a
high-bandwidth-delay path will be poor — correct, but poor. No
`getaddrinfo`, so any program that resolves a name needs patching, which
is the single most likely reason a network application will not run
unmodified today.

## Refresh — AC97 audio driver (2026-09-05)

The first audio device: an AC97 controller (QEMU's `-device AC97`,
Intel 82801AA emulation), with an ALSA-shaped userland surface at
`/dev/snd/controlC0` and `/dev/snd/pcmC0D0p`.

**Implemented:** `open`/`ioctl`/`write` on both nodes, with real
`SNDRV_*` ioctl numbers and `struct snd_pcm_hw_params`/
`snd_pcm_sw_params`/`snd_ctl_card_info` layouts (byte-verified against
upstream Linux's `include/uapi/sound/asound.h` via `_Static_assert` and
a standalone compile check, not reconstructed from memory) for
`PVERSION`, `CARD_INFO`, `HW_PARAMS`, `SW_PARAMS`, `PREPARE`, `START`,
`DROP`.

**Stubbed/absent:** capture (`/dev/snd/pcmC0D0c`), any mixer control
beyond `PVERSION`/`CARD_INFO`, format negotiation beyond one fixed
16-bit/stereo/48kHz configuration, `mmap`-based playback (real ALSA
clients commonly prefer mmap; this driver only supports the `write()`
path).

**What a real ported application would hit:** any app that calls
`snd_pcm_hw_params_set_format_first`/similar to auto-negotiate the
"best" format rather than asking for S16_LE/2ch/48kHz directly may see
`-EINVAL` where real hardware would have offered a different format
that also happened to work. An app hard-coded to CD-quality stereo PCM
(extremely common for game sound effects, this driver's actual
motivating use case) works unmodified.

**A real hardware-sharing lesson, not an ABI note:** AC97 and
virtio-net can land on the same PCI interrupt line under QEMU's
default slot assignment. The first attempt at handling that shared a
single IOAPIC vector between both drivers' interrupt handlers — the
standard way real operating systems handle shared PCI IRQs, but unsafe
here because `virtio_net_irq()` unconditionally acknowledges its
device's ISR register with a real side effect, and was never written
to tolerate a spurious call. Every AC97 completion interrupt then
caused an extra ack on the virtio device, which surfaced as an
unrelated TLB shootdown selftest failing intermittently. The shipped
fix places AC97 at a distinct PCI address (`addr=0x6` in the Makefile's
`QEMU_COMMON`) so the two never share a line at all; the vector-sharing
code remains only as an honest fallback that skips routing (rather than
sharing unsafely) if some future configuration collides again. Any
future driver sharing an IOAPIC vector with an existing one must
confirm that existing one's IRQ handler is provably safe to call
spuriously before doing so.

## Refresh — fault-tolerant user-copy machinery (2026-09-06)

**The bug this closes:** `sys_read()` (and, it turned out, nearly every
other syscall touching a user buffer) passed the raw user pointer
straight into the underlying object's `read`/`write`/fill routine
(`fatfs_read`, `net_udp_output`, `vfs_stat_vnode`'s caller, ...), which
dereferences it from kernel context with no fault-recovery of any kind.
A destination page that had never been touched (a fresh `malloc()`
region, the common case for a program's very first `read()` into a
buffer) has no frame mapped yet; the resulting page fault happened at
CPL0 and hit the "kernel bug, halt" path instead of being demand-paged
in. Found via the neoos-doom port's WAD loader, which does exactly
that; general enough that it could halt the kernel on any large-enough
first read anywhere in userland.

**The fix:** a real exception-table mechanism, matching what Linux and
every other production kernel does — not a per-syscall patch.
`copy_to_user`/`copy_from_user` (`kernel/mm/uaccess.h`/`.c`) copy byte
by byte inside an inline-asm block whose instruction address is
recorded in a `.ex_table` linker section alongside a fixup address.
`kernel/arch/isr.c`'s page-fault dispatcher, on a ring-0 fault whose
`rip` matches an exception-table entry, first tries `vma_fault()` (the
same demand-paging path ring-3 faults already used) — if the process
has a real VMA covering the address, the frame is installed and the
faulting instruction retried transparently. Only a genuinely invalid
address falls through to the recorded fixup, which reports a partial
byte count back to the C caller instead of halting the machine. Every
OTHER ring-0 fault, at any other address, is unaffected — this is an
opt-in for instructions the kernel itself marked recoverable, not a
blanket "ring-0 faults are fine" rule.

**Converted to the new primitive, full audit (not just the crashing
site):** every unguarded direct dereference of a user pointer across
`sys_read`/`sys_write`/`sys_readv`/`sys_writev`/`sys_getdents`,
`stat`/`lstat`/`fstat`/`newfstatat`/`getcwd`/`pipe2` (sys_file.c),
`clock_gettime`/`nanosleep` (sys_misc.c), `wait4`/`thread_join`
(sys_proc.c), the full `rt_sig*`/`sigaltstack` family (sys_signal.c),
`arch_prctl(ARCH_GET_FS)`/`getrandom` (sys_mem.c), `poll`/`select`
(sys_poll.c), and the socket family's address/option marshalling plus
`sendto`/`recvfrom`/generic `read`/`write` on a socket fd (net/socket.c,
including `net_udp_output`'s destination-buffer safety — sendto now
copies the whole payload into a kernel staging buffer before that call
rather than handing it a raw user pointer). Two sites were explicitly
excluded, by design rather than oversight: `ipc/futex.c` (the futex
word must be touched atomically; a byte-loop copy cannot preserve
that), and `copy_user_vector` in sys_proc.c's exec-argument path
(already correct via a proven, independent incremental-validation
approach predating this work).

**Locking, checked at every conversion:** `copy_to_user`/
`copy_from_user`'s fault path takes `mm_lock` (`LOCK_RANK_MM` = 3) via
`vma_fault`. Every converted site was checked for a lock ranked >= 3
still held at the moment of copy; `sys_rt_sigaction` was the one real
case (`p->lock`, taken via `spin_lock_irqsave` — IRQs off, and a fault
under IRQs-off spinning into `vma_fault`'s allocation/sleep path would
be its own bug) and was restructured to copy in before the lock and
copy out after, rather than across it.

**A second bug this audit found along the way, same family:** several
already-guarded sites (`sys_mem.c`'s `getrandom`, `sys_poll.c`'s
`poll`/`select`, `net/socket.c`'s address/option calls) used
`user_range_writable`/`user_range_readable` — a pre-flight check for
`PAGE_PRESENT`, not a copy mechanism — which rejects a valid-but-
untouched page exactly like the crash above, just returning `-EFAULT`
instead of halting. Migrated onto `copy_to_user`/`copy_from_user` for
the same reason as the unguarded sites: a first-touch page should
demand-page in and succeed, not fail.

Verified: the full `[uaccess] selftest` (baseline correctness; a
process-less boot has no VMA list to make demand-paging or
invalid-address behavior meaningful to test, so those two properties
are verified by the real neoos-doom regression instead — see that
port's own history) plus a clean 15/15 gauntlet after each of the three
conversion passes (read/write family, stat/signal/wait family,
already-guarded migrations).

## Refresh — DNS resolution (2026-09-06)

The first prerequisite for porting real networked tools (curl, wget):
musl's unmodified `getaddrinfo()`/`gethostbyname()` now genuinely
resolve hostnames on NeoOS. Found and fixed via a feasibility spike,
not a planned milestone — investigated because `docs/stdlib.md`
recorded "no resolver" as a known gap, and porting curl/wget needs one.

**Two real, independent kernel-side bugs, not one:**

1. **`sendmsg`/`recvmsg` did not exist as syscalls at all.** musl's
   resolver (`res_msend.c`) sends its query via `sendto()` (already
   shimmed) but reads the reply via `recvmsg()` — the shim's `default:`
   case caught every call and returned `-ENOSYS`, so the reply
   genuinely sitting in the kernel's UDP queue was never read, and
   `getaddrinfo()` always failed with `EAI_AGAIN` after exhausting its
   retry budget. Fixed with real `SYS_SENDMSG`/`SYS_RECVMSG` syscalls
   (`net/socket.c`'s `socket_sendmsg`/`socket_recvmsg`, `SOCK_DGRAM`
   only — implemented by gathering/scattering through the existing
   `socket_sendto`/`socket_recvfrom`, using the `copy_to_user`/
   `copy_from_user` machinery from this milestone's own earlier work)
   plus the two shim entries (`LX_SENDMSG`=46, `LX_RECVMSG`=47).
   `struct k_msghdr`/`struct k_iovec` are Linux's x86-64 layouts,
   byte-verified at boot in `socket_selftest()` the same way
   `k_sockaddr_in` already was.

2. **Every unbound or wildcard-bound UDP socket's outgoing packets
   claimed a source address of `127.0.0.1`, regardless of actual
   destination.** `socket_sendto`'s `src_ip_n = s->local_ip_n ? ... :
   IP_LOOPBACK_N` substituted loopback unconditionally whenever a
   socket hadn't bound to a specific address — which a DNS query
   always does (an unbound-then-auto-bound or explicitly
   `bind()`-to-`INADDR_ANY` socket, exactly musl's own `res_msend.c`
   pattern). `net_udp_output` already had the CORRECT fallback for
   `src_n == 0` (the routed device's own `dev->ip_n`, which equals
   `IP_LOOPBACK_N` for a loopback destination and the real interface
   address otherwise) — `socket_sendto` was substituting loopback
   BEFORE that logic ever ran, so it never got the chance. A query
   claiming to come from `127.0.0.1` reached slirp's resolver (it
   replied to the raw kernel-level boot-time probe in
   `net/dnsprobe.c`, which passes `src_n=0` straight through and so
   was never affected), but nothing routed the reply back correctly.
   Found by writing a raw send/poll/recvmsg probe that bypassed
   musl's resolver entirely, tracing kernel-side UDP send/receive with
   temporary serial instrumentation (removed once the cause was
   confirmed), and comparing against `dnsprobe.c`'s own already-working
   pattern. Fixed by passing `s->local_ip_n` through unsubstituted.

**Also shipped:** `/etc/resolv.conf` (`nameserver 10.0.2.3` — slirp's
built-in resolver, the address `dnsprobe.c`'s selftest already queries)
is now generated at disk-image build time; without it musl's resolver
has no nameserver to ask at all. The nameserver is **static, not
DHCP-learned** — DHCP option 6 is still parsed and stored (`dhcp.c`)
but nothing wires it into this file yet, a real divergence for a
network whose DNS server isn't slirp's.

**Verified:** a throwaway userland program calling
`getaddrinfo("example.com", "80", ...)` resolves real addresses
(confirmed against the live internet through slirp's NAT); the 15/15
gauntlet stays green (MPI's UDP-over-loopback and every other existing
network user are all loopback-destined, so `dev->ip_n` still resolves
to `IP_LOOPBACK_N` for them exactly as the old substitution did — this
bug fix is additive, not a behavior change for anything already
working).

**What a real ported application still hits:** only `AF_INET`/
`SOCK_DGRAM`/`SOCK_STREAM` resolve (no `AF_INET6`, so a `getaddrinfo()`
call requesting only IPv6 results finds none); `msg_control` (ancillary
data — `SCM_RIGHTS`, timestamps) is not implemented; TLS is still
entirely absent, so `https://` URLs remain out of reach until that
lands separately.

## Refresh — libssh2 port: TCP send/recv routing and stream poll() (2026-09-06)

Porting libssh2 (a real SSH client library, the first thing to drive
NeoOS's TCP stack through anything but MPI's loopback UDP and a
hand-rolled getaddrinfo probe) found three real bugs — two in the
kernel, one in how the port was built — none in the design docs'
original DNS/OpenSSL/libssh2 sequencing. All three were needed
together: fixing only one still leaves an SSH session that cannot
finish a handshake.

**1. `socket_sendto`/`socket_recvfrom` had no `SOCK_STREAM` dispatch
at all.** Every `send()`/`recv()`/`sendto()`/`recvfrom()` call on a
connected TCP socket — which is what musl's libc routes `send()`/
`recv()` through — fell straight into the UDP-only path and called
`net_udp_output()`, firing a stray datagram at the peer's address from
a fresh, unrelated ephemeral port while the real TCP data was silently
never sent, yet the syscall reported success. `read()`/`write()`
(`sock_read`/`sock_write`) already dispatched correctly by `s->type`;
`sendto`/`recvfrom` simply never had the same check added. Invisible
until now because nothing before libssh2 called `send()`/`recv()` on a
stream socket — MPI uses `sendto`/`recvfrom` on UDP by design, and the
DNS milestone's traffic is UDP end to end. Fixed by giving
`socket_sendto`/`socket_recvfrom` the same `s->type == SOCK_STREAM`
dispatch `sock_read`/`sock_write` already had, staged through a 4096-
byte kernel buffer via `copy_to_user`/`copy_from_user` (this session's
uaccess machinery). `sendmsg`/`recvmsg` inherit the fix for free, since
they are built on `sendto`/`recvfrom`.

**2. `sock_poll_head()` returned a non-null head for stream sockets,
which silently defeats their only working wakeup path.** `poll_core()`
registers a waiter on a file's poll head if `poll_head()` returns one,
and OPTS OUT of the global poll broadcast (`poll_wants_broadcast = 0`)
when it does. But a stream socket's readiness changes — data arriving,
the send window reopening — are raised exclusively through `wake()` in
`tcp.c`, which calls `waitq_poll_notify()` (the global broadcast) and
never `poll_head_notify(&s->poll)`. Registering on `&s->poll` for a
stream socket therefore opts a poller out of the one mechanism that
actually fires, and into one that never does: `poll()`/`select()` on a
connected TCP socket would block forever, even past the moment data
it's waiting for has already arrived. Invisible until now for the same
reason as bug 1's blind spot inverted: every blocking `send()`/`recv()`
on a stream socket is implemented by `stream_send`/`stream_recv`
sleeping directly on `t->waiters`, which never touches `poll()` at
all — so this path is only exercised by a genuinely non-blocking
socket doing its own `poll()`-based wait, which nothing had done
before libssh2. Fixed by having `sock_poll_head()` return `0` for
`SOCK_STREAM` sockets specifically, opting them into the working
broadcast; a `SOCK_DGRAM` socket keeps its own head, since
`net_udp_deliver`'s `poll_head_notify(&s->poll)` is a real, working
per-object signal for that case.

**3. Not a kernel bug: libssh2's own cross-compile feature detection
silently failed shut.** `neoos-libssh2`'s CMake configure step probes
`HAVE_O_NONBLOCK`/`HAVE_POLL`/`HAVE_SELECT` (among others) with
`check_c_source_compiles()`/`check_function_exists()`, which compile
*and link* a throwaway test program using the project's own
`CMAKE_C_FLAGS` — including this port's `-nostdlib`. Without a `_start`
or a libc to satisfy it, every one of those probes failed to link and
was recorded as unavailable, regardless of whether musl actually
provides the symbol (it does). With `HAVE_O_NONBLOCK` undefined,
libssh2's `session_nonblock()` (`session.c`) falls back to a no-op:
the fd libssh2 believes it switched to non-blocking silently stays
blocking, so its own drain-until-`EAGAIN` loops (e.g.
`_libssh2_channel_read`'s `do { rc = _libssh2_transport_read(session);
} while (rc > 0);`) block on a real, indefinite `recv()` instead of the
prompt `EAGAIN` they are written to expect — a channel read that had
already-buffered data sitting in `session->packets` would still hang,
because the code never got back around to returning it. Fixed in
`neoos-libssh2/build.sh` by passing musl's `crt1.o` and `libc.a` as
`CMAKE_REQUIRED_LIBRARIES`, so the feature probes link against what a
real NeoOS binary actually links against and observe the truth.

**Found by:** kernel-level `stream_send`/`stream_recv`/`tcp_input`
serial tracing (temporary, removed once each cause was confirmed,
matching this session's established practice) proving byte-for-byte
that every TCP segment sent and received matched the host-side proxy's
observed traffic in both directions — which ruled out the transport
layer for bug 1 and pointed straight at the dispatch-less `sendto`
itself; then, after fixing bug 1, a libssh2 debug-trace build
(`ENABLE_DEBUG_LOGGING=ON`, `libssh2_trace(session, ~0)`) showing
`_libssh2_channel_read` successfully parsing and buffering the SSH
channel-data packet (`Conn: increasing read_avail by 18 bytes to
18/2097152`) and then hanging anyway on the *next* transport read — a
symptom that only made sense once `libssh2_config.h`'s `HAVE_*` block
was actually read, which led to bug 3, whose fix then exposed bug 2
(the very first genuinely non-blocking `poll()`-based wait NeoOS had
ever been asked to satisfy).

**Verified:** a real, independent `asyncssh` server (a second SSH
implementation, not NeoOS's own code on both ends) — full handshake,
password auth, channel open, `exec` with a real command-output
`channel_read()`, and a full SFTP file write/read/verify round trip,
all against the real cross-compiled `neoos-libssh2` binary running
under QEMU with a genuine guest-to-host TCP path (no loopback). The
15/15 gauntlet stays green with zero retries — bug 2's fix changes
`poll()`/`select()` wakeup behavior for every stream socket, so this
was the one fix in this refresh with real regression surface, and nothing else exercising it regressed.

**What a real ported application still hits:** libssh2 covers the SSH
transport and SFTP subsystem only — no interactive terminal/PTY
allocation path has been exercised, and NeoOS still has no SSH
*server*; both remain for a later milestone. TLS (OpenSSL) and SSH
(libssh2) are now both proven, which is what curl's `https://` and
`sftp://` support need next.
