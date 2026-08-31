#ifndef NEOOS_RAMFS_H
#define NEOOS_RAMFS_H

#include "fs/vfs.h"

#define RAMFS_MAX_NODES 32
#define RAMFS_MAX_PAGES 4   // 16KiB ceiling per file

extern const struct vfs_ops ramfs_ops;

#endif
