#ifndef NEOOS_SYSCALL_NR_H
#define NEOOS_SYSCALL_NR_H

// NeoOS's own syscall numbering.
//
// These numbers are INTERNAL and deliberately unlike Linux's: per
// /CLAUDE.md, the numbers stay ours and the musl shim translates them.
// What must not diverge is what sits behind them -- struct layouts,
// flag values, semantics. So a number here may be renumbered freely;
// the SHAPE of the call may not.
//
// The numbers are dense and assigned in the order features landed, with
// two historical exceptions (30/31 and 33-36 were filled in after 32
// and 37-39). Dispatch is a table indexed by this number, so a gap
// costs one null pointer and nothing else -- but SYS_MAX must always be
// one past the highest number in use, or the table silently truncates.
//
// Keep in sync with docs/abi-compatibility.md's syscall inventory.

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_YIELD           2
#define SYS_GETPID          3
#define SYS_SPAWN           4
#define SYS_WAIT            5
#define SYS_READ            6
#define SYS_OPEN            7
#define SYS_CLOSE           8
#define SYS_MKDIR           9
#define SYS_UNLINK          10
#define SYS_LSEEK           11
#define SYS_FORK            12
#define SYS_EXEC            13
#define SYS_MOUNT           14
#define SYS_UMOUNT          15
#define SYS_GETDENTS        16
#define SYS_THREAD_CREATE   17
#define SYS_THREAD_EXIT     18
#define SYS_THREAD_JOIN     19
#define SYS_THREAD_SELF     20
#define SYS_RT_SIGACTION    21
#define SYS_RT_SIGPROCMASK  22
#define SYS_RT_SIGRETURN    23
#define SYS_RT_SIGPENDING   24
#define SYS_RT_SIGSUSPEND   25
#define SYS_RT_SIGTIMEDWAIT 26
#define SYS_RT_SIGQUEUEINFO 27
#define SYS_SIGALTSTACK     28
#define SYS_KILL            29
#define SYS_TKILL           30
#define SYS_TGKILL          31
#define SYS_WAIT4           32
#define SYS_SETPGID         33
#define SYS_GETPGID         34
#define SYS_SETSID          35
#define SYS_GETSID          36
#define SYS_MMAP            37
#define SYS_MUNMAP          38
#define SYS_MPROTECT        39
// SMP visibility. Linux exposes these through sysconf(3) and
// sched_getcpu(3), which are library calls over sysfs/vDSO rather than
// syscalls of their own -- NeoOS gives them real syscall numbers, and
// the library presents the POSIX shapes on top. See docs/stdlib.md.
#define SYS_CPU_COUNT       40
#define SYS_GETCPU          41
// Linux's futex, with Linux's operation numbers and semantics. Every
// higher-level POSIX synchronisation primitive is built on it, so it is
// the one that had to be Linux-shaped down to the last return value.
#define SYS_FUTEX           42
// POSIX pipe2. Linux's pipe(2) is the same call with flags == 0, so
// only the two-argument form exists here and the library supplies
// pipe() on top -- exactly how musl does it on architectures where
// Linux dropped the legacy call.
#define SYS_PIPE2           43
// arch_prctl(ARCH_SET_FS/ARCH_GET_FS). Linux's codes and semantics; it
// is how every x86-64 libc installs the thread pointer.
#define SYS_ARCH_PRCTL      44
// BSD sockets. Linux gives each its own number on x86-64 (rather than
// the socketcall multiplexer it uses on i386), and so does NeoOS.
#define SYS_SOCKET          45
#define SYS_BIND            46
#define SYS_CONNECT         47
#define SYS_SENDTO          48
#define SYS_RECVFROM        49
#define SYS_GETSOCKNAME     50
// spawn with an argument vector. spawn (4) stays, taking only a path;
// this is what a launcher needs, and what execve's argv will become.
#define SYS_SPAWNV          51
// fcntl, F_GETFL/F_SETFL only. Enough to turn O_NONBLOCK on and off,
// which is what a program needs to poll a pipe or a socket without a
// select() to wait on.
#define SYS_FCNTL           52

// Per-process current working directory. Every path-taking syscall
// resolves through it, so a relative path finally means something.
#define SYS_CHDIR           53
#define SYS_GETCWD          54

// The stat family, with Linux's `struct stat` behind it. musl reaches
// for the plain forms on x86-64 whenever the path is absolute or the
// directory is AT_FDCWD, and only falls back to newfstatat for a path
// relative to a real directory fd -- so all four exist, but newfstatat
// accepts AT_FDCWD alone until there is an openat family to produce a
// real dirfd. See docs/porting-coreutils.md.
#define SYS_STAT            55
#define SYS_LSTAT           56
#define SYS_FSTAT           57
#define SYS_NEWFSTATAT      58

// The calls musl makes on its way to main, and the two stdio depends
// on. See docs/porting-coreutils.md's Tier 0.
#define SYS_SET_TID_ADDRESS 59
#define SYS_EXIT_GROUP      60
#define SYS_WRITEV          61
#define SYS_READV           62
#define SYS_IOCTL           63
#define SYS_CLOCK_GETTIME   64
#define SYS_NANOSLEEP       65

// Test-only syscall for deterministic key injection in headless tests.
// Compiled only under -DNEOOS_TEST_HOOKS; returns -ENOSYS in production.
#define SYS_TEST_HOOK       66

#define SYS_POLL            67
#define SYS_SELECT          68

// reboot(2). PID-1 only (NeoOS has no uids/capabilities -- see
// docs/stdlib.md). POWER_OFF / HALT / RESTART command words match
// Linux's magic-2 values.
#define SYS_REBOOT          69

// dup / dup2 / dup3. NeoOS never reallocates fds 0/1/2 through open(),
// so these are the only way a process rebinds its own standard streams
// (a shell doing redirection, a terminal wiring a child to a pty).
#define SYS_DUP             70
#define SYS_DUP2            71
#define SYS_DUP3            72

// One past the highest number in use. The dispatch table is this long.
#define SYS_MAX             73

#endif
