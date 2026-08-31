#ifndef NEOOS_DEVFS_H
#define NEOOS_DEVFS_H

#include "fs/vfs.h"

extern const struct vfs_ops devfs_ops;

// 1 if this devfs vnode is the terminal. ioctl needs to distinguish a
// tty from /dev/NULL, and the device table is private to devfs.c.
int devfs_vnode_is_tty(struct vnode *vn);

#endif
