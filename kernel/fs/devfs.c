#include "fs/devfs.h"
#include "errno.h"
#include "dev/serial.h"
#include "dev/vga.h"
#include "dev/tty.h"

// Static device table. inode_id is the index into it; id 0 is the root
// directory, so real devices start at 1.
struct devfs_node {
    const char     *name;
    enum vnode_type type;
    int64_t (*read)(void *buf, uint32_t len);
    int64_t (*write)(const void *buf, uint32_t len);
};

// The console IS the terminal now: both ends go through the line
// discipline, so a read blocks for a line, ^C signals the foreground
// group, and writes get ONLCR.
static int64_t console_read(void *buf, uint32_t len) {
    return tty_read(buf, len);
}

static int64_t console_write(const void *buf, uint32_t len) {
    return tty_write(buf, len);
}

static int64_t null_read(void *buf, uint32_t len) { (void)buf; (void)len; return 0; }
static int64_t null_write(const void *buf, uint32_t len) { (void)buf; return (int64_t)len; }

static int64_t zero_read(void *buf, uint32_t len) {
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) { b[i] = 0; }
    return (int64_t)len;
}

static const struct devfs_node devices[] = {
    { "/",       VNODE_DIR,    0,            0             },
    { "CONSOLE", VNODE_DEVICE, console_read, console_write },
    { "TTY",     VNODE_DEVICE, console_read, console_write },
    { "NULL",    VNODE_DEVICE, null_read,    null_write    },
    { "ZERO",    VNODE_DEVICE, zero_read,    null_write    },
};
#define DEVFS_COUNT (sizeof(devices) / sizeof(devices[0]))

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int devfs_mount_op(struct vfs_mount *m, const char *source) {
    (void)source;
    m->fs_private = 0;
    return 0;
}

static void devfs_umount_op(struct vfs_mount *m) { m->fs_private = 0; }

static int devfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    (void)m;
    if (inode_id >= DEVFS_COUNT) { return -ENOENT; }
    out->type = devices[inode_id].type;
    out->size = 0;
    out->fs_private = (void *)&devices[inode_id];
    return 0;
}

static int devfs_sync_inode(struct vnode *vn) { (void)vn; return 0; }

static int devfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    (void)dir;
    for (uint64_t i = 1; i < DEVFS_COUNT; i++) {
        if (name_eq(devices[i].name, name)) { *out_inode_id = i; return 0; }
    }
    return -ENOENT;
}

static int64_t devfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    (void)pos; // devices are not seekable; position is ignored
    const struct devfs_node *d = (const struct devfs_node *)vn->fs_private;
    if (!d->read) { return -EPERM; }
    return d->read(buf, len);
}

static int64_t devfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    (void)pos;
    const struct devfs_node *d = (const struct devfs_node *)vn->fs_private;
    if (!d->write) { return -EPERM; }
    return d->write(buf, len);
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
    (void)dir;
    uint64_t id = index + 1; // skip the root entry at index 0
    if (id >= DEVFS_COUNT) { return -ENOENT; }
    int i = 0;
    while (devices[id].name[i] && i < VFS_NAME_MAX - 1) { out->name[i] = devices[id].name[i]; i++; }
    out->name[i] = '\0';
    out->type = DT_CHR;
    out->ino  = id;
    return 0;
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
    return name_eq(devices[vn->inode_id].name, "CONSOLE") ||
           name_eq(devices[vn->inode_id].name, "TTY");
}
