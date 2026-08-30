#ifndef NEOOS_FCNTL_H
#define NEOOS_FCNTL_H

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400
// Linux's values. Only pipe2() honours O_NONBLOCK today; open() ignores
// both, since no NeoOS file can block. O_CLOEXEC is accepted and
// ignored -- there is no exec-time fd table walk yet. docs/stdlib.md.
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000

// Opens (or, with O_CREAT, creates) the file at `path` with the given
// flags. Returns a file descriptor, or a negative errno.h code on
// failure.
int open(const char *path, int flags);

#endif
