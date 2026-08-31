#ifndef NEOOS_SYS_STAT_H
#define NEOOS_SYS_STAT_H

#include <stdint.h>

// Linux's x86-64 `struct stat`, byte for byte. Must stay identical to
// kernel/fs/stat.h -- the kernel and library trees do not share
// headers, so the definition is duplicated and the two must move
// together, exactly like the syscall numbers.
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;

    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;

    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
};

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH       0x1000

// All four return 0, or a negative <errno.h> code. NeoOS returns the
// error directly rather than -1 plus errno; see docs/stdlib.md.
int stat(const char *path, struct stat *st);
// Identical to stat: no filesystem NeoOS mounts can hold a symlink.
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);

#endif
