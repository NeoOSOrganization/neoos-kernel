// The block-device registry: the one place that knows which disks and
// partitions exist. Controller drivers register whole disks; the
// partition scanner registers what it finds on them; everything above
// (blkcache, filesystems, devfs, /proc/partitions) works in terms of
// struct blockdev and never learns which controller is underneath.
#include "block/blockdev.h"
#include "block/part.h"
#include "block/ramblk.h"
#include "block/blkdev_file.h"
#include "fs/devfs.h"
#include "fs/blkcache.h"
#include "sync/lock.h"
#include "errno.h"
#include "drivers/char/serial.h"

static struct blockdev *table[BLOCKDEV_MAX];
static struct blockdev  part_pool[BLOCKDEV_MAX];
static uint8_t          part_used[BLOCKDEV_MAX];
static struct spinlock  reg_lock;
// Major 259 ("blkext"): partitions past the 15th of an sd disk, and
// NVMe, both take the next minor in registration order, as Linux does.
static uint32_t         blkext_next_minor;

static int name_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int name_len(const char *s) { int n = 0; while (s[n]) { n++; } return n; }

void blockdev_init(void) { spin_init(&reg_lock, LOCK_RANK_BLOCKDEV, "blockdev"); }

uint64_t blockdev_makedev(uint32_t major, uint32_t minor) {
    return (((uint64_t)major & 0xfffff000ULL) << 32) | (((uint64_t)major & 0xfffULL) << 8) |
           (((uint64_t)minor & 0xffffff00ULL) << 12) | ((uint64_t)minor & 0xffULL);
}

// Caller holds reg_lock.
static struct blockdev *find_locked(const char *name) {
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (table[i] && name_eq(table[i]->name, name)) { return table[i]; }
    }
    return 0;
}

// Caller holds reg_lock. Returns the slot index, -EEXIST or -ENOSPC.
static int insert_locked(struct blockdev *d) {
    if (find_locked(d->name)) { return -EEXIST; }
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (!table[i]) { table[i] = d; return i; }
    }
    return -ENOSPC;
}

static void publish(struct blockdev *d) {
    if (d->flags & BLOCKDEV_HIDDEN) { return; }
    int rc = devfs_register_blk(d->name, &blkdev_file_ops, d, blockdev_makedev(d->major, d->minor));
    if (rc != 0) {
        serial_write_string("[blockdev] ");
        serial_write_string(d->name);
        serial_write_string(": /dev node FAILED\n");
    }
}

int blockdev_register_disk(struct blockdev *d) {
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    int slot = insert_locked(d);
    if (slot < 0) { spin_unlock_irqrestore(&reg_lock, fl); return slot; }
    d->parent = 0; d->start_lba = 0; d->partno = 0;
    if (d->flags & BLOCKDEV_HIDDEN) {
        d->major = 0; d->minor = 0;
    } else if (d->name[0] == 's' && d->name[1] == 'd' && name_len(d->name) == 3) {
        d->major = 8; d->minor = (uint32_t)(d->name[2] - 'a') * 16;
    } else {
        d->major = 259; d->minor = blkext_next_minor++;
    }
    spin_unlock_irqrestore(&reg_lock, fl);

    publish(d);
    if (!(d->flags & BLOCKDEV_HIDDEN)) {
        serial_write_string("[blockdev] ");
        serial_write_string(d->name);
        serial_write_string(": sectors=");
        serial_write_hex64(d->sector_count);
        serial_write_string(" sector_size=");
        serial_write_hex64(d->sector_size);
        serial_write_string("\n");
    }
    part_scan(d);
    return 0;
}

static int part_read(struct blockdev *p, uint64_t lba, uint32_t count, void *buf) {
    return blockdev_read(p->parent, p->start_lba + lba, count, buf);
}
static int part_write(struct blockdev *p, uint64_t lba, uint32_t count, const void *buf) {
    return blockdev_write(p->parent, p->start_lba + lba, count, buf);
}
static int part_flush(struct blockdev *p) { return blockdev_flush(p->parent); }

static const struct blockdev_ops part_ops = { .read = part_read, .write = part_write, .flush = part_flush };

