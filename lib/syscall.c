#include "unistd.h"
#include "string.h"
#include "fcntl.h"
#include "dirent.h"
#include "signal.h"
#include "sys/wait.h"
#include "futex.h"
#include "tls.h"
#include "sys/mman.h"
#include "sys/stat.h"
#include "sys/uio.h"
#include "termios.h"
#include "time.h"

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_SPAWN  4
#define SYS_WAIT   5
#define SYS_READ   6
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_MKDIR  9
#define SYS_UNLINK 10
#define SYS_LSEEK  11
#define SYS_FORK   12
#define SYS_EXEC   13
#define SYS_MOUNT  14
#define SYS_UMOUNT 15
#define SYS_GETDENTS 16
#define SYS_THREAD_CREATE 17
#define SYS_THREAD_EXIT   18
#define SYS_THREAD_JOIN   19
#define SYS_THREAD_SELF   20
#define SYS_RT_SIGACTION  21
#define SYS_RT_SIGPROCMASK 22
#define SYS_CPU_COUNT      40
#define SYS_GETCPU         41
#define SYS_RT_SIGPENDING 24
#define SYS_RT_SIGSUSPEND 25
#define SYS_SIGALTSTACK   28
#define SYS_MMAP          37
#define SYS_MUNMAP        38
#define SYS_MPROTECT      39
#define SYS_KILL          29
#define SYS_WAIT4         32
#define SYS_SETPGID       33
#define SYS_GETPGID       34
#define SYS_SETSID        35
#define SYS_GETSID        36
#define SYS_TKILL         30
#define SYS_TGKILL        31
#define SYS_FUTEX         42
#define SYS_PIPE2         43
#define SYS_ARCH_PRCTL    44
#define SYS_SPAWNV        51
#define SYS_FCNTL         52
#define SYS_DUP           70
#define SYS_DUP2          71
#define SYS_DUP3          72

#define SYS_CHDIR         53
#define SYS_GETCWD        54
#define SYS_STAT          55
#define SYS_LSTAT         56
#define SYS_FSTAT         57
#define SYS_NEWFSTATAT    58
#define SYS_SET_TID_ADDRESS 59
#define SYS_EXIT_GROUP    60
#define SYS_WRITEV        61
#define SYS_READV         62
#define SYS_IOCTL         63
#define SYS_CLOCK_GETTIME 64
#define SYS_NANOSLEEP     65
#define SYS_TEST_HOOK     66

// Test-hook request codes
#define TESTHOOK_INJECT_KEY   1
#define TESTHOOK_MIG_COUNT    2
#define TESTHOOK_PARENT_PID   3
#define TESTHOOK_PMM_FREE     4
#define TESTHOOK_POLL_STATS   5
#define TESTHOOK_POLL_DEPTH   6

static inline int64_t syscall0(int64_t num) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(int64_t num, int64_t a1) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall2(int64_t num, int64_t a1, int64_t a2) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(int64_t num, int64_t a1, int64_t a2, int64_t a3) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

// The fourth argument goes in R10, not RCX: the `syscall` instruction
// overwrites RCX with the return address. That is the SysV kernel
// calling convention, and syscall_entry.asm expects it.
static inline int64_t syscall4(int64_t num, int64_t a1, int64_t a2,
                               int64_t a3, int64_t a4) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                      : "rcx", "r11", "memory");
    return ret;
}

// Six arguments: the fifth and sixth go in r8 and r9, which is where
// syscall_entry.asm expects them (mmap, sendto and recvfrom are the
// only calls that need them). Exported unmangled rather than static
// inline because lib/socket.c is a separate translation unit.
long __neoos_syscall6(long n, long a, long b, long c, long d, long e, long f) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

long __neoos_syscall3(long n, long a, long b, long c) {
    return (long)syscall3(n, a, b, c);
}

void exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

int64_t write(int fd, const void *buf, uint64_t len) {
    return syscall3(SYS_WRITE, fd, (int64_t)(uint64_t)buf, (int64_t)len);
}

