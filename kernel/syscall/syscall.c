#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "arch/gdt.h"
#include "arch/msr.h"
#include "dev/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "errno.h"
#include "sync/lock.h"
#include "ipc/signal.h"
#include "dev/timer.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "ipc/futex.h"
#include "fs/file.h"
#include "ipc/pipe.h"
#include "net/socket.h"



// LSTAR as each CPU actually read it back after programming itself.
// Written once per CPU during bringup, read only by the selftest.
static uint64_t syscall_msr_seen[MAX_CPUS];

#include "syscall/syscall_nr.h"

// Mirrors lib/include/fcntl.h's O_* values exactly -- the two trees
// don't share headers, so these must be kept in sync by hand.

// Mirrors lib/include/unistd.h's SEEK_* values exactly.
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern void syscall_entry(void); // syscall_entry.asm


// The filesystem lock now lives in kernel/fs/vfs.c, because
// kernel/file.c's vnode operations take it too and a pipe's must not.

// Every fd access in this file goes through these three, so the
// backing store is named in exactly one place.

struct file_descriptor *fd_get(struct process *p, int fd) {
    if (!p) { return 0; }
    return fd_table_get(p->fd_table, fd);
}

int fd_alloc(struct process *p) {
    if (!p) return -EMFILE;
    return fd_table_alloc(p->fd_table);
}

int fd_close(struct process *p, int fd) {
    if (!p || fd < 0) return -EBADF;
    if (!fd_table_get(p->fd_table, fd)) return -EBADF;
    fd_table_close(p->fd_table, fd);
    return 0;
}

// Copies up to out_size-1 bytes from a user-supplied (pointer, len)
// pair into a NUL-terminated kernel buffer. Shared by every syscall
// that takes a path (SPAWN/OPEN/MKDIR/UNLINK).
// Bounded copy of a NUL-terminated user string. Used only by
// SYS_MOUNT: every other path-taking syscall passes an explicit
// (pointer, length) pair via copy_user_path, but mount needs three
// strings and syscall_dispatch has only four argument slots
// (a1-a4, from rdi/rsi/rdx/r10 in syscall_entry.asm). Widening the
// syscall ABI to six arguments for one call was rejected.
void copy_user_string(int64_t user_ptr, char *out, uint64_t out_size) {
    const char *s = (const char *)(uintptr_t)user_ptr;
    uint64_t i = 0;
    while (i < out_size - 1 && s[i]) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

void copy_user_path(int64_t user_ptr, int64_t user_len, char *out, uint64_t out_size) {
    uint64_t len = (uint64_t)user_len;
    if (len > out_size - 1) {
        len = out_size - 1;
    }
    const char *user_path = (const char *)(uintptr_t)user_ptr;
    for (uint64_t i = 0; i < len; i++) {
        out[i] = user_path[i];
    }
    out[len] = '\0';
}

// Copies a user path and resolves it against the CALLER'S cwd, so that
// every path-taking syscall accepts a relative path identically. `out`
// must be VFS_MAX_PATH bytes: the joined result is longer than the raw
// argument, which is why callers no longer resolve out of the small
// stack buffer they read into.
int copy_user_path_at(int64_t user_ptr, int64_t user_len, char *out) {
    char raw[VFS_MAX_PATH];
    copy_user_path(user_ptr, user_len, raw, sizeof(raw));
    struct process *task = current_proc();
    return vfs_path_canonicalise(task ? task->cwd : "/", raw, out);
}


// ---------------------------------------------------------- dispatch
//
// One function per syscall, reached through a table indexed by the
// syscall number. The switch this replaces had grown to four hundred
// lines in a single function: every case shared one scope, so a `char
// path_buf[64]` in one arm sat on the stack of every other arm, and
// adding a call meant editing the middle of a function nothing could
// be said about as a whole. A table also makes the syscall set
// enumerable -- the integrity selftest below, and eventually a trace
// facility, need exactly that.
//
























// Linux's command numbers.


























// x86-64 arch_prctl. Linux's code values, unchanged.
#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004
#define MSR_FS_BASE 0xC0000100









// The table. Designated initialisers, so the number beside each entry
// IS the index -- a reordering of these lines cannot renumber a
// syscall, which a positional array would allow. Entries omitted here
// are null and dispatch as -ENOSYS.
//
// The name is not decoration: syscall_table_selftest reports by it, and
// an unimplemented number is far easier to diagnose from a log line
// naming the neighbours than from a bare number.
struct syscall_desc {
    syscall_handler fn;
    const char     *name;
};

static const struct syscall_desc syscall_table[SYS_MAX] = {
    [SYS_EXIT]            = { sys_exit,            "exit" },
    [SYS_WRITE]           = { sys_write,           "write" },
    [SYS_YIELD]           = { sys_yield,           "yield" },
    [SYS_GETPID]          = { sys_getpid,          "getpid" },
    [SYS_SPAWN]           = { sys_spawn,           "spawn" },
    [SYS_WAIT]            = { sys_wait,            "wait" },
    [SYS_READ]            = { sys_read,            "read" },
    [SYS_OPEN]            = { sys_open,            "open" },
    [SYS_CLOSE]           = { sys_close,           "close" },
    [SYS_MKDIR]           = { sys_mkdir,           "mkdir" },
    [SYS_UNLINK]          = { sys_unlink,          "unlink" },
    [SYS_LSEEK]           = { sys_lseek,           "lseek" },
    [SYS_FORK]            = { sys_fork,            "fork" },
    [SYS_EXEC]            = { sys_exec,            "exec" },
    [SYS_MOUNT]           = { sys_mount,           "mount" },
    [SYS_UMOUNT]          = { sys_umount,          "umount" },
    [SYS_GETDENTS]        = { sys_getdents,        "getdents" },
    [SYS_THREAD_CREATE]   = { sys_thread_create,   "thread_create" },
    [SYS_THREAD_EXIT]     = { sys_thread_exit,     "thread_exit" },
    [SYS_THREAD_JOIN]     = { sys_thread_join,     "thread_join" },
    [SYS_THREAD_SELF]     = { sys_thread_self,     "thread_self" },
    [SYS_RT_SIGACTION]    = { sys_rt_sigaction,    "rt_sigaction" },
    [SYS_RT_SIGPROCMASK]  = { sys_rt_sigprocmask,  "rt_sigprocmask" },
    [SYS_RT_SIGRETURN]    = { sys_rt_sigreturn,    "rt_sigreturn" },
    [SYS_RT_SIGPENDING]   = { sys_rt_sigpending,   "rt_sigpending" },
    [SYS_RT_SIGSUSPEND]   = { sys_rt_sigsuspend,   "rt_sigsuspend" },
    [SYS_RT_SIGTIMEDWAIT] = { sys_rt_sigtimedwait, "rt_sigtimedwait" },
    [SYS_RT_SIGQUEUEINFO] = { sys_rt_sigqueueinfo, "rt_sigqueueinfo" },
    [SYS_SIGALTSTACK]     = { sys_sigaltstack,     "sigaltstack" },
    [SYS_KILL]            = { sys_kill,            "kill" },
    [SYS_TKILL]           = { sys_tkill,           "tkill" },
    [SYS_TGKILL]          = { sys_tgkill,          "tgkill" },
    [SYS_WAIT4]           = { sys_wait4,           "wait4" },
    [SYS_SETPGID]         = { sys_setpgid,         "setpgid" },
    [SYS_GETPGID]         = { sys_getpgid,         "getpgid" },
    [SYS_SETSID]          = { sys_setsid,          "setsid" },
    [SYS_GETSID]          = { sys_getsid,          "getsid" },
    [SYS_MMAP]            = { sys_mmap,            "mmap" },
    [SYS_MUNMAP]          = { sys_munmap,          "munmap" },
    [SYS_MPROTECT]        = { sys_mprotect,        "mprotect" },
    [SYS_CPU_COUNT]       = { sys_cpu_count,       "cpu_count" },
    [SYS_GETCPU]          = { sys_getcpu,          "getcpu" },
    [SYS_FUTEX]           = { sys_futex,           "futex" },
    [SYS_PIPE2]           = { sys_pipe2,           "pipe2" },
    [SYS_ARCH_PRCTL]      = { sys_arch_prctl,      "arch_prctl" },
    [SYS_SOCKET]          = { sys_socket,          "socket" },
    [SYS_BIND]            = { sys_bind,            "bind" },
    [SYS_CONNECT]         = { sys_connect,         "connect" },
    [SYS_SENDTO]          = { sys_sendto,          "sendto" },
    [SYS_RECVFROM]        = { sys_recvfrom,        "recvfrom" },
    [SYS_GETSOCKNAME]     = { sys_getsockname,     "getsockname" },
    [SYS_SPAWNV]          = { sys_spawnv,          "spawnv" },
    [SYS_FCNTL]           = { sys_fcntl,           "fcntl" },
    [SYS_CHDIR]           = { sys_chdir,           "chdir" },
    [SYS_GETCWD]          = { sys_getcwd,          "getcwd" },
    [SYS_STAT]            = { sys_stat,            "stat" },
    [SYS_LSTAT]           = { sys_lstat,           "lstat" },
    [SYS_FSTAT]           = { sys_fstat,           "fstat" },
    [SYS_NEWFSTATAT]      = { sys_newfstatat,      "newfstatat" },
    [SYS_SET_TID_ADDRESS] = { sys_set_tid_address, "set_tid_address" },
    [SYS_EXIT_GROUP]      = { sys_exit_group,      "exit_group" },
    [SYS_WRITEV]          = { sys_writev,          "writev" },
    [SYS_READV]           = { sys_readv,           "readv" },
    [SYS_IOCTL]           = { sys_ioctl,           "ioctl" },
    [SYS_CLOCK_GETTIME]   = { sys_clock_gettime,   "clock_gettime" },
    [SYS_NANOSLEEP]       = { sys_nanosleep,       "nanosleep" },
};

// Asserts what the table's shape is supposed to guarantee. Cheap, and
// it catches the one mistake designated initialisers still allow:
// SYS_MAX left behind when a syscall is added, which truncates the
// table silently and turns the new call into -ENOSYS at run time.
void syscall_table_selftest(void) {
    int implemented = 0;
    for (int i = 0; i < SYS_MAX; i++) {
        if (syscall_table[i].fn && !syscall_table[i].name) {
            serial_write_string("[syscall] table selftest FAILED: unnamed handler at ");
            serial_write_hex64((uint64_t)i);
            serial_write_string("\n");
            return;
        }
        if (syscall_table[i].fn) { implemented++; }
    }
    // The highest number in use must be the last slot. A gap at the end
    // means SYS_MAX is larger than the table's contents (harmless), but
    // a MISSING last slot means SYS_MAX is too small and the syscall
    // that should live there was silently dropped.
    if (!syscall_table[SYS_MAX - 1].fn) {
        serial_write_string("[syscall] table selftest FAILED: SYS_MAX overshoots the last handler\n");
        return;
    }
    serial_write_string("[syscall] table selftest passed, implemented=");
    serial_write_hex64((uint64_t)implemented);
    serial_write_string(" of ");
    serial_write_hex64((uint64_t)SYS_MAX);
    serial_write_string("\n");
}

// Called only from syscall_entry.asm's `call syscall_dispatch`, via
// syscall_dispatch below.
static int64_t syscall_dispatch_inner(int64_t num, struct syscall_args *args) {
    // One unsigned compare covers both ends: a negative number becomes
    // enormous and fails the same test.
    if ((uint64_t)num >= SYS_MAX || !syscall_table[num].fn) {
        // -ENOSYS, not -1. Linux's answer for an unimplemented call,
        // and musl's feature probes depend on being able to tell "this
        // kernel lacks the call" from "the call failed".
        return -ENOSYS;
    }
    return syscall_table[num].fn(args);
}

// Single exit point for every syscall. A thread killed by a sibling's
// exit() unwinds to here: whatever it was doing has finished, and it
// must not return to user mode.
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3,
                         int64_t a4, struct syscall_frame *frame) {
    struct syscall_args args = { a1, a2, a3, a4, frame };
    int64_t ret = syscall_dispatch_inner(num, &args);
    return signal_deliver_from_syscall(frame, num, ret);
}

