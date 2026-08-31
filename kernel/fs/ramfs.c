#include "fs/ramfs.h"
#include "errno.h"
#include "mm/pmm.h"
#include "mm/paging.h"

struct ramfs_node {
    int             in_use;
    char            name[VFS_NAME_MAX];
    uint32_t        parent;                  // node index; node 0 is the root
    enum vnode_type type;
    uint32_t        size;
    uint64_t        pages[RAMFS_MAX_PAGES];  // physical frames, 0 = unallocated
};

// One global pool: this milestone mounts exactly one ramfs. A second
// ramfs mount would share it, which is why vfs_mount_fs rejects a
// duplicate target and nothing here mounts ramfs twice.
static struct ramfs_node nodes[RAMFS_MAX_NODES];

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void name_copy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < VFS_NAME_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int ramfs_mount_op(struct vfs_mount *m, const char *source) {
    (void)source;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        nodes[i].in_use = 0;
        for (int p = 0; p < RAMFS_MAX_PAGES; p++) { nodes[i].pages[p] = 0; }
    }
    nodes[0].in_use = 1;
    nodes[0].type = VNODE_DIR;
    nodes[0].parent = 0;
    nodes[0].size = 0;
    name_copy(nodes[0].name, "/");
    m->fs_private = nodes;
    return 0;
}

static void ramfs_umount_op(struct vfs_mount *m) {
    (void)m;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        for (int p = 0; p < RAMFS_MAX_PAGES; p++) {
            if (nodes[i].pages[p]) {
                pmm_free(nodes[i].pages[p], 0);
                nodes[i].pages[p] = 0;
            }
        }
        nodes[i].in_use = 0;
    }
}

static int ramfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    (void)m;
    if (inode_id >= RAMFS_MAX_NODES || !nodes[inode_id].in_use) {
        return -ENOENT;
    }
    out->type = nodes[inode_id].type;
    out->size = nodes[inode_id].size;
    out->fs_private = &nodes[inode_id];
    return 0;
}

static int ramfs_sync_inode(struct vnode *vn) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (n) { n->size = vn->size; }
    return 0;
}

static int ramfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    uint32_t dir_index = (uint32_t)(d - nodes);
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use && nodes[i].parent == dir_index &&
            (uint32_t)i != dir_index && name_eq(nodes[i].name, name)) {
            *out_inode_id = (uint64_t)i;
            return 0;
        }
    }
    return -ENOENT;
}

static int64_t ramfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (pos >= n->size) { return 0; }
    if (pos + len > n->size) { len = n->size - pos; }

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t done = 0; done < len; ) {
        uint32_t off = pos + done;
        uint32_t page = off / PMM_FRAME_SIZE;
        uint32_t in_page = off % PMM_FRAME_SIZE;
        uint32_t chunk = PMM_FRAME_SIZE - in_page;
        if (chunk > len - done) { chunk = len - done; }
        if (page >= RAMFS_MAX_PAGES || !n->pages[page]) { return (int64_t)done; }
        uint8_t *src = (uint8_t *)phys_to_virt(n->pages[page]) + in_page;
        for (uint32_t k = 0; k < chunk; k++) { dst[done + k] = src[k]; }
        done += chunk;
    }
    return (int64_t)len;
}

static int64_t ramfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (pos + len > RAMFS_MAX_PAGES * PMM_FRAME_SIZE) { return -ENOSPC; }

    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t done = 0; done < len; ) {
        uint32_t off = pos + done;
        uint32_t page = off / PMM_FRAME_SIZE;
        uint32_t in_page = off % PMM_FRAME_SIZE;
        uint32_t chunk = PMM_FRAME_SIZE - in_page;
        if (chunk > len - done) { chunk = len - done; }

        if (!n->pages[page]) {
            uint64_t frame = pmm_alloc(0);
            if (!frame) { return -ENOSPC; }
            uint8_t *z = (uint8_t *)phys_to_virt(frame);
            for (uint32_t k = 0; k < PMM_FRAME_SIZE; k++) { z[k] = 0; }
            n->pages[page] = frame;
        }
        uint8_t *dst = (uint8_t *)phys_to_virt(n->pages[page]) + in_page;
        for (uint32_t k = 0; k < chunk; k++) { dst[k] = src[done + k]; }
        done += chunk;
    }

    if (pos + len > n->size) {
        n->size = pos + len;
        vn->size = n->size;
    }
    return (int64_t)len;
}