int64_t read(int fd, void *buf, uint64_t len) {
    return syscall3(SYS_READ, fd, (int64_t)(uint64_t)buf, (int64_t)len);
}

int open(const char *path, int flags) {
    uint64_t len = strlen(path);
    return (int)syscall3(SYS_OPEN, (int64_t)(uint64_t)path, (int64_t)len, flags);
}

int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

int64_t lseek(int fd, int64_t offset, int whence) {
    return syscall3(SYS_LSEEK, fd, offset, whence);
}

int getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

// POSIX shapes over the NeoOS syscalls. Linux implements both in libc
// (sysfs and the vDSO); NeoOS answers them from the kernel directly.
// The NUMA node sched_getcpu's Linux cousin getcpu(2) can report is
// always 0 here -- NeoOS has no NUMA. Recorded in docs/stdlib.md.
long sysconf(int name) {
    switch (name) {
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
        return (long)syscall0(SYS_CPU_COUNT);
    default:
        return -1;
    }
}

int sched_getcpu(void) {
    return (int)syscall0(SYS_GETCPU);
}

void yield(void) {
    syscall0(SYS_YIELD);
}

int spawn(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_SPAWN, (int64_t)(uint64_t)path, (int64_t)len);
}

int spawnv(const char *path, char *const argv[]) {
    uint64_t len = strlen(path);
    return (int)__neoos_syscall3(SYS_SPAWNV, (long)(uintptr_t)path, (long)len,
                                 (long)(uintptr_t)argv);
}

int fork(void) {
    return (int)syscall0(SYS_FORK);
}

int getdents(int fd, void *buf, int bytes) {
    return (int)syscall3(SYS_GETDENTS, fd, (int64_t)(uintptr_t)buf, bytes);
}

int mount(const char *source, const char *target, const char *fstype) {
    // Three NUL-terminated pointers, no lengths -- see the note on
    // copy_user_string in kernel/syscall.c for why mount differs from
    // every other path-taking call here.
    return (int)syscall3(SYS_MOUNT, (int64_t)(uint64_t)source,
                          (int64_t)(uint64_t)target, (int64_t)(uint64_t)fstype);
}

int umount(const char *target) {
    uint64_t len = strlen(target);
    return (int)syscall2(SYS_UMOUNT, (int64_t)(uint64_t)target, (int64_t)len);
}

int exec(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_EXEC, (int64_t)(uint64_t)path, (int64_t)len);
}

int wait(int pid) {
    return (int)syscall1(SYS_WAIT, pid);
}

int mkdir(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_MKDIR, (int64_t)(uint64_t)path, (int64_t)len);
}

int unlink(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_UNLINK, (int64_t)(uint64_t)path, (int64_t)len);
}

int __sys_thread_create(unsigned long entry, unsigned long arg) {
    return (int)syscall2(SYS_THREAD_CREATE, (int64_t)entry, (int64_t)arg);
}

void __sys_thread_exit(int code) {
    syscall1(SYS_THREAD_EXIT, code);
    __builtin_unreachable();
}

int __sys_thread_join(int tid, int *code) {
    return (int)syscall2(SYS_THREAD_JOIN, tid, (int64_t)(uint64_t)(uintptr_t)code);
}

int __sys_thread_self(void) {
    return (int)syscall0(SYS_THREAD_SELF);
}

struct k_sigaction;
long __sys_rt_sigaction(int sig, const struct k_sigaction *act,
                        struct k_sigaction *old) {
    return (long)syscall3(SYS_RT_SIGACTION, sig,
                          (int64_t)(uint64_t)(uintptr_t)act,
                          (int64_t)(uint64_t)(uintptr_t)old);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    return (int)syscall3(SYS_RT_SIGPROCMASK, how,
                        (int64_t)(uint64_t)(uintptr_t)set,
                        (int64_t)(uint64_t)(uintptr_t)old);
}
int sigpending(sigset_t *set) {
    return (int)syscall1(SYS_RT_SIGPENDING, (int64_t)(uint64_t)(uintptr_t)set);
}
int sigsuspend(const sigset_t *mask) {
    return (int)syscall1(SYS_RT_SIGSUSPEND, (int64_t)(uint64_t)(uintptr_t)mask);
}

