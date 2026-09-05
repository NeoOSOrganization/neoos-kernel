#include "fs/embedfs.h"
#include "errno.h"
#include <stddef.h>

// Supplied by build/embedfs_table.c (tools/gen-embedfs.py), which is
// regenerated every build from whatever *.nex files are staged for
// this build (always the boot-critical apps; optionally the test
// suite and/or ports via EMBED_DIRS). An empty table is valid -- it is
// what a bare `make test` with no EMBED_DIRS produces -- and every
// lookup against it correctly reports ENOENT rather than crashing.
extern const struct embedfs_entry g_embedfs_table[];
extern const int g_embedfs_table_count;

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void name_copy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < VFS_NAME_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// The category this mount serves ("bin", "sbin", or "tests"), passed
// as `source` to vfs_mount_fs and stashed in fs_private. The string
// always outlives the mount -- callers pass a literal.
static int embedfs_mount_op(struct vfs_mount *m, const char *source) {
    m->fs_private = (void *)source;
    return 0;
}

static void embedfs_umount_op(struct vfs_mount *m) { (void)m; }

// inode_id 0 is the mount's root directory (every driver's convention,
// per vfs_mount_fs: "reserved id 0 is every driver's root"). 1..N map
// to g_embedfs_table[0..N-1].
static int embedfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    (void)m;
    if (inode_id == 0) {
        out->type = VNODE_DIR;
        out->size = 0;
        out->mtime = out->atime = out->ctime = 0;
        out->fs_private = 0;
        return 0;
    }
    if (inode_id > (uint64_t)g_embedfs_table_count) { return -ENOENT; }
    const struct embedfs_entry *e = &g_embedfs_table[inode_id - 1];
    out->type = VNODE_FILE;
    out->size = (uint32_t)((const char *)e->end - (const char *)e->data);
    out->mtime = out->atime = out->ctime = 0;
    out->fs_private = (void *)e;
    return 0;
}

static int embedfs_sync_inode(struct vnode *vn) { (void)vn; return 0; }

static int embedfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    const char *category = (const char *)dir->mount->fs_private;
    for (int i = 0; i < g_embedfs_table_count; i++) {
        if (name_eq(g_embedfs_table[i].category, category) &&
            name_eq(g_embedfs_table[i].name, name)) {
            *out_inode_id = (uint64_t)(i + 1);
            return 0;
        }
    }
    return -ENOENT;
}

static int64_t embedfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    const struct embedfs_entry *e = (const struct embedfs_entry *)vn->fs_private;
    uint32_t size = (uint32_t)((const char *)e->end - (const char *)e->data);
    if (pos >= size) { return 0; }
    if (pos + len > size) { len = size - pos; }
    const uint8_t *src = (const uint8_t *)e->data + pos;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) { dst[i] = src[i]; }
    return (int64_t)len;
}

// Read-only: every mutating op returns -EROFS, never NULL (per the
// vfs_ops convention: "no op pointer is ever NULL").
static int64_t embedfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    (void)vn; (void)pos; (void)buf; (void)len;
    return -EROFS;
}

static int embedfs_create(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    (void)dir; (void)name; (void)out_inode_id;
    return -EROFS;
}

static int embedfs_mkdir(struct vnode *dir, const char *name) {
    (void)dir; (void)name;
    return -EROFS;
}

static int embedfs_unlink(struct vnode *dir, const char *name) {
    (void)dir; (void)name;
    return -EROFS;
}

static int embedfs_truncate(struct vnode *vn) { (void)vn; return -EROFS; }

static int embedfs_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out) {
    const char *category = (const char *)dir->mount->fs_private;
    uint32_t seen = 0;
    for (int i = 0; i < g_embedfs_table_count; i++) {
        if (!name_eq(g_embedfs_table[i].category, category)) { continue; }
        if (seen == index) {
            name_copy(out->name, g_embedfs_table[i].name);
            out->type = DT_REG;
            out->ino = (uint64_t)(i + 1);
            return 0;
        }
        seen++;
    }
    return -ENOENT; // past the last entry
}

const struct vfs_ops embedfs_ops = {
    .mount      = embedfs_mount_op,
    .umount     = embedfs_umount_op,
    .read_inode = embedfs_read_inode,
    .sync_inode = embedfs_sync_inode,
    .lookup     = embedfs_lookup,
    .read       = embedfs_read,
    .write      = embedfs_write,
    .create     = embedfs_create,
    .mkdir      = embedfs_mkdir,
    .unlink     = embedfs_unlink,
    .truncate   = embedfs_truncate,
    .readdir    = embedfs_readdir,
};
