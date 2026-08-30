#ifndef NEOOS_UNISTD_H
#define NEOOS_UNISTD_H

#include <stdint.h>

// NeoOS's standard library. Function names follow POSIX convention
// where the semantics match; spawn/wait are NeoOS-specific: spawn
// builds a fresh process directly from a path (not fork+exec), and
// wait takes one specific PID (not "any child").

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

void exit(int code) __attribute__((noreturn));

// Writes `len` bytes from `buf` to the file (or console, for fd
// STDOUT_FILENO/STDERR_FILENO) open on `fd`. Returns the number of
// bytes written, or a negative errno.h code on failure.
int64_t write(int fd, const void *buf, uint64_t len);

// Reads up to `len` bytes from the file (or console, for fd
// STDIN_FILENO, which always returns 0 -- there is no
// keyboard-to-process input path yet) open on `fd` into `buf`.
// Returns the number of bytes actually read (0 at EOF), or a negative
// errno.h code on failure.
int64_t read(int fd, void *buf, uint64_t len);

// Closes `fd`. Returns 0, or a negative errno.h code on failure.
int close(int fd);

// Moves fd's read/write position. whence is SEEK_SET/SEEK_CUR/
// SEEK_END. Returns the new absolute position, or a negative errno.h
// code on failure.
int64_t lseek(int fd, int64_t offset, int whence);

int getpid(void);

// Creates a pipe: fds[0] is the read end, fds[1] the write end.
// Returns 0, or a negative errno.h code.
//
// A read from an empty pipe blocks until data arrives, or returns 0
// once every write end is closed. A write blocks while the pipe is
// full, and raises SIGPIPE (returning -EPIPE if it transferred
// nothing) once every read end is closed. `flags` takes O_NONBLOCK,
// which makes both ends return -EAGAIN rather than blocking.
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);

// spawn with an argument vector. `argv` is NULL-terminated and becomes
// the new process's argv; passing null is the same as spawn(). At most
// 8 arguments of 128 bytes each -- see docs/stdlib.md.
int spawnv(const char *path, char *const argv[]);

// sysconf() names. Values match Linux's <bits/confname.h> so a ported
// program compiled against glibc/musl headers sees the same constants.
#define _SC_NPROCESSORS_CONF  83
#define _SC_NPROCESSORS_ONLN  84

long sysconf(int name);
int  sched_getcpu(void);
void yield(void);

// Builds a fresh process directly from the ELF executable at `path`
// (NUL-terminated) and returns its PID, or -1 on failure.
int spawn(const char *path);

// Blocks until the process with the given PID exits, reaps it, and
// returns its exit code.
int wait(int pid);

// Duplicates the calling process. Returns 0 in the child, the
// child's PID in the parent, or -1 on failure (parent unaffected).
// Each side's open file descriptors are independent copies after
// this call -- reads/writes/lseeks on inherited fds no longer share
// a position between parent and child (unlike POSIX, which shares one
// underlying open-file description). NeoOS-specific simplification.
int fork(void);

// Replaces the calling process's address space with the ELF
// executable at `path` (NUL-terminated). Open file descriptors, pid,
// and parent are preserved. On success, never returns -- execution
// continues at the new program's entry point. Returns -1 on failure
// (bad path, out of memory), leaving the calling process completely
// unchanged and still running its original code.
int exec(const char *path);

// Mounts the filesystem `fstype` ("fat", "ramfs", or "devfs") at
// `target`. `source` is "hd0" or "hd1" for "fat" and ignored
// otherwise; FAT16 versus FAT32 is auto-detected. Returns 0, or
// -ENODEV (unknown type or unreadable volume), -EEXIST (already
// mounted there), or -ENOSPC (mount table full).
int mount(const char *source, const char *target, const char *fstype);

// Unmounts the filesystem at `target`. Returns 0, -ENOENT if nothing
// is mounted there, or -EBUSY if any file on it is still open.
int umount(const char *target);

// Creates a new, empty directory at `path`. Returns 0, or a negative
// errno.h code on failure.
int mkdir(const char *path);

// Deletes the file at `path`. Returns 0, or a negative errno.h code
// on failure (including -EISDIR if `path` is a directory).
int unlink(const char *path);

#endif