static int ramfs_create(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    uint64_t existing;
    if (ramfs_lookup(dir, name, &existing) == 0) { return -EEXIST; }

    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    for (int i = 1; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use) { continue; }
        nodes[i].in_use = 1;
        nodes[i].type = VNODE_FILE;
        nodes[i].parent = (uint32_t)(d - nodes);
        nodes[i].size = 0;
        name_copy(nodes[i].name, name);
        *out_inode_id = (uint64_t)i;
        return 0;
    }
    return -ENOSPC;
}

static int ramfs_truncate(struct vnode *vn) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    for (int p = 0; p < RAMFS_MAX_PAGES; p++) {
        if (n->pages[p]) { pmm_free(n->pages[p], 0); n->pages[p] = 0; }
    }
    n->size = 0;
    vn->size = 0;
    return 0;
}

static int ramfs_mkdir(struct vnode *dir, const char *name) {
    uint64_t existing;
    if (ramfs_lookup(dir, name, &existing) == 0) { return -EEXIST; }

    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    for (int i = 1; i < RAMFS_MAX_NODES; i++) {
        if (nodes[i].in_use) { continue; }
        nodes[i].in_use = 1;
        nodes[i].type = VNODE_DIR;
        nodes[i].parent = (uint32_t)(d - nodes);
        nodes[i].size = 0;
        name_copy(nodes[i].name, name);
        return 0;
    }
    return -ENOSPC;
}

static int ramfs_unlink(struct vnode *dir, const char *name) {
    uint64_t id;
    int rc = ramfs_lookup(dir, name, &id);
    if (rc != 0) { return rc; }
    if (nodes[id].type == VNODE_DIR) { return -EISDIR; }

    for (int p = 0; p < RAMFS_MAX_PAGES; p++) {
        if (nodes[id].pages[p]) { pmm_free(nodes[id].pages[p], 0); nodes[id].pages[p] = 0; }
    }
    nodes[id].in_use = 0;
    return 0;
}

// Enumerates the dir's children by ordinal. `index` counts only
// matching children, so callers can walk 0,1,2,... until -ENOENT
// without knowing anything about the pool's internal layout.
static int ramfs_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out) {
    struct ramfs_node *d = (struct ramfs_node *)dir->fs_private;
    uint32_t dir_index = (uint32_t)(d - nodes);
    uint32_t seen = 0;

    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!nodes[i].in_use || nodes[i].parent != dir_index || (uint32_t)i == dir_index) {
            continue;
        }
        if (seen == index) {
            name_copy(out->name, nodes[i].name);
            out->type = (nodes[i].type == VNODE_DIR) ? DT_DIR : DT_REG;
            out->ino  = (uint64_t)i;
            return 0;
        }
        seen++;
    }
    return -ENOENT; // past the last entry
}

const struct vfs_ops ramfs_ops = {
    .mount      = ramfs_mount_op,
    .umount     = ramfs_umount_op,
    .read_inode = ramfs_read_inode,
    .sync_inode = ramfs_sync_inode,
    .lookup     = ramfs_lookup,
    .read       = ramfs_read,
    .write      = ramfs_write,
    .create     = ramfs_create,
    .mkdir      = ramfs_mkdir,
    .unlink     = ramfs_unlink,
    .truncate   = ramfs_truncate,
    .readdir    = ramfs_readdir,
};
