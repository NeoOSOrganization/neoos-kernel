#ifndef NEOOS_FS_STAT_H
#define NEOOS_FS_STAT_H

#include <stdint.h>

// Linux's x86-64 `struct stat`, EXACTLY. 144 bytes, and every offset
// fixed by the ABI -- this is musl's `struct kstat`
// (third_party/musl/arch/x86_64/kstat.h), which is what its
// stat/lstat/fstat wrappers read the kernel's answer into.
//
// No shim can retrofit this layout, because it is compiled into the
// caller. The _Static_asserts below are the whole point of writing it
// out longhand: a field reordered or a padding word dropped is a bug
// that would otherwise surface as a program reading st_size out of
// st_gid.
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

_Static_assert(sizeof(struct stat) == 144, "struct stat must be Linux's 144 bytes");
_Static_assert(__builtin_offsetof(struct stat, st_dev)     ==   0, "st_dev");
_Static_assert(__builtin_offsetof(struct stat, st_ino)     ==   8, "st_ino");
_Static_assert(__builtin_offsetof(struct stat, st_nlink)   ==  16, "st_nlink");
_Static_assert(__builtin_offsetof(struct stat, st_mode)    ==  24, "st_mode");
_Static_assert(__builtin_offsetof(struct stat, st_uid)     ==  28, "st_uid");
_Static_assert(__builtin_offsetof(struct stat, st_gid)     ==  32, "st_gid");
_Static_assert(__builtin_offsetof(struct stat, st_rdev)    ==  40, "st_rdev");
_Static_assert(__builtin_offsetof(struct stat, st_size)    ==  48, "st_size");
_Static_assert(__builtin_offsetof(struct stat, st_blksize) ==  56, "st_blksize");
_Static_assert(__builtin_offsetof(struct stat, st_blocks)  ==  64, "st_blocks");
_Static_assert(__builtin_offsetof(struct stat, st_atime_sec) == 72, "st_atime_sec");
_Static_assert(__builtin_offsetof(struct stat, st_mtime_sec) == 88, "st_mtime_sec");
_Static_assert(__builtin_offsetof(struct stat, st_ctime_sec) == 104, "st_ctime_sec");

// File type bits, in Linux's (and POSIX's) octal encoding.
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000

// AT_FDCWD, for the fstatat form. Linux's value.
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH       0x1000

struct vnode;
// Fills `out` from `vn`. See docs/stdlib.md for which fields are real
// and which are synthesized -- FAT stores neither owners nor
// timestamps NeoOS can read yet.
void vfs_stat_vnode(struct vnode *vn, struct stat *out);

#endif
