#include "unistd.h"
#include "string.h"
#include "fcntl.h"
#include "dirent.h"
#include "signal.h"

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
#define SYS_KILL          29
#define SYS_TKILL         30
#define SYS_TGKILL        31

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

void yield(void) {
    syscall0(SYS_YIELD);
}

int spawn(const char *path) {
    uint64_t len = strlen(path);
    return (int)syscall2(SYS_SPAWN, (int64_t)(uint64_t)path, (int64_t)len);
}

int fork(void) {
    return (int)syscall0(SYS_FORK);
}

int getdents(int fd, struct dirent *buf, int count) {
    return (int)syscall3(SYS_GETDENTS, fd, (int64_t)(uint64_t)buf, count);
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

int kill(int pid, int sig)             { return (int)syscall2(SYS_KILL, pid, sig); }
int tkill(int tid, int sig)            { return (int)syscall2(SYS_TKILL, tid, sig); }
int tgkill(int tgid, int tid, int sig) { return (int)syscall3(SYS_TGKILL, tgid, tid, sig); }
