#ifndef NEOOS_FCNTL_H
#define NEOOS_FCNTL_H

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

// fcntl commands. Linux's numbers. Only F_GETFL and F_SETFL do
// anything: F_GETFD/F_SETFD are accepted and ignored (nothing closes
// fds at exec yet), and everything else returns -EINVAL rather than a
// silent success -- a caller that asked for a duplicate descriptor and
// got one would use fd -1 as if it were open.
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

// Returns the flags (F_GETFL), 0 (F_SETFL and the FD commands), or a
// negative errno.h code. F_GETFL reports only O_NONBLOCK; the access
// mode is not tracked per descriptor.
int fcntl(int fd, int cmd, int arg);

// Opens (or, with O_CREAT, creates) the file at `path` with the given
// flags. Returns a file descriptor, or a negative errno.h code on
// failure.
int open(const char *path, int flags);

#endif
