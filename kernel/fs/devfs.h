#ifndef NEOOS_DEVFS_H
#define NEOOS_DEVFS_H

#include "fs/vfs.h"

extern const struct vfs_ops devfs_ops;

// Forward declaration
struct file_ops;
struct file_descriptor;

// Device entry in the devfs device table. When a device fd is opened,
// sys_open calls dev->open(f) to initialize f->priv and sets f->ops to dev->fops.
struct devfs_dev {
    const char            *name;
    enum vnode_type        type;
    const struct file_ops *fops;
    int (*open)(struct file_descriptor *f);
};

// 1 if this devfs vnode is the terminal. ioctl needs to distinguish a
// tty from /dev/NULL, and the device table is private to devfs.c.
int devfs_vnode_is_tty(struct vnode *vn);

#endif
