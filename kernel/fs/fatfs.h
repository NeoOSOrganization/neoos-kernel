#ifndef NEOOS_FATFS_H
#define NEOOS_FATFS_H

#include <stdint.h>
#include "errno.h"
#include "fs/vfs.h"

// FAT inode identity is the file's directory-entry location on disk,
// (dir_entry_lba << 16) | dir_entry_offset -- unique per file per
// volume. First cluster will not serve: every empty file has cluster
// 0. Reserved id 0 means the root directory, which has no directory
// entry of its own; that cannot collide with a real entry, because id
// 0 requires dir_entry_lba == 0 and LBA 0 is the boot sector.
#define FATFS_ROOT_INODE 0ULL

extern const struct vfs_ops fatfs_ops;

// The legacy single-volume entry points. Everything else that was
// here -- fat16_find, fat16_read_file, fat16_read_at, fat16_write_file,
// fat16_truncate, fat16_create_file, fat16_mkdir, fat16_delete_entry,
// fat16_update_entry_size -- is gone: syscall.c and process.c now go
// through fatfs_ops. These three remain because kmain still calls
// them, and the selftests exercise the driver's internals directly.
int fat16_mount(void);
void fat16_selftest(void);
void fat16_write_selftest(void);

#endif