int sigaltstack(const stack_t *ss, stack_t *old) {
    return (int)syscall2(SYS_SIGALTSTACK,
                        (int64_t)(uint64_t)(uintptr_t)ss,
                        (int64_t)(uint64_t)(uintptr_t)old);
}

int kill(int pid, int sig)             { return (int)syscall2(SYS_KILL, pid, sig); }
int tkill(int tid, int sig)            { return (int)syscall2(SYS_TKILL, tid, sig); }
int tgkill(int tgid, int tid, int sig) { return (int)syscall3(SYS_TGKILL, tgid, tid, sig); }

int wait4(int pid, int *status, int options, void *rusage) {
    (void)rusage;   // NeoOS keeps no per-process resource accounting
    return (int)syscall3(SYS_WAIT4, pid,
                        (int64_t)(uint64_t)(uintptr_t)status, options);
}
int waitpid(int pid, int *status, int options) { return wait4(pid, status, options, 0); }

int setpgid(int pid, int pgid) { return (int)syscall2(SYS_SETPGID, pid, pgid); }
int getpgid(int pid)           { return (int)syscall1(SYS_GETPGID, pid); }
int setsid(void)               { return (int)syscall0(SYS_SETSID); }
int getsid(int pid)            { return (int)syscall1(SYS_GETSID, pid); }

