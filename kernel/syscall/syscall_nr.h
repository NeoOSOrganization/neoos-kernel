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

// BB2, measured rather than predicted: these are the three numbers
// BusyBox actually reached for that NeoOS did not have (see
// docs/superpowers/plans/2026-09-01-busybox-track.md).
#define SYS_GETPPID         73
#define SYS_UNAME           74
#define SYS_BRK             75

// BB6, measured the same way: BusyBox's interactive shell asks for
// these four at startup. NeoOS is single-user with no notion of
// credentials, so all four answer 0 -- which is root, and is the honest
// answer rather than a placeholder. They are KERNEL calls rather than
// shim constants because the shim translates and never emulates.
#define SYS_GETUID          76
#define SYS_GETEUID         77
#define SYS_GETGID          78
#define SYS_GETEGID         79

// N3. Only uid 0 (god) may change identity, and only downward -- login
// is the one caller.
#define SYS_SETUID          80
#define SYS_SETGID          81

// Random bytes. The same CSPRNG /dev/urandom reads from, reachable
// without a file descriptor -- which is what musl's getentropy() and
// arc4random() use, and what a program needs before it can open
// anything.
#define SYS_GETRANDOM       82

// D5. TCP's socket surface. Linux gives each its own number on x86-64
// and so does NeoOS; the shim maps musl's numbers onto these.
#define SYS_LISTEN          83
#define SYS_ACCEPT4         84
#define SYS_SHUTDOWN        85
#define SYS_GETPEERNAME     86
#define SYS_SETSOCKOPT      87
#define SYS_GETSOCKOPT      88

// The generalized scatter/gather form of sendto/recvfrom: an iovec
// array instead of one buffer, found missing when musl's DNS resolver
// (res_msend.c) turned out to call recvmsg() to read a UDP reply and
// got -ENOSYS from the shim every time -- see docs/abi-compatibility.md's
// DNS resolution refresh. UDP (SOCK_DGRAM) only, matching sendto/
// recvfrom's own existing scope: both are implemented by gathering/
// scattering through those two entry points, which only ever handle
// a socket's dgram queue.
#define SYS_SENDMSG         89
#define SYS_RECVMSG         90

// AF_UNIX only, backed by two of kernel/ipc/pipe.c's ring buffers --
// see kernel/ipc/socketpair.c and docs/abi-compatibility.md's
// socketpair refresh. Added for curl's multi-handle wakeup mechanism,
// which musl's shim otherwise has no way to satisfy at all.
#define SYS_SOCKETPAIR      91

// clone(flags, child_stack, ptid, ctid, tls) -- raw Linux argument
// order and register convention. Accepts EXACTLY musl's own
// pthread_create flag combination (see sys_clone's own comment);
// anything else is -EINVAL. See docs/superpowers/plans/
// 2026-09-07-clone-pthread.md and docs/stdlib.md.
#define SYS_CLONE           92

// sched_getaffinity(pid, cpusetsize, mask) -- Linux shape. Found
// missing via dotnet NativeAOT's CoreCLR startup (PalGetCurrentThread
// affinity query during RhInitialize). NeoOS reports every online CPU
// as affine to every thread: there is no CPU-affinity/cpuset concept
// to restrict it against. See docs/stdlib.md.
#define SYS_SCHED_GETAFFINITY 93

// membarrier(cmd, flags) -- Linux's command bitmask (see
// kernel/smp/membarrier.h), backed by a real cross-CPU IPI broadcast
// (kernel/smp/membarrier.c), not a lie: NeoOS is SMP-capable, and a
// single-core box just has nothing to IPI. Also found missing via
// CoreCLR's startup. See docs/stdlib.md.
#define SYS_MEMBARRIER        94

// mlock(addr, len) -- accepted and a genuine no-op success rather than
// -ENOSYS: NeoOS has no swap and never pages out anonymous memory, so
// every resident page is already exactly what mlock(2) asks for. Real
// Linux's mlock() can still fail (ENOMEM against RLIMIT_MEMLOCK,
// EFAULT on a bad range) -- NeoOS has no memlock rlimit to enforce, so
// the only check kept is that the range is a real, mapped part of the
// caller's address space. Also found missing via CoreCLR's startup
// (GC card table / write-barrier metadata pinning). See docs/stdlib.md.
#define SYS_MLOCK             95

// sysinfo(struct sysinfo *) -- Linux shape, real numbers: totalram/
// freeram come from kernel/mm/pmm.h's frame counters, not fabricated
// ones. Found missing one syscall deeper into CoreCLR's startup, once
// sched_getaffinity/membarrier/mlock stopped blocking it -- the GC's
// own heap-sizing logic needs real total/free memory to size against.
// See docs/stdlib.md.
#define SYS_SYSINFO           96

// statfs(path, struct statfs *) -- Linux shape. NeoOS reports a
// generic, unnamed filesystem (f_type 0: no magic number this
// implementation claims to match) with real block counts from
// kernel/mm/pmm.h -- NeoOS's filesystems (FAT, ramfs, devfs, procfs)
// have no unified free-space concept of their own to report instead.
// Found alongside sysinfo/get_mempolicy. See docs/stdlib.md.
#define SYS_STATFS            97