int blockdev_register_part(struct blockdev *disk, uint32_t partno, uint64_t start_lba,
                           uint64_t sector_count, const uint8_t type[16], const uint8_t uuid[16]) {
    if (partno == 0 || sector_count == 0 || start_lba >= disk->sector_count ||
        sector_count > disk->sector_count - start_lba) { return -EINVAL; }

    // Linux's rule: "sda" + 1 = "sda1", but "nvme0n1" + 1 = "nvme0n1p1".
    char name[BLOCKDEV_NAME_MAX];
    int n = name_len(disk->name), j = 0;
    for (; j < n; j++) { name[j] = disk->name[j]; }
    if (n > 0 && disk->name[n - 1] >= '0' && disk->name[n - 1] <= '9') { name[j++] = 'p'; }
    char num[11];
    int k = 0;
    uint32_t v = partno;
    do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    if (j + k >= BLOCKDEV_NAME_MAX) { return -ENAMETOOLONG; }
    while (k) { name[j++] = num[--k]; }
    name[j] = 0;

    uint64_t fl = spin_lock_irqsave(&reg_lock);
    int pi = -1;
    for (int i = 0; i < BLOCKDEV_MAX; i++) { if (!part_used[i]) { pi = i; break; } }
    if (pi < 0) { spin_unlock_irqrestore(&reg_lock, fl); return -ENOSPC; }
    struct blockdev *p = &part_pool[pi];
    for (uint64_t i = 0; i < sizeof *p; i++) { ((uint8_t *)p)[i] = 0; }
    for (int i = 0; i <= j; i++) { p->name[i] = name[i]; }
    p->sector_size = disk->sector_size;
    p->sector_count = sector_count;
    p->ops = &part_ops;
    p->parent = disk;
    p->start_lba = start_lba;
    p->partno = partno;
    p->flags = disk->flags;
    for (int i = 0; i < 16; i++) {
        p->part_type[i] = type ? type[i] : 0;
        p->part_uuid[i] = uuid ? uuid[i] : 0;
    }
    int slot = insert_locked(p);
    if (slot < 0) { spin_unlock_irqrestore(&reg_lock, fl); return slot; }
    part_used[pi] = 1;
    if (p->flags & BLOCKDEV_HIDDEN)           { p->major = 0;   p->minor = 0; }
    else if (disk->major == 8 && partno < 16) { p->major = 8;   p->minor = disk->minor + partno; }
    else                                      { p->major = 259; p->minor = blkext_next_minor++; }
    spin_unlock_irqrestore(&reg_lock, fl);

    publish(p);
    if (!(p->flags & BLOCKDEV_HIDDEN)) {
        serial_write_string("part: ");
        serial_write_string(p->name);
        serial_write_string(" start=");
        serial_write_hex64(start_lba);
        serial_write_string(" sectors=");
        serial_write_hex64(sector_count);
        serial_write_string("\n");
    }
    return 0;
}

void blockdev_unregister_disk(struct blockdev *d) {
    blkcache_invalidate(d);
    char gone[BLOCKDEV_MAX][BLOCKDEV_NAME_MAX];
    int ngone = 0;
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        struct blockdev *e = table[i];
        if (!e || (e != d && e->parent != d)) { continue; }
        if (!(e->flags & BLOCKDEV_HIDDEN)) {
            for (int c = 0; c < BLOCKDEV_NAME_MAX; c++) { gone[ngone][c] = e->name[c]; }
            ngone++;
        }
        table[i] = 0;
        if (e != d) { part_used[e - part_pool] = 0; }
    }
    spin_unlock_irqrestore(&reg_lock, fl);
    for (int i = 0; i < ngone; i++) { devfs_unregister(gone[i]); }
}

int blockdev_alloc_name(const char *prefix, char out[BLOCKDEV_NAME_MAX]) {
    if (!(prefix[0] == 's' && prefix[1] == 'd' && prefix[2] == 0)) { return -EINVAL; }
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    for (char c = 'a'; c <= 'z'; c++) {
        char cand[4] = { 's', 'd', c, 0 };
        if (!find_locked(cand)) {
            for (int i = 0; i < 4; i++) { out[i] = cand[i]; }
            spin_unlock_irqrestore(&reg_lock, fl);
            return 0;
        }
    }
    spin_unlock_irqrestore(&reg_lock, fl);
    return -ENOSPC;
}

struct blockdev *blockdev_find(const char *name) {
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    struct blockdev *d = find_locked(name);
    spin_unlock_irqrestore(&reg_lock, fl);
    return d;
}

struct blockdev *blockdev_find_path(const char *path) {
    if (!path) { return 0; }
    const char *p = path;
    if (p[0] == '/' && p[1] == 'd' && p[2] == 'e' && p[3] == 'v' && p[4] == '/') { p += 5; }
    return blockdev_find(p);
}

void blockdev_foreach(void (*cb)(struct blockdev *d, void *arg), void *arg) {
    // Snapshot under the lock, call back without it: callbacks format
    // text and may take other locks.
    struct blockdev *snap[BLOCKDEV_MAX];
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    for (int i = 0; i < BLOCKDEV_MAX; i++) { if (table[i]) { snap[n++] = table[i]; } }
    spin_unlock_irqrestore(&reg_lock, fl);
    for (int i = 0; i < n; i++) { cb(snap[i], arg); }
}

