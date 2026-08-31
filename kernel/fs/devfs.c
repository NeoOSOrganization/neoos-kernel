#include "fs/devfs.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "errno.h"
#include "dev/serial.h"
#include "dev/vga.h"
#include "dev/tty.h"
#include "dev/evdev.h"
#include "dev/fb.h"
#include <stddef.h>

// Forward declarations of file_ops implementations
extern const struct file_ops tty_file_ops;

// null and zero device file_ops (simple implementations)
static int64_t null_read(void *buf, uint32_t len) { (void)buf; (void)len; return 0; }
static int64_t null_write(const void *buf, uint32_t len) { (void)buf; return (int64_t)len; }

static int64_t null_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f;
    return null_read(buf, (uint32_t)len);
}

static int64_t null_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    return null_write(buf, (uint32_t)len);
}

static int64_t null_fop_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -ESPIPE;
}

static int64_t null_fop_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

static int64_t null_fop_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f; (void)request; (void)arg;
    return -ENOTTY;
}

static int null_fop_poll(struct file_descriptor *f, int events) {
    (void)f;
    return events;  // null/zero are always ready
}

static void null_fop_dup(struct file_descriptor *f) { (void)f; }
static void null_fop_close(struct file_descriptor *f) { (void)f; }

static const struct file_ops null_file_ops = {
    .name     = "null",
    .read     = null_fop_read,
    .write    = null_fop_write,
    .lseek    = null_fop_lseek,
    .getdents = null_fop_getdents,
    .ioctl    = null_fop_ioctl,
    .poll     = null_fop_poll,
    .dup      = null_fop_dup,
    .close    = null_fop_close,
};

// zero device is similar to null but reads return zeros
static int64_t zero_read(void *buf, uint32_t len) {
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) { b[i] = 0; }
    return (int64_t)len;
}

static int64_t zero_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f;
    return zero_read(buf, (uint32_t)len);
}

static const struct file_ops zero_file_ops = {
    .name     = "zero",
    .read     = zero_fop_read,
    .write    = null_fop_write,
    .lseek    = null_fop_lseek,
    .getdents = null_fop_getdents,
    .ioctl    = null_fop_ioctl,
    .poll     = null_fop_poll,
    .dup      = null_fop_dup,
    .close    = null_fop_close,
};

// Device open functions
static int null_open(struct file_descriptor *f) {
    f->priv = NULL;
    f->ops = &null_file_ops;
    return 0;
}

static int zero_open(struct file_descriptor *f) {
    f->priv = NULL;
    f->ops = &zero_file_ops;
    return 0;
}

static int tty_open(struct file_descriptor *f) {
    // TTY opens as the console, with priv pointing to the console tty
    f->ops = &tty_file_ops;
    // The tty_file_ops will handle priv initialization
    return 0;
}

// Device table using the new devfs_dev structure with file_ops
static const struct devfs_dev devices[] = {
    // Format: { name, type, fops, open }
    // Note: index 0 is reserved for root "/"
    { "CONSOLE", VNODE_DEVICE, &tty_file_ops, tty_open },
    { "TTY",     VNODE_DEVICE, &tty_file_ops, tty_open },
    { "NULL",    VNODE_DEVICE, &null_file_ops, null_open },
    { "ZERO",    VNODE_DEVICE, &zero_file_ops, zero_open },
    { "input",   VNODE_DIR,    NULL,           NULL },
    { "input/event0", VNODE_DEVICE, &evdev_file_ops, evdev_devfs_open },
    // Appended AFTER input/* so the hardcoded inode ids in devfs_lookup /
    // devfs_readdir for the input dir (5) and event0 (6) do not shift.
    { "fb0",     VNODE_DEVICE, &fb_file_ops,  fb_open },
};
#define DEVFS_COUNT (sizeof(devices) / sizeof(devices[0]))

// Root entry (reserved, not in the devices table)
static const struct devfs_dev root_dev = {
    .name = "/",
    .type = VNODE_DIR,
    .fops = NULL,
    .open = NULL,
};

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Simple memcpy for use in devfs.c
static void *memcpy_local(void *dest, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static int devfs_mount_op(struct vfs_mount *m, const char *source) {
    (void)source;
    m->fs_private = 0;
    return 0;
}

static void devfs_umount_op(struct vfs_mount *m) { m->fs_private = 0; }

static int devfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    (void)m;
    // inode_id 0 is the root directory
    if (inode_id == 0) {
        out->type = VNODE_DIR;
        out->size = 0;
        out->fs_private = (void *)&root_dev;
        return 0;
    }
    // inode_id 1+ are device entries
    if (inode_id > DEVFS_COUNT) { return -ENOENT; }
    const struct devfs_dev *dev = &devices[inode_id - 1];
    out->type = dev->type;
    out->size = 0;
    out->fs_private = (void *)dev;
    return 0;
}

static int devfs_sync_inode(struct vnode *vn) { (void)vn; return 0; }

static int devfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    // Handle hierarchical lookups for input/event0.
    // If we're in the "input" directory (inode_id 5) and looking for "event0", return the event0 device.
    // If we're in the root and looking for "input", return the input dir.

    if (dir && dir->inode_id == 5) {  // We're in the "input" directory
        if (name_eq(name, "event0")) {
            *out_inode_id = 6;  // input/event0 is at devices[5], which is inode_id 6 (1-indexed)
            return 0;
        }
        return -ENOENT;
    }

    // Default root-level lookup (dir->inode_id == 0 or dir is NULL)
    for (uint64_t i = 0; i < DEVFS_COUNT; i++) {
        const char *entry_name = devices[i].name;

        // Check if this entry contains a "/"
        int has_slash = 0;
        for (const char *p = entry_name; *p; p++) {
            if (*p == '/') {
                has_slash = 1;
                break;
            }
        }

        if (has_slash) {
            // This is a hierarchical entry like "input/event0"
            // Only match the "input" part at root level
            if (name_eq(name, "input")) {
                *out_inode_id = i + 1;  // devices[i] corresponds to inode_id i+1
                return 0;
            }
        } else {
            // Regular flat entry
            if (name_eq(entry_name, name)) {
                *out_inode_id = i + 1;  // devices[i] corresponds to inode_id i+1
                return 0;
            }
        }
    }

    return -ENOENT;
}

// devfs devices now use file_ops for read/write, handled through the file descriptor.
// These VFS operations are no longer used for device files.
static int64_t devfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    (void)vn; (void)pos; (void)buf; (void)len;
    return -EINVAL;  // Devices must be opened through a file descriptor
}

static int64_t devfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    (void)vn; (void)pos; (void)buf; (void)len;
    return -EINVAL;  // Devices must be opened through a file descriptor
}

// devfs is read-only as a namespace: its node set is fixed at compile
// time. These return -EPERM rather than being NULL so callers never
// have to know which driver they are talking to.
static int devfs_create(struct vnode *dir, const char *name, uint64_t *out_id) {
    (void)dir; (void)name; (void)out_id; return -EPERM;
}
static int devfs_mkdir(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int devfs_unlink(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int devfs_truncate(struct vnode *vn) { (void)vn; return -EPERM; }

static int devfs_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out) {
    // Handle hierarchical directory listing.
    // At root level (inode_id 0), list all top-level devices and the "input" directory (once).
    // At input directory level (inode_id 5), list "event0".

    if (dir && dir->inode_id == 5) {  // We're in the "input" directory
        if (index != 0) { return -ENOENT; }  // Only one entry in input/
        // Return "event0"
        memcpy_local(out->name, "event0", 7);
        out->type = DT_CHR;
        out->ino = 6;  // input/event0 is at devices[5], inode_id 6
        return 0;
    }

    // Root level listing (dir->inode_id == 0 or dir is NULL)
    uint32_t count = 0;
    int input_dir_listed = 0;

    for (uint64_t i = 0; i < DEVFS_COUNT; i++) {
        const char *entry_name = devices[i].name;

        // Check if this is a hierarchical entry
        int has_slash = 0;
        for (const char *p = entry_name; *p; p++) {
            if (*p == '/') {
                has_slash = 1;
                break;
            }
        }

        if (has_slash) {
            // This is a hierarchical entry like "input/event0"
            // Only show the "input" dir if we haven't shown it yet
            if (!input_dir_listed && count == index) {
                memcpy_local(out->name, "input", 6);
                out->type = DT_DIR;
                out->ino = i + 1;  // devices[i] corresponds to inode_id i+1, but we want the "input" dir entry
                // Actually, we need the inode_id of the "input" directory, which is devices[4] at index 4
                // Find the "input" directory entry
                for (uint64_t j = 0; j < DEVFS_COUNT; j++) {
                    if (name_eq(devices[j].name, "input")) {
                        out->ino = j + 1;
                        break;
                    }
                }
                input_dir_listed = 1;
                return 0;
            }
            if (!input_dir_listed) {
                count++;
                input_dir_listed = 1;  // Only list input once
            }
        } else {
            // Regular flat entry
            if (count == index) {
                int j = 0;
                while (entry_name[j] && j < VFS_NAME_MAX - 1) {
                    out->name[j] = entry_name[j];
                    j++;
                }
                out->name[j] = '\0';
                out->type = DT_CHR;
                out->ino = i + 1;  // devices[i] corresponds to inode_id i+1
                return 0;
            }
            count++;
        }
    }

    return -ENOENT;
}

const struct vfs_ops devfs_ops = {
    .mount      = devfs_mount_op,
    .umount     = devfs_umount_op,
    .read_inode = devfs_read_inode,
    .sync_inode = devfs_sync_inode,
    .lookup     = devfs_lookup,
    .read       = devfs_read,
    .write      = devfs_write,
    .create     = devfs_create,
    .mkdir      = devfs_mkdir,
    .unlink     = devfs_unlink,
    .truncate   = devfs_truncate,
    .readdir    = devfs_readdir,
};

// CONSOLE and TTY are the same underlying terminal; NULL and ZERO are
// not terminals and must report -ENOTTY, or isatty() would claim a
// redirect to /dev/null was a tty.
int devfs_vnode_is_tty(struct vnode *vn) {
    if (!vn || vn->type != VNODE_DEVICE) { return 0; }
    if (vn->inode_id == 0) { return 0; }  // Root is not a device
    uint64_t dev_idx = vn->inode_id - 1;  // Convert inode_id to devices array index
    if (dev_idx >= DEVFS_COUNT) { return 0; }
    return name_eq(devices[dev_idx].name, "CONSOLE") ||
           name_eq(devices[dev_idx].name, "TTY");
}
