// File operations for an opened /dev/<block device>. Byte-granular,
// like Linux's buffered block devices: whole aligned sectors go
// straight to the device; a partial head or tail sector is
// read-modified-written through the cache.
#include "block/blkdev_file.h"
#include "block/blockdev.h"
#include "fs/blkcache.h"
#include "fs/vfs.h"
#include "sync/poll_head.h"
#include "sched/proc.h"
#include "mm/uaccess.h"
#include "errno.h"

// Linux's values (<linux/fs.h>).
#define BLKGETSIZE    0x1260
#define BLKSSZGET     0x1268
#define BLKGETSIZE64  0x80081272

// Most sectors moved by one multi-sector command.
#define XFER_MAX_SECTORS 256

// One bounce sector for the unaligned edges. Guarded by fs_lock, which
// every transfer below holds.
static uint8_t edge[BLKCACHE_MAX_SECTOR];

static struct blockdev *bd(struct file_descriptor *f) { return (struct blockdev *)f->priv; }
static uint64_t dev_bytes(struct blockdev *d) { return d->sector_count * d->sector_size; }

static void cp(uint8_t *d, const uint8_t *s, uint64_t n) { for (uint64_t i = 0; i < n; i++) { d[i] = s[i]; } }

static int64_t xfer(struct file_descriptor *f, uint8_t *buf, uint64_t len, int wr) {
    struct blockdev *d = bd(f);
    uint64_t size = dev_bytes(d), pos = f->position;
    if (pos >= size) { return wr ? -ENOSPC : 0; }
    if (len > size - pos) { len = size - pos; }
    uint32_t ss = d->sector_size;
    uint64_t done = 0;
    int err = 0;

    vfs_lock();
    while (done < len) {
        uint64_t off = pos + done, lba = off / ss, left = len - done;
        uint32_t in = (uint32_t)(off % ss);
        if (in == 0 && left >= ss) {
            uint64_t n = left / ss;
            if (n > XFER_MAX_SECTORS) { n = XFER_MAX_SECTORS; }
            err = wr ? blkcache_write_multi(d, lba, (uint32_t)n, buf + done)
                     : blkcache_read_multi(d, lba, (uint32_t)n, buf + done);
            if (err) { break; }
            done += n * ss;
            continue;
        }
        uint64_t chunk = ss - in;
        if (chunk > left) { chunk = left; }
        err = blkcache_read(d, lba, edge);
        if (err) { break; }
        if (wr) {
            cp(edge + in, buf + done, chunk);
            err = blkcache_write(d, lba, edge);
            if (err) { break; }
        } else {
            cp(buf + done, edge + in, chunk);
        }
        done += chunk;
    }
    vfs_unlock();

    if (done == 0 && err) { return err; }
    f->position += done;
    return (int64_t)done;
}

static int64_t b_read(struct file_descriptor *f, void *buf, uint64_t len) {
    return xfer(f, (uint8_t *)buf, len, 0);
}

static int64_t b_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    return xfer(f, (uint8_t *)buf, len, 1);
}

// Linux's blkdev_llseek is fixed_size_llseek: the device size is a hard
// ceiling, and a position beyond it is EINVAL rather than a hole.
static int64_t b_lseek(struct file_descriptor *f, int64_t off, int whence) {
    int64_t size = (int64_t)dev_bytes(bd(f)), base;
    if (whence == 0)      { base = 0; }
    else if (whence == 1) { base = (int64_t)f->position; }
    else if (whence == 2) { base = size; }
    else                  { return -EINVAL; }
    int64_t pos = base + off;
    if (pos < 0 || pos > size) { return -EINVAL; }
    f->position = (uint64_t)pos;
    return pos;
}

static int64_t b_ioctl(struct file_descriptor *f, uint64_t req, void *arg) {
    struct blockdev *d = bd(f);
    if (req == BLKGETSIZE64) {
        uint64_t v = dev_bytes(d);
        return copy_to_user(arg, &v, sizeof v) ? -EFAULT : 0;
    }
    if (req == BLKSSZGET) {
        int v = (int)d->sector_size;
        return copy_to_user(arg, &v, sizeof v) ? -EFAULT : 0;
    }
    if (req == BLKGETSIZE) {
        unsigned long v = (unsigned long)(dev_bytes(d) / 512);
        return copy_to_user(arg, &v, sizeof v) ? -EFAULT : 0;
    }
    return -ENOTTY;
}

static int     b_fsync(struct file_descriptor *f) { return blockdev_flush(bd(f)); }
static int64_t b_getdents(struct file_descriptor *f, void *b, int n) { (void)f; (void)b; (void)n; return -ENOTDIR; }
static int     b_poll(struct file_descriptor *f, int ev) { (void)f; return ev; }
static struct poll_head *b_poll_head(struct file_descriptor *f) { (void)f; return poll_head_always_ready(); }
static void    b_dup(struct file_descriptor *f) { (void)f; }
static void    b_close(struct file_descriptor *f) { (void)f; }

const struct file_ops blkdev_file_ops = {
    .name      = "blkdev",
    .read      = b_read,
    .write     = b_write,
    .lseek     = b_lseek,
    .getdents  = b_getdents,
    .ioctl     = b_ioctl,
    .poll      = b_poll,
    .poll_head = b_poll_head,
    .dup       = b_dup,
    .close     = b_close,
    .fsync     = b_fsync,
};
