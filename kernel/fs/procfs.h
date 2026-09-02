#ifndef NEOOS_PROCFS_H
#define NEOOS_PROCFS_H

// A synthetic, read-only /proc: just enough for `ps` (BB5). See
// procfs.c for what it does and deliberately does not provide.
extern const struct vfs_ops procfs_ops;

#endif
