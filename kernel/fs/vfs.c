#include "vfs.h"
#include "vnode_slab.h"
#include "ramfs.h"
#include "fatfs.h"
#include "devfs.h"
#include "../errno.h"
#include "../serial.h"
#include "../sched/proc.h"
#include "../sched/fd_table.h"

static struct vfs_mount mounts[MAX_MOUNTS];
// vnodes now allocated via slab allocator (vnode_slab.c)

#define VNODE_BUCKETS 16
static struct vnode *buckets[VNODE_BUCKETS];

static unsigned bucket_of(struct vfs_mount *m, uint64_t inode_id) {
    // Mix the mount pointer in so two volumes' identical inode ids
    // (both roots are id 0) do not always collide in one bucket.
    uint64_t h = inode_id ^ ((uint64_t)(uintptr_t)m >> 4);
    return (unsigned)(h % VNODE_BUCKETS);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, uint64_t dst_size) {
    uint64_t i = 0;
    while (src[i] && i < dst_size - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Number of pool slots currently claimed. A mount holds one (its
// root), so a quiesced system with N mounts reads exactly N. Used by
// vfs_selftest's leaked-reference check and by the end-of-milestone
// leak gate; also the first thing to look at when a refcount bug is
// suspected, since a count that rises across an operation localises it
// immediately.
uint32_t vfs_vnode_in_use_count(void) {
    return vnode_slab_in_use_count();
}

void vfs_init(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mounts[i].in_use = 0;
    }
    for (int i = 0; i < VNODE_BUCKETS; i++) {
        buckets[i] = 0;
    }
    vnode_slab_init();  // Initialize vnode slab allocator
    serial_write_string("[vfs] initialized\n");
}

struct vnode *vnode_get(struct vfs_mount *m, uint64_t inode_id) {
    unsigned b = bucket_of(m, inode_id);
    for (struct vnode *vn = buckets[b]; vn; vn = vn->next) {
        if (vn->mount == m && vn->inode_id == inode_id) {
            vn->refcount++;
            return vn;
        }
    }

    // Allocate new vnode from slab pool
    struct vnode *slot = vnode_slab_alloc();
    if (!slot) {
        return 0; // pool exhausted or OOM -- caller reports -ENFILE
    }

    slot->mount = m;
    slot->inode_id = inode_id;
    slot->fs_private = 0;
    slot->refcount = 1;
    if (m->ops->read_inode(m, inode_id, slot) != 0) {
        slot->refcount = 0;
        slot->mount = 0;
        vnode_slab_free(slot);
        return 0;
    }

    slot->next = buckets[b];
    buckets[b] = slot;
    return slot;
}

void vnode_put(struct vnode *vn) {
    if (!vn || vn->refcount == 0) {
        return;
    }
    vn->refcount--;
    if (vn->refcount > 0) {
        return;
    }

    vn->mount->ops->sync_inode(vn);

    unsigned b = bucket_of(vn->mount, vn->inode_id);
    struct vnode **link = &buckets[b];
    while (*link && *link != vn) {
        link = &(*link)->next;
    }
    if (*link == vn) {
        *link = vn->next;
    }
    vn->next = 0;
    vn->mount = 0;
    vn->fs_private = 0;

    // Return to slab pool
    vnode_slab_free(vn);
}

// Returns the mount owning `path` (longest matching mount-point
// prefix) and points *out_rel at the path relative to that mount,
// always starting with '/'. Resolution is done once, up front: unlike
// real Unix, no per-directory mount check happens during the walk.
// Results are identical here, including correct shadowing -- a mount
// at /mnt hides any real /mnt directory on the root volume.
static struct vfs_mount *mount_for(const char *path, const char **out_rel) {
    struct vfs_mount *best = 0;
    uint64_t best_len = 0;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            continue;
        }
        uint64_t mlen = str_len(mounts[i].path);
        // "/" matches everything; any other prefix must be followed by
        // '/' or end-of-string so that /mnt does not match /mnttab.
        int prefix_ok = 1;
        for (uint64_t k = 0; k < mlen; k++) {
            if (path[k] != mounts[i].path[k]) { prefix_ok = 0; break; }
        }
        if (!prefix_ok) {
            continue;
        }
        if (mlen > 1 && path[mlen] != '/' && path[mlen] != '\0') {
            continue;
        }
        if (mlen >= best_len) {
            best = &mounts[i];
            best_len = mlen;
        }
    }

    if (!best) {
        return 0;
    }
    *out_rel = (best_len > 1) ? path + best_len : path;
    if ((*out_rel)[0] == '\0') {
        *out_rel = "/";
    }
    return best;
}

int vfs_mount_fs(const char *source, const char *target, const char *fstype) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].in_use && str_eq(mounts[i].path, target)) {
            return -EEXIST;
        }
    }

    struct vfs_mount *m = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) { m = &mounts[i]; break; }
    }
    if (!m) {
        return -ENOSPC;
    }

    if (str_eq(fstype, "ramfs")) {
        m->ops = &ramfs_ops;
    } else if (str_eq(fstype, "fat")) {
        m->ops = &fatfs_ops;
    } else if (str_eq(fstype, "devfs")) {
        m->ops = &devfs_ops;
    } else {
        return -ENODEV;
    }

    str_copy(m->path, target, VFS_MAX_PATH);
    m->fs_private = 0;
    m->root = 0;
    m->in_use = 1;

    int rc = m->ops->mount(m, source);
    if (rc != 0) {
        m->in_use = 0;
        return rc;
    }

    m->root = vnode_get(m, 0); // reserved id 0 is every driver's root
    if (!m->root) {
        m->ops->umount(m);
        m->in_use = 0;
        return -ENFILE;
    }

    serial_write_string("[vfs] mounted ");
    serial_write_string(fstype);
    serial_write_string(" at ");
    serial_write_string(target);
    serial_write_string("\n");
    return 0;
}

int vfs_umount(const char *target) {
    struct vfs_mount *m = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].in_use && str_eq(mounts[i].path, target)) {
            m = &mounts[i];
            break;
        }
    }
    if (!m) {
        return -ENOENT;
    }

    // The root vnode's own reference is ours, so anything above 1 --
    // or any OTHER vnode on this mount still held -- means live users.
    for (int b = 0; b < VNODE_BUCKETS; b++) {
        for (struct vnode *vn = buckets[b]; vn; vn = vn->next) {
            if (vn->mount != m) {
                continue;
            }
            if (vn == m->root) {
                if (vn->refcount > 1) { return -EBUSY; }
            } else if (vn->refcount > 0) {
                return -EBUSY;
            }
        }
    }

    vnode_put(m->root);
    m->root = 0;
    m->ops->umount(m);
    m->in_use = 0;
    return 0;
}

// Copies the next '/'-delimited component of *p into out and advances
// *p past it. Returns 0 when there are no components left.
static int next_component(const char **p, char *out) {
    const char *s = *p;
    while (*s == '/') s++;
    if (*s == '\0') { *p = s; return 0; }

    uint64_t i = 0;
    while (*s && *s != '/' && i < VFS_NAME_MAX - 1) {
        out[i++] = *s++;
    }
    out[i] = '\0';
    while (*s && *s != '/') s++; // discard anything over VFS_NAME_MAX-1
    *p = s;
    return 1;
}

// Shared by vfs_resolve and vfs_resolve_parent. `stop_short` means
// "return the parent of the last component instead of the component
// itself", and then the last component is copied into out_name.
static struct vnode *resolve_walk(const char *path, int stop_short,
                                  char *out_name, int *out_err) {
    const char *rel;
    struct vfs_mount *m = mount_for(path, &rel);
    if (!m) {
        *out_err = -ENOENT;
        return 0;
    }

    struct vnode *dir = m->root;
    dir->refcount++; // we hand back a reference the caller must put

    char name[VFS_NAME_MAX];
    const char *cursor = rel;
    while (next_component(&cursor, name)) {
        if (stop_short) {
            // Peek: if nothing follows, `name` is the final component
            // and `dir` is already the parent we want.
            const char *peek = cursor;
            char discard[VFS_NAME_MAX];
            if (!next_component(&peek, discard)) {
                if (out_name) { str_copy(out_name, name, VFS_NAME_MAX); }
                return dir;
            }
        }

        if (dir->type != VNODE_DIR) {
            vnode_put(dir);
            *out_err = -ENOTDIR;
            return 0;
        }

        uint64_t child_id;
        int rc = dir->mount->ops->lookup(dir, name, &child_id);
        if (rc != 0) {
            vnode_put(dir);
            *out_err = rc;
            return 0;
        }

        struct vnode *child = vnode_get(dir->mount, child_id);
        // Release the parent whether or not the child materialised --
        // a failed walk must leak no references.
        vnode_put(dir);
        if (!child) {
            *out_err = -ENFILE;
            return 0;
        }
        dir = child;
    }

    if (stop_short) {
        // Path was the mount root itself, e.g. "/" -- no final
        // component exists to create or unlink.
        vnode_put(dir);
        *out_err = -EINVAL;
        return 0;
    }
    return dir;
}

struct vnode *vfs_resolve(const char *path, int *out_err) {
    return resolve_walk(path, 0, 0, out_err);
}

struct vnode *vfs_resolve_parent(const char *path, char *out_name, int *out_err) {
    return resolve_walk(path, 1, out_name, out_err);
}

int vfs_open_into(const char *path, struct process *p, int fd, int writable) {
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) { return err; }

    if (!fd_table_put(p->fd_table, fd, vn, writable)) {
        vnode_put(vn);   // slot already taken, or out of memory
        return -EMFILE;
    }

    return 0;
}

void vfs_selftest(void) {
    int err = 0;

    char name[VFS_NAME_MAX];
    struct vnode *dir = vfs_resolve_parent("/tmp/T.TXT", name, &err);
    if (!dir) {
        serial_write_string("[vfs] selftest FAILED: resolve_parent /tmp/T.TXT\n");
        return;
    }
    uint64_t id;
    if (dir->mount->ops->create(dir, name, &id) != 0) {
        serial_write_string("[vfs] selftest FAILED: create\n");
        vnode_put(dir);
        return;
    }
    vnode_put(dir);

    struct vnode *a = vfs_resolve("/tmp/T.TXT", &err);
    if (!a) {
        serial_write_string("[vfs] selftest FAILED: resolve after create\n");
        return;
    }
    const char *msg = "vfs-ok";
    if (a->mount->ops->write(a, 0, msg, 6) != 6) {
        serial_write_string("[vfs] selftest FAILED: write\n");
        vnode_put(a);
        return;
    }

    // Second resolve of the same path MUST return the same vnode --
    // that is the whole point of the cache, and it is what makes the
    // write above visible here without reopening.
    struct vnode *b = vfs_resolve("/tmp/T.TXT", &err);
    if (b != a) {
        serial_write_string("[vfs] selftest FAILED: cache did not alias\n");
        vnode_put(a);
        if (b) { vnode_put(b); }
        return;
    }

    char buf[8] = {0};
    if (b->mount->ops->read(b, 0, buf, 6) != 6 ||
        buf[0] != 'v' || buf[5] != 'k') {
        serial_write_string("[vfs] selftest FAILED: readback mismatch\n");
        vnode_put(a); vnode_put(b);
        return;
    }

    if (vfs_umount("/tmp") != -EBUSY) {
        serial_write_string("[vfs] selftest FAILED: umount did not report busy\n");
        vnode_put(a); vnode_put(b);
        return;
    }

    // A failed walk must release every reference it took on the way
    // in. This is the milestone's most likely refcount bug, so it gets
    // a direct check rather than relying on the end-of-milestone leak
    // gate to notice it later.
    uint32_t before = vfs_vnode_in_use_count();
    int missing_err = 0;
    if (vfs_resolve("/tmp/NO/SUCH.TXT", &missing_err) != 0) {
        serial_write_string("[vfs] selftest FAILED: resolve of a missing path succeeded\n");
        vnode_put(a); vnode_put(b);
        return;
    }
    if (vfs_vnode_in_use_count() != before) {
        serial_write_string("[vfs] selftest FAILED: failed walk leaked a vnode\n");
        vnode_put(a); vnode_put(b);
        return;
    }

    vnode_put(a);
    vnode_put(b);

    struct vnode *root = vfs_resolve("/tmp", &err);
    if (!root) {
        serial_write_string("[vfs] selftest FAILED: resolve /tmp\n");
        return;
    }
    if (root->mount->ops->mkdir(root, "SUB") != 0) {
        serial_write_string("[vfs] selftest FAILED: mkdir\n");
        vnode_put(root);
        return;
    }

    // Walk the directory by ordinal until -ENOENT; we expect exactly
    // T.TXT (a file) and SUB (a directory), in either order.
    struct dirent de;
    int files = 0, dirs = 0;
    for (uint32_t i = 0; root->mount->ops->readdir(root, i, &de) == 0; i++) {
        if (de.type == DT_DIR) { dirs++; } else { files++; }
    }
    if (files != 1 || dirs != 1) {
        serial_write_string("[vfs] selftest FAILED: readdir count wrong\n");
        vnode_put(root);
        return;
    }

    if (root->mount->ops->unlink(root, "T.TXT") != 0) {
        serial_write_string("[vfs] selftest FAILED: unlink\n");
        vnode_put(root);
        return;
    }
    if (root->mount->ops->unlink(root, "SUB") != -EISDIR) {
        serial_write_string("[vfs] selftest FAILED: unlink of a dir should be EISDIR\n");
        vnode_put(root);
        return;
    }
    vnode_put(root);

    // Read a file that exists on the FAT16 root volume, proving the
    // driver works through the same interface ramfs just did.
    struct vnode *hello = vfs_resolve("/HELLO.TXT", &err);
    if (!hello) {
        serial_write_string("[vfs] selftest FAILED: resolve /HELLO.TXT\n");
        return;
    }
    char hbuf[16] = {0};
    if (hello->mount->ops->read(hello, 0, hbuf, 5) != 5 || hbuf[0] != 'H') {
        serial_write_string("[vfs] selftest FAILED: FAT read through VFS\n");
        vnode_put(hello);
        return;
    }
    vnode_put(hello);

    // And one in a subdirectory, proving multi-component resolution.
    struct vnode *nested = vfs_resolve("/DIR/NESTED.TXT", &err);
    if (!nested) {
        serial_write_string("[vfs] selftest FAILED: resolve /DIR/NESTED.TXT\n");
        return;
    }
    vnode_put(nested);

    // Read a file that exists only on the FAT32 volume. Reaching it
    // proves variant detection, 32-bit FAT entries, and the
    // cluster-chained root all work.
    struct vnode *f32 = vfs_resolve("/mnt/FAT32.TXT", &err);
    if (!f32) {
        serial_write_string("[vfs] selftest FAILED: resolve /mnt/FAT32.TXT\n");
        return;
    }
    char f32buf[8] = {0};
    if (f32->mount->ops->read(f32, 0, f32buf, 5) != 5 || f32buf[0] != 'H') {
        serial_write_string("[vfs] selftest FAILED: FAT32 read\n");
        vnode_put(f32);
        return;
    }
    vnode_put(f32);

    // And one in a FAT32 subdirectory.
    struct vnode *f32n = vfs_resolve("/mnt/SUB/F32NEST.TXT", &err);
    if (!f32n) {
        serial_write_string("[vfs] selftest FAILED: resolve /mnt/SUB/F32NEST.TXT\n");
        return;
    }
    vnode_put(f32n);

    serial_write_string("[vfs] selftest passed\n");
}