void syscall_init(void) {
    syscall_init_this_cpu();
    serial_write_string("[syscall] SYSCALL/SYSRET configured\n");
}

// EFER, STAR, LSTAR and SFMASK are all PER-CPU MSRs -- writing them on
// the BSP configures the BSP and nothing else. Every CPU that may ever
// return to ring 3 has to program them for itself, so ap_main calls
// this too. A user thread that reaches an AP whose MSRs were never
// written SYSCALLs into an unconfigured LSTAR; observed as every
// userland suite producing no output at all, with no exception logged.
//
// Reachable without work stealing: enqueue_ready() targets this_cpu(),
// so a user thread woken by a kernel thread running on an AP is queued
// -- and then run -- there.
void syscall_init_this_cpu(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    // EFER_NXE: elf_load (Task 5) is the first code in NeoOS to
    // actually set PAGE_NO_EXECUTE (bit 63) on a real PTE -- without
    // NXE enabled, that bit is reserved and setting it faults the
    // moment the page is walked. Grouped here since it's the same MSR
    // as EFER_SCE, not because it's conceptually part of SYSCALL setup.
    wrmsr(MSR_EFER, efer | EFER_SCE | EFER_NXE);

    // STAR[47:32] = kernel CS (kernel SS = that + 8, matches
    // GDT_KERNEL_DATA_SELECTOR at 0x10); STAR[63:48] = the SYSRET
    // base (user data at that+8, user code64 at that+16 -- see
    // Task 1's GDT layout).
    uint64_t star = ((uint64_t)GDT_USER_CODE32_SELECTOR << 48) | ((uint64_t)GDT_KERNEL_CODE_SELECTOR << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200); // mask IF (bit 9) on syscall entry

    // Read BACK, so the selftest below asserts what the hardware holds
    // rather than what we believe we wrote.
    syscall_msr_seen[this_cpu() - &cpus[0]] = rdmsr(MSR_LSTAR);
}

// Asserts the invariant the split above exists to maintain: every
// online CPU has its own LSTAR pointing at syscall_entry. Silent
// breakage here shows up as a userland suite that emits nothing at all
// and logs no exception, which is close to undiagnosable from the
// serial log -- hence a direct check.
void syscall_msr_selftest(void) {
    int online = smp_online_count();
    for (int i = 0; i < online; i++) {
        if (syscall_msr_seen[i] != (uint64_t)(uintptr_t)syscall_entry) {
            serial_write_string("[syscall] msr selftest FAILED: cpu=");
            serial_write_hex64((uint64_t)i);
            serial_write_string(" lstar=");
            serial_write_hex64(syscall_msr_seen[i]);
            serial_write_string("\n");
            return;
        }
    }
    serial_write_string("[syscall] per-cpu msr selftest passed, cpus=");
    serial_write_hex64((uint64_t)online);
    serial_write_string("\n");
}
