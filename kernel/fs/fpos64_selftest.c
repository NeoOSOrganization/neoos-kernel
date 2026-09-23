// fpos64_selftest -- file positions and sizes are 64-bit end to end.
// A position of 5 GiB must survive lseek and a failed write intact;
// before storage-01 it was stored in a uint32_t and came back as 1 GiB.
#include "fs/vfs.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "errno.h"
#include "drivers/char/serial.h"
#include "block/blockdev.h"
#include "block/ramblk.h"
#include "block/blkdev_file.h"

#define FIVE_GIB (5ULL << 30)

// The block-node half: an opened /dev node on a 64 KiB, 4 KiB-sector
// RAM disk -- device-size seeks, EOF and ENOSPC at the end, and a write
// that straddles a sector boundary landing on the device.
static const char *blk_half(void) {
    struct blockdev *d = ramblk_create("fpb", 4096, 16);
    if (!d) { return "ramblk"; }
    blockdev_register_disk(d);
    struct file_descriptor f = {0};
    f.in_use = 1; f.ops = &blkdev_file_ops; f.priv = d; f.readable = 1; f.writable = 1;
    const int64_t size = 16 * 4096;
    const char *why = 0;
    char msg[10] = { 'b','o','u','n','d','a','r','y','!','!' };
    char back[10];
    if (file_lseek(&f, 0, 2 /* SEEK_END */) != size)                  { why = "SEEK_END != size"; }
    else if (file_read(&f, back, 1) != 0)                             { why = "read at end not 0"; }
    else if (file_write(&f, msg, 1) != -ENOSPC)                       { why = "write at end not ENOSPC"; }
    else if (file_lseek(&f, size + 1, 0) != -EINVAL)                  { why = "seek past end not EINVAL"; }
    else if (file_lseek(&f, 4090, 0) != 4090)                         { why = "seek 4090"; }
    else if (file_write(&f, msg, 10) != 10)                           { why = "write across sector"; }
    else if (f.position != 4100)                                      { why = "position after write"; }
    else if (file_lseek(&f, 4090, 0) != 4090 || file_read(&f, back, 10) != 10) { why = "read back"; }
    else {
        for (int i = 0; i < 10; i++) { if (back[i] != msg[i]) { why = "data mismatch"; break; } }
        if (!why && (ramblk_data(d)[4095] != 'a' || ramblk_data(d)[4096] != 'r')) { why = "bytes not on device"; }
        if (!why && (file_lseek(&f, size - 4, 0) != size - 4 || file_write(&f, msg, 10) != 4)) { why = "write crossing end not short"; }
    }
    ramblk_destroy(d);
    return why;
}

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
    if (!why) { why = blk_half(); }
    if (why) {
        serial_write_string("[fpos64] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[fpos64] selftest passed\n");
}