// get_mempolicy(mode, nodemask, maxnode, addr, flags) -- Linux shape.
// NeoOS has exactly one NUMA node, always: MPOL_DEFAULT, node 0. Found
// alongside sysinfo/statfs. See docs/stdlib.md.
#define SYS_GET_MEMPOLICY     98

// madvise(addr, len, advice) -- Linux shape, and a genuine no-op
// success for every advice value: it is purely advisory on Linux too
// (a conforming kernel may ignore any of it), and NeoOS has nothing to
// act on -- no swap to make MADV_DONTNEED/MADV_FREE meaningful, no
// speculative readahead to steer with MADV_WILLNEED/MADV_SEQUENTIAL/
// MADV_RANDOM. The one check kept is the one real Linux would also
// make: the range must actually be mapped (ENOMEM otherwise). Found
// hanging dotnet NativeAOT's CoreCLR GC (which retries indefinitely on
// -ENOSYS rather than treating it as fatal, unlike the syscalls found
// immediately before it -- the hang, not a crash or clean exit, was
// the tell). See docs/stdlib.md.
#define SYS_MADVISE           99

// epoll_create1(flags) / epoll_ctl(epfd, op, fd, event) /
// epoll_wait(epfd, events, maxevents, timeout) / epoll_pwait(...,
// sigmask) -- Linux shape, built on the same poll_core() scan-and-
// sleep loop poll()/select() already use (kernel/sync/epoll.c/.h).
// Found missing chasing a dotnet NativeAOT TCP socket example: .NET's
// SocketAsyncEngine uses epoll even for a single synchronous
// connect+send+recv. Level-triggered only -- see epoll.h's own
// comment. epoll_pwait's sigmask argument is accepted and not applied
// (see docs/stdlib.md).
#define SYS_EPOLL_CREATE1    100
#define SYS_EPOLL_CTL        101
#define SYS_EPOLL_WAIT       102
#define SYS_EPOLL_PWAIT      103

// readlink(path, buf, bufsize) -- Linux shape (path,len,buf,bufsize
// order, matching stat's own shim marshaling). Always -EINVAL past a
// resolvable path -- see kernel/syscall/sys_file.c's own comment on
// sys_readlink for why (no filesystem NeoOS mounts can represent a
// symlink, the same divergence lstat already has). Found missing
// getting a real ASP.NET Core app running.
#define SYS_READLINK         104

// inotify_init1(flags) / inotify_add_watch(fd, path, mask) /
// inotify_rm_watch(fd, wd) -- Linux shape, a real but inert fd (see
// kernel/sync/inotify.h/.c). Found missing (fatal, unlike readlink
// just above) getting a real ASP.NET Core app running: the generic
// host's configuration system creates one unconditionally to watch
// appsettings.json.
#define SYS_INOTIFY_INIT1       105
#define SYS_INOTIFY_ADD_WATCH   106
#define SYS_INOTIFY_RM_WATCH    107

// getrusage(who, usage) -- Linux shape, every field honestly zero
// (see kernel/syscall/sys_misc.c's own comment on sys_getrusage for
// why). Found missing (fatal) getting a real ASP.NET Core app
// running.
#define SYS_GETRUSAGE           108

// mremap(old_addr, old_size, new_size, flags) -- deliberately narrow,
// see kernel/mm/vma.c's vma_mremap_locked for the real semantics.
// Found missing (and confirmed the direct cause of a real .NET GC
// crash, not a tidiness gap) via musl's pthread_getattr_np() probing
// the main thread's own stack size. See docs/stdlib.md.
#define SYS_MREMAP              109

// Scheduler ABI (SCH-1 Task 5). The fair-class EEVDF core made real
// nice/policy/slice control meaningful; these expose it. RT policies
// (SCHED_FIFO/RR/DEADLINE) return -EINVAL until SCH-3/SCH-4. See
// docs/stdlib.md for the divergences. sched_yield already had a number
// (SYS_YIELD 2); it is now a real EEVDF yield.
#define SYS_NICE                     110
#define SYS_SETPRIORITY             111
#define SYS_GETPRIORITY            112
#define SYS_SCHED_SETSCHEDULER      113
#define SYS_SCHED_GETSCHEDULER      114
#define SYS_SCHED_SETPARAM         115
#define SYS_SCHED_GETPARAM        116
#define SYS_SCHED_GET_PRIORITY_MAX  117
#define SYS_SCHED_GET_PRIORITY_MIN  118
#define SYS_SCHED_RR_GET_INTERVAL   119
#define SYS_SCHED_SETATTR          120
#define SYS_SCHED_GETATTR         121
#define SYS_SCHED_SETAFFINITY      122

// MSC-1. eventfd2(initval, flags) -- a 64-bit counter behind an fd, the
// wake primitive for libuv/.NET/tokio event loops
// (kernel/ipc/eventfd.c). prctl(option, ...) -- the no-op-safe subset
// (PR_SET_NAME/GET_NAME touch comm; the rest are accepted-inert).
#define SYS_EVENTFD2               123
#define SYS_PRCTL                 124

// One past the highest number in use. The dispatch table is this long.
#define SYS_MAX              125

#endif
