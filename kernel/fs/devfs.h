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

// Runtime device registration. `path` is relative to /dev and may
// contain one slash ("pts/3"); the parent directory must already be a
// static entry. On open, f->ops is set to `ops`, f->priv to `priv`,
// and `open(f)` (if non-null) is called. Returns 0, -EEXIST, -ENOSPC.
int  devfs_register(const char *path, const struct file_ops *ops, void *priv,
                    int (*open)(struct file_descriptor *f));
void devfs_unregister(const char *path);
// A block node at the ROOT of /dev ("sda", "sda1"): same dynamic table
// as /dev/pts/N, with VNODE_BLOCK and its dev_t. Returns as devfs_register.
int  devfs_register_blk(const char *name, const struct file_ops *ops, void *priv, uint64_t rdev);
// st_rdev for a devfs vnode: a block node's dev_t, 0 for everything else.
uint64_t devfs_vnode_rdev(const struct vnode *vn);

void devfs_selftest(void);

#endif