long mmap_fd_raw(unsigned long addr, unsigned long len, int prot, int flags,
                int fd, long offset) {
    // Raw NeoOS-native wrapper so the mmap path can be tested before
    // musl exists. mmap's 5th/6th args (fd, offset) go in r8/r9, so this
    // uses inline asm rather than the 4-argument helpers.
    long ret;
    register long r10 __asm__("r10") = flags;
    register long r8  __asm__("r8")  = fd;
    register long r9  __asm__("r9")  = offset;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"((long)SYS_MMAP), "D"(addr), "S"(len), "d"(prot),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

long mmap_raw(unsigned long addr, unsigned long len, int prot, int flags) {
    return mmap_fd_raw(addr, len, prot, flags, -1, 0);
}

int munmap_raw(unsigned long addr, unsigned long len) {
    return (int)syscall2(SYS_MUNMAP, (int64_t)addr, (int64_t)len);
}

// POSIX shapes over the raw calls: -1/MAP_FAILED instead of a negative
// errno. This is the one place in the library that translates, and it
// does so because these three names have a return convention every
// caller already knows.
void *mmap(void *addr, uint64_t length, int prot, int flags, int fd, int64_t offset) {
    // Anonymous mappings and device fds that implement mmap (/dev/fb0).
    // A regular-file fd still gets -ENOSYS from the kernel.
    long rc = mmap_fd_raw((unsigned long)(uintptr_t)addr, length, prot, flags,
                          fd, (long)offset);
    if (rc < 0) { return MAP_FAILED; }
    return (void *)(uintptr_t)rc;
}

int munmap(void *addr, uint64_t length) {
    int rc = munmap_raw((unsigned long)(uintptr_t)addr, length);
    return rc < 0 ? -1 : 0;
}

int mprotect(void *addr, uint64_t length, int prot) {
    int rc = (int)syscall3(SYS_MPROTECT, (int64_t)(uint64_t)(uintptr_t)addr,
                           (int64_t)length, prot);
    return rc < 0 ? -1 : 0;
}

// ---------------------------------------------------------------- futex
//
// The raw primitive, exposed so <semaphore.h> and <pthread.h> can be
// built on it -- and so that anything else needing to block on a word
// of memory does not invent its own mechanism. Arguments and return
// values are Linux's, unchanged; see docs/stdlib.md.
long futex(int *uaddr, int op, int val, const struct timespec *timeout) {
    return (long)syscall4(SYS_FUTEX, (int64_t)(uint64_t)(uintptr_t)uaddr,
                          op, val, (int64_t)(uint64_t)(uintptr_t)timeout);
}

int futex_wait(int *uaddr, int expected) {
    long rc = futex(uaddr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, 0);
    return (int)rc;
}

int futex_wait_timeout(int *uaddr, int expected, const struct timespec *rel) {
    long rc = futex(uaddr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, rel);
    return (int)rc;
}

int futex_wake(int *uaddr, int count) {
    return (int)futex(uaddr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, count, 0);
}

// ----------------------------------------------------------------- pipes

int pipe2(int fds[2], int flags) {
    return (int)syscall2(SYS_PIPE2, (int64_t)(uint64_t)(uintptr_t)fds, flags);
}

// Linux dropped the zero-argument pipe(2) on newer architectures and
// musl supplies it over pipe2 there. Same arrangement here: one
// syscall, and the legacy spelling is library code.
int pipe(int fds[2]) { return pipe2(fds, 0); }

// ------------------------------------------------------------- poll/select

#include "poll.h"
#define SYS_POLL_NR   67
#define SYS_SELECT_NR 68

int poll(struct pollfd *fds, unsigned long nfds, int timeout_ms) {
    return (int)syscall3(SYS_POLL_NR, (int64_t)(uint64_t)(uintptr_t)fds,
                         (int64_t)nfds, timeout_ms);
}

// ------------------------------------------------------------- reboot

#include "sys/reboot.h"
#define SYS_REBOOT_NR 69

int reboot(int cmd) {
    long rc = syscall1(SYS_REBOOT_NR, cmd);
    return rc < 0 ? -1 : 0;
}

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv) {
    // nfds/rd/wr/ex in the usual arg registers; tv (5th) in r8, which
    // the kernel reads as frame->r8 -- same trick as mmap_fd_raw.
    long ret;
    register long r10 __asm__("r10") = (long)(uintptr_t)ex;
    register long r8  __asm__("r8")  = (long)(uintptr_t)tv;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"((long)SYS_SELECT_NR), "D"((long)nfds),
                        "S"((long)(uintptr_t)rd), "d"((long)(uintptr_t)wr),
                        "r"(r10), "r"(r8)
                      : "rcx", "r11", "memory");
    return (int)ret;
}

int fcntl(int fd, int cmd, int arg) {
    return (int)syscall3(SYS_FCNTL, fd, cmd, arg);
}

int dup(int oldfd) {
    return (int)syscall1(SYS_DUP, oldfd);
}
int dup2(int oldfd, int newfd) {
    return (int)syscall2(SYS_DUP2, oldfd, newfd);
}
int dup3(int oldfd, int newfd, int flags) {
    return (int)syscall3(SYS_DUP3, oldfd, newfd, flags);
}

// ------------------------------------------------------------------- TLS

int arch_prctl(int code, unsigned long addr) {
    return (int)syscall2(SYS_ARCH_PRCTL, code, (int64_t)addr);
}

// --------------------------------------------------------- working dir

int chdir(const char *path) {
    return (int)syscall2(SYS_CHDIR, (int64_t)(uint64_t)(uintptr_t)path,
                         (int64_t)strlen(path));
}

// Linux's raw getcwd returns the length INCLUDING the NUL, or -ERANGE.
// POSIX's getcwd(3) returns the buffer, or NULL. The translation is
// library work, exactly as it is in musl.
char *getcwd(char *buf, uint64_t size) {
    long rc = syscall2(SYS_GETCWD, (int64_t)(uint64_t)(uintptr_t)buf,
                       (int64_t)size);
    if (rc < 0) { return 0; }
    return buf;
}

// ---------------------------------------------------------------- stat

