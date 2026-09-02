#ifndef NEOOS_SYSCALL_INTERNAL_H
#define NEOOS_SYSCALL_INTERNAL_H

// Shared between syscall.c and the sys_*.c handler files it was split
// into. NOT a public kernel interface: nothing outside kernel/syscall/
// should include this.

#include <stdint.h>
#include "syscall/syscall.h"

struct process;
struct file_descriptor;

// Handlers take one argument rather than the five they would otherwise
// need. That is not only tidier: with five parameters, every handler
// that ignores some of them needs a `(void)` cast to satisfy
// -Wunused-parameter, and forty of those is noise that hides the one
// place it might have meant something.
struct syscall_args {
    int64_t a1, a2, a3, a4;
    // The caller's full saved user context. fork() copies it, exec()
    // and sigreturn() overwrite it in place, and mmap reads arguments
    // 5 and 6 out of frame->r8/r9 -- syscall_entry.asm pushes those
    // before its argument shuffle, so the four-argument calling
    // convention never had to be widened.
    struct syscall_frame *frame;
};

typedef int64_t (*syscall_handler)(struct syscall_args *);


// ---- flag values and shorthands shared by the handlers ---------------
//
// These cross the syscall boundary, so their VALUES are Linux's except
// All values match Linux x86_64.

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

#define fs_lock_acquire() vfs_lock()
#define fs_lock_release() vfs_unlock()

#define F_DUPFD 0
#define F_DUPFD_CLOEXEC 1030
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x800

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

// Test-hook request codes (SYS_TEST_HOOK only in NEOOS_TEST_HOOKS builds)
#define TESTHOOK_INJECT_KEY   1   // a2 = keycode, a3 = pressed
#define TESTHOOK_MIG_COUNT    2   // returns the user-thread migration count
#define TESTHOOK_PARENT_PID   3   // a2 = pid; returns that process's parent_pid or -ESRCH
#define TESTHOOK_PMM_FREE     4   // returns pmm_free_frame_count()
#define TESTHOOK_POLL_STATS   5   // returns (poll_events << 32) | poll_wakeups
#define TESTHOOK_POLL_DEPTH   6   // threads blocked on the poll broadcast now
#define TESTHOOK_POLL_WASTED  7   // wakeups that found nothing ready

// ---- shared helpers, defined in syscall.c ---------------------------

struct file_descriptor *fd_get(struct process *p, int fd);
int  fd_alloc(struct process *p);
int  fd_close(struct process *p, int fd);

void copy_user_string(int64_t user_ptr, char *out, uint64_t out_size);
void copy_user_path(int64_t user_ptr, int64_t user_len, char *out, uint64_t out_size);
// Copies a user path and resolves it against the CALLER'S cwd. `out`
// must be VFS_MAX_PATH bytes.
int  copy_user_path_at(int64_t user_ptr, int64_t user_len, char *out);

// ---- the handlers ---------------------------------------------------

int64_t sys_arch_prctl(struct syscall_args *a);
int64_t sys_bind(struct syscall_args *a);
int64_t sys_chdir(struct syscall_args *a);
int64_t sys_close(struct syscall_args *a);
int64_t sys_connect(struct syscall_args *a);
int64_t sys_cpu_count(struct syscall_args *a);
int64_t sys_exec(struct syscall_args *a);
int64_t sys_exit(struct syscall_args *a);
int64_t sys_fcntl(struct syscall_args *a);
int64_t sys_fork(struct syscall_args *a);
int64_t sys_futex(struct syscall_args *a);
int64_t sys_getcpu(struct syscall_args *a);
int64_t sys_getcwd(struct syscall_args *a);
int64_t sys_stat(struct syscall_args *a);
int64_t sys_lstat(struct syscall_args *a);
int64_t sys_fstat(struct syscall_args *a);
int64_t sys_newfstatat(struct syscall_args *a);
int64_t sys_set_tid_address(struct syscall_args *a);
int64_t sys_exit_group(struct syscall_args *a);
int64_t sys_writev(struct syscall_args *a);
int64_t sys_readv(struct syscall_args *a);
int64_t sys_ioctl(struct syscall_args *a);
int64_t sys_clock_gettime(struct syscall_args *a);
int64_t sys_nanosleep(struct syscall_args *a);
int64_t sys_getdents(struct syscall_args *a);
int64_t sys_getpgid(struct syscall_args *a);
int64_t sys_getpid(struct syscall_args *a);
int64_t sys_getppid(struct syscall_args *a);
int64_t sys_uname(struct syscall_args *a);
int64_t sys_brk(struct syscall_args *a);
int64_t sys_getuid(struct syscall_args *a);
int64_t sys_getsid(struct syscall_args *a);
int64_t sys_getsockname(struct syscall_args *a);
int64_t sys_kill(struct syscall_args *a);
int64_t sys_lseek(struct syscall_args *a);
int64_t sys_mkdir(struct syscall_args *a);
int64_t sys_mmap(struct syscall_args *a);
int64_t sys_mount(struct syscall_args *a);
int64_t sys_mprotect(struct syscall_args *a);
int64_t sys_munmap(struct syscall_args *a);
int64_t sys_open(struct syscall_args *a);
int64_t sys_pipe2(struct syscall_args *a);
int64_t sys_read(struct syscall_args *a);
int64_t sys_recvfrom(struct syscall_args *a);
int64_t sys_rt_sigaction(struct syscall_args *a);
int64_t sys_rt_sigpending(struct syscall_args *a);
int64_t sys_rt_sigprocmask(struct syscall_args *a);
int64_t sys_rt_sigqueueinfo(struct syscall_args *a);
int64_t sys_rt_sigreturn(struct syscall_args *a);
int64_t sys_rt_sigsuspend(struct syscall_args *a);
int64_t sys_rt_sigtimedwait(struct syscall_args *a);
int64_t sys_sendto(struct syscall_args *a);
int64_t sys_setpgid(struct syscall_args *a);
int64_t sys_setsid(struct syscall_args *a);
int64_t sys_sigaltstack(struct syscall_args *a);
int64_t sys_socket(struct syscall_args *a);
int64_t sys_spawn(struct syscall_args *a);
int64_t sys_spawnv(struct syscall_args *a);
int64_t sys_tgkill(struct syscall_args *a);
int64_t sys_thread_create(struct syscall_args *a);
int64_t sys_thread_exit(struct syscall_args *a);
int64_t sys_thread_join(struct syscall_args *a);
int64_t sys_thread_self(struct syscall_args *a);
int64_t sys_tkill(struct syscall_args *a);
int64_t sys_umount(struct syscall_args *a);
int64_t sys_unlink(struct syscall_args *a);
int64_t sys_wait(struct syscall_args *a);
int64_t sys_wait4(struct syscall_args *a);
int64_t sys_write(struct syscall_args *a);
int64_t sys_yield(struct syscall_args *a);

// Always declared; returns -ENOSYS without -DNEOOS_TEST_HOOKS.
int64_t sys_test_hook(struct syscall_args *a);

int64_t sys_poll(struct syscall_args *a);
int64_t sys_select(struct syscall_args *a);
int64_t sys_reboot(struct syscall_args *a);
int64_t sys_dup(struct syscall_args *a);
int64_t sys_dup2(struct syscall_args *a);
int64_t sys_dup3(struct syscall_args *a);

#endif
