// A RAM-backed block device for selftests. Always HIDDEN: no /dev node,
// not in /proc/partitions, never a candidate for anything but the test
// that made it.
#include "block/ramblk.h"
#include "block/blockdev.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "errno.h"

struct ramblk {
    struct blockdev bdev;       // first: a blockdev * is a ramblk *
    uint64_t phys;
    unsigned order;
    uint8_t *data;
    uint64_t reads;
};

static void copy(uint8_t *d, const uint8_t *s, uint64_t n) { for (uint64_t i = 0; i < n; i++) { d[i] = s[i]; } }

static int rb_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) {
    struct ramblk *r = (struct ramblk *)d;
    r->reads++;
    copy((uint8_t *)buf, r->data + lba * d->sector_size, (uint64_t)count * d->sector_size);
    return 0;
}

static int rb_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) {
    struct ramblk *r = (struct ramblk *)d;
    copy(r->data + lba * d->sector_size, (const uint8_t *)buf, (uint64_t)count * d->sector_size);
    return 0;
}

static int rb_flush(struct blockdev *d) { (void)d; return 0; }

static const struct blockdev_ops rb_ops = { .read = rb_read, .write = rb_write, .flush = rb_flush };

struct blockdev *ramblk_create(const char *name, uint32_t sector_size, uint64_t sectors) {
    uint64_t bytes = (uint64_t)sector_size * sectors;
    unsigned order = 0;
    while (((uint64_t)PMM_FRAME_SIZE << order) < bytes) { order++; }
    if (order > PMM_MAX_ORDER) { return 0; }
    struct ramblk *r = (struct ramblk *)kmalloc(sizeof *r);
    if (!r) { return 0; }
    for (uint64_t i = 0; i < sizeof *r; i++) { ((uint8_t *)r)[i] = 0; }
    r->phys = pmm_alloc(order);
    if (!r->phys) { kfree(r); return 0; }
    r->order = order;
    r->data = (uint8_t *)phys_to_virt(r->phys);
    for (uint64_t i = 0; i < ((uint64_t)PMM_FRAME_SIZE << order); i++) { r->data[i] = 0; }
    int j = 0;
    while (name[j] && j < BLOCKDEV_NAME_MAX - 1) { r->bdev.name[j] = name[j]; j++; }
    r->bdev.sector_size = sector_size;
    r->bdev.sector_count = sectors;
    r->bdev.ops = &rb_ops;
    r->bdev.flags = BLOCKDEV_HIDDEN;
    return &r->bdev;
}

void ramblk_destroy(struct blockdev *d) {
    if (!d) { return; }
    struct ramblk *r = (struct ramblk *)d;
    blockdev_unregister_disk(d);    // no-op if it was never registered
    pmm_free(r->phys, r->order);
    kfree(r);
}

uint8_t *ramblk_data(struct blockdev *d) { return ((struct ramblk *)d)->data; }
uint64_t ramblk_reads(struct blockdev *d) { return ((struct ramblk *)d)->reads; }