struct blockdev *blockdev_whole(struct blockdev *d) { return d->parent ? d->parent : d; }
uint64_t blockdev_disk_lba(struct blockdev *d, uint64_t lba) { return d->start_lba + lba; }

static int in_range(struct blockdev *d, uint64_t lba, uint32_t count) {
    return lba < d->sector_count && count <= d->sector_count - lba;
}

int blockdev_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) {
    if (count == 0) { return 0; }
    if (!in_range(d, lba, count)) { return -EIO; }
    return d->ops->read(d, lba, count, buf);
}

int blockdev_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) {
    if (count == 0) { return 0; }
    if (!in_range(d, lba, count)) { return -EIO; }
    return d->ops->write(d, lba, count, buf);
}

int blockdev_flush(struct blockdev *d) { return d->ops->flush(d); }

void blockdev_claim(struct blockdev *d) {
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    d->claims++;
    spin_unlock_irqrestore(&reg_lock, fl);
}

void blockdev_release(struct blockdev *d) {
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    if (d->claims) { d->claims--; }
    spin_unlock_irqrestore(&reg_lock, fl);
}

int blockdev_busy(struct blockdev *d) {
    struct blockdev *w = blockdev_whole(d);
    int busy = 0;
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    for (int i = 0; i < BLOCKDEV_MAX && !busy; i++) {
        struct blockdev *e = table[i];
        if (!e || !e->claims) { continue; }
        // The same device, or anything on the same disk when one side is
        // the whole disk. Two different partitions do not conflict.
        if (e == d || (blockdev_whole(e) == w && (e == w || d == w))) { busy = 1; }
    }
    spin_unlock_irqrestore(&reg_lock, fl);
    return busy;
}

void blockdev_selftest(void) {
    const char *why = 0;
    struct blockdev *a = ramblk_create("tsta", 512, 256);   // name ends in a letter
    struct blockdev *b = ramblk_create("tst0", 512, 256);   // name ends in a digit
    if (!a || !b) { serial_write_string("[blockdev] selftest FAILED: ramblk_create\n"); return; }
    static const uint8_t t[16] = { 0x83 };
    char nm[BLOCKDEV_NAME_MAX];

    if (blockdev_register_disk(a) || blockdev_register_disk(b))            { why = "register"; }
    else if (blockdev_register_disk(a) != -EEXIST)                         { why = "duplicate accepted"; }
    else if (blockdev_register_part(a, 1, 16, 32, t, t) != 0)              { why = "part a1"; }
    else if (blockdev_register_part(b, 1, 16, 32, t, t) != 0)              { why = "part b1"; }
    else if (!blockdev_find("tsta1"))                                      { why = "letter-suffix name"; }
    else if (!blockdev_find("tst0p1"))                                     { why = "digit-suffix p name"; }
    else if (blockdev_find_path("/dev/tsta1") != blockdev_find("tsta1"))  { why = "find_path /dev/"; }
    else if (blockdev_whole(blockdev_find("tsta1")) != a)                  { why = "whole"; }
    else if (blockdev_disk_lba(blockdev_find("tsta1"), 5) != 21)           { why = "disk lba"; }
    else if (blockdev_alloc_name("sd", nm) != 0 || nm[0] != 's' || nm[1] != 'd' ||
             nm[2] < 'a' || nm[2] > 'z' || nm[3] != 0 || blockdev_find(nm)) { why = "alloc_name"; }
    else {
        uint8_t buf[512];
        struct blockdev *p = blockdev_find("tsta1");
        ramblk_data(a)[21 * 512] = 0x5A;
        if (blockdev_read(p, 5, 1, buf) != 0 || buf[0] != 0x5A)            { why = "partition read offset"; }
        else if (blockdev_read(p, 31, 1, buf) != 0)                        { why = "last sector refused"; }
        else if (blockdev_read(p, 32, 1, buf) != -EIO)                     { why = "read past partition end"; }
        else if (blockdev_read(p, 31, 2, buf) != -EIO)                     { why = "read straddling end"; }
        else if (blockdev_read(a, 256, 1, buf) != -EIO)                    { why = "read past disk end"; }
        else if (blockdev_register_part(a, 2, 250, 32, t, t) != -EINVAL)   { why = "part past disk accepted"; }
    }
    ramblk_destroy(a);
    ramblk_destroy(b);
    if (!why && (blockdev_find("tsta") || blockdev_find("tsta1")))         { why = "unregister left entries"; }
    if (why) {
        serial_write_string("[blockdev] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[blockdev] selftest passed\n");
}
