#ifndef NEOOS_DIRENT_H
#define NEOOS_DIRENT_H

#include <stdint.h>

// Linux's d_type values. These arrive inside a getdents64 record, so
// they are Linux's numbers -- they used to be a private 1/2/3, which
// disagreed with everything compiled against a real <dirent.h>.
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

// Linux's userspace `struct dirent` for 64-bit: the getdents64 record
// with d_name given a fixed upper bound so callers can hold one by
// value. The kernel writes VARIABLE-length records; readdir() hands
// back a pointer into its own buffer, so the declared d_name size is a
// ceiling, not the on-the-wire size.
//
// Must stay identical to kernel/fs/vfs.h's struct linux_dirent64. The
// kernel and library trees do not share headers, so the definition is
// duplicated and the two must move together -- like the syscall
// numbers, and like struct stat.
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

// Opaque to callers: allocated from a fixed pool inside the library.
typedef struct DIR DIR;

// The raw syscall. Fills `buf` with as many complete records as fit in
// `bytes`, returning the number of BYTES written, 0 at end of
// directory, or a negative <errno.h> code. -EINVAL if the buffer
// cannot hold even one record.
//
// Note this counts BYTES, not entries: the records are variable
// length, so an entry count could not say how much room there is.
int getdents(int fd, void *buf, int bytes);

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif
