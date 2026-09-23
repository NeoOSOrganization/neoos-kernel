// fpos64_selftest -- file positions and sizes are 64-bit end to end.
// A position of 5 GiB must survive lseek and a failed write intact;
// before storage-01 it was stored in a uint32_t and came back as 1 GiB.
#include "fs/vfs.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "errno.h"
#include "drivers/char/serial.h"

#define FIVE_GIB (5ULL << 30)

void fpos64_selftest(void) {
    const char *why = 0;
    char name[VFS_NAME_MAX];
    int err = 0;
    vfs_lock();
    struct vnode *dir = vfs_resolve_parent("/tmp/.fpos64", name, &err);
    if (!dir) { vfs_unlock(); serial_write_string("[fpos64] selftest FAILED: resolve /tmp\n"); return; }
    uint64_t id = 0;
    if (dir->mount->ops->create(dir, name, &id) != 0) {
        vnode_put(dir); vfs_unlock();
        serial_write_string("[fpos64] selftest FAILED: create\n"); return;
    }
    struct vnode *vn = vnode_get(dir->mount, id);
    vfs_unlock();
    struct file_descriptor f = {0};
    if (!vn) { why = "vnode_get"; goto out_dir; }

    f.in_use = 1; f.vn = vn; f.readable = 1; f.writable = 1;
    file_bind_vnode_ops(&f);

    if (file_lseek(&f, (int64_t)FIVE_GIB, 0 /* SEEK_SET */) != (int64_t)FIVE_GIB) { why = "lseek result"; }
    else if (f.position != FIVE_GIB)                                              { why = "position truncated"; }
    else if (file_lseek(&f, 0, 1 /* SEEK_CUR */) != (int64_t)FIVE_GIB)            { why = "SEEK_CUR"; }
    else if (file_write(&f, "x", 1) != -EFBIG)                                    { why = "write past ramfs cap not EFBIG"; }
    else if (f.position != FIVE_GIB)                                              { why = "failed write moved position"; }

    vnode_put(vn);
out_dir:
    vfs_lock();
    dir->mount->ops->unlink(dir, name);
    vnode_put(dir);
    vfs_unlock();
    // Task 5 inserts the block-node half here: if (!why) { why = blk_half(); }
    if (why) {
        serial_write_string("[fpos64] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[fpos64] selftest passed\n");
}