int stat(const char *path, struct stat *st) {
    return (int)syscall3(SYS_STAT, (int64_t)(uint64_t)(uintptr_t)path,
                         (int64_t)strlen(path), (int64_t)(uint64_t)(uintptr_t)st);
}

int lstat(const char *path, struct stat *st) {
    return (int)syscall3(SYS_LSTAT, (int64_t)(uint64_t)(uintptr_t)path,
                         (int64_t)strlen(path), (int64_t)(uint64_t)(uintptr_t)st);
}

int fstat(int fd, struct stat *st) {
    return (int)syscall2(SYS_FSTAT, fd, (int64_t)(uint64_t)(uintptr_t)st);
}

// Five arguments: the path is a (pointer, length) pair, so `flags`
// lands in the fifth slot, which is r8 -- the same route mmap's fifth
// takes. __neoos_syscall6 already sets up r8/r9, so no new helper.
int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    return (int)__neoos_syscall6(SYS_NEWFSTATAT, dirfd,
                                 (long)(uintptr_t)path, (long)strlen(path),
                                 (long)(uintptr_t)st, flags, 0);
}

// ------------------------------------------------------- Tier 0 calls

int64_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return syscall3(SYS_WRITEV, fd, (int64_t)(uintptr_t)iov, iovcnt);
}

int64_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return syscall3(SYS_READV, fd, (int64_t)(uintptr_t)iov, iovcnt);
}

int ioctl(int fd, unsigned long request, void *arg) {
    return (int)syscall3(SYS_IOCTL, fd, (int64_t)request, (int64_t)(uintptr_t)arg);
}

// NeoOS has no terminal driver, so this is always 0. It exists because
// stdio asks, and because a program checking it should get an answer
// rather than a link error.
int isatty(int fd) {
    // A REAL struct, not NULL: the probe has to be a call the terminal
    // can actually answer, and musl's isatty passes one too.
    struct winsize ws;
    return ioctl(fd, TIOCGWINSZ, &ws) == 0 ? 1 : 0;
}

int clock_gettime(int clk, struct timespec *out) {
    return (int)syscall2(SYS_CLOCK_GETTIME, clk, (int64_t)(uintptr_t)out);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)syscall2(SYS_NANOSLEEP, (int64_t)(uintptr_t)req,
                         (int64_t)(uintptr_t)rem);
}

int set_tid_address(void *ptr) {
    return (int)syscall1(SYS_SET_TID_ADDRESS, (int64_t)(uintptr_t)ptr);
}

void exit_group(int code) {
    syscall1(SYS_EXIT_GROUP, code);
    for (;;) { }
}

// ------------------------------------------------------------ termios

int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int actions, const struct termios *t) {
    unsigned long req = TCSETS;
    if (actions == TCSADRAIN) { req = TCSETSW; }
    else if (actions == TCSAFLUSH) { req = TCSETSF; }
    return ioctl(fd, req, (void *)t);
}

// ------------------------------------------------------------ test hooks

#include "neoos_test.h"

int neoos_test_inject_key(unsigned keycode, int pressed) {
    return (int)syscall3(SYS_TEST_HOOK, TESTHOOK_INJECT_KEY,
                         (int64_t)keycode, (int64_t)pressed);
}

long neoos_test_migration_count(void) {
    return (long)syscall1(SYS_TEST_HOOK, TESTHOOK_MIG_COUNT);
}

int neoos_test_parent_pid(int pid) {
    return (int)syscall3(SYS_TEST_HOOK, TESTHOOK_PARENT_PID, (int64_t)pid, 0);
}

long neoos_test_pmm_free(void) {
    return (long)syscall1(SYS_TEST_HOOK, TESTHOOK_PMM_FREE);
}

long neoos_test_poll_stats(void) {
    return (long)syscall1(SYS_TEST_HOOK, TESTHOOK_POLL_STATS);
}

long neoos_test_poll_depth(void) {
    return (long)syscall1(SYS_TEST_HOOK, TESTHOOK_POLL_DEPTH);
}
