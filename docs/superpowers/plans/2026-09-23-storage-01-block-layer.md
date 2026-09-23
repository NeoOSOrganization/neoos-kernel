# storage-01: Block Layer, Partitions, 64-bit File Positions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put a Linux-shaped block-device layer (registry, GPT/MBR partitions, `/dev/sdX` raw nodes, `/proc/partitions`) between the filesystems and the ATA driver, and make every file position and size 64-bit.

**Architecture:** A new `kernel/block/` directory holds the registry (`blockdev.c`), the partition scanner (`part.c`), a RAM-backed test device (`ramblk.c`) and the raw-node file ops (`blkdev_file.c`). The ATA driver becomes the first blockdev driver; `blkcache` is re-keyed by `(whole-disk blockdev, disk LBA)`; FAT resolves its mount source to a blockdev. devfs gains block nodes through its existing dynamic-entry table.

**Tech Stack:** freestanding C (gnu11, `-mcmodel=kernel`), x86_64, QEMU `pc` machine, musl-based userland built with the hosted `x86_64-neoos-linux-musl` gcc.

**Spec:** `docs/superpowers/specs/2026-09-23-storage-01-block-layer-design.md` (roadmap: `docs/superpowers/specs/2026-09-23-storage-00-roadmap.md`). Read both before starting.

## Global Constraints

- No host-runnable unit tests exist: every test is a kernel boot selftest printing `... selftest passed` or a line containing `FAILED`, or a userland program printing `PASS <name>`. `make test` fails on any `FAILED`/`PANIC` line in `build/serial.log` and on any missing `CORE_REQUIRED_MARKERS` entry.
- Never run two `make` invocations concurrently; the gauntlet (`tools/gauntlet.sh`) needs an exclusive build and dies silently otherwise.
- Work directly on `main` (repo convention). Commit after each task; end every commit message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Device names, majors/minors, `S_IFBLK` (`0060000`), ioctl numbers (`BLKGETSIZE64 = 0x80081272`, `BLKSSZGET = 0x1268`, `BLKGETSIZE = 0x1260`), `/proc/partitions` format and errno choices are ABI: use Linux's values exactly.
- `sd` major is 8 (minor = disk_index * 16 + partition, partitions 1–15); partitions ≥ 16 and NVMe use major 259 with minors allocated in registration order.
- Lock ranks: blockdev registry and blkcache locks are `LOCK_RANK_BLOCKDEV` (7); driver locks `LOCK_RANK_DRIVER` (8); devfs dynamic lock is `LOCK_RANK_DEVFS` (253). Never hold the registry lock across I/O or across `devfs_register*`. Run `make lock-check` after adding a lock.
- Kernel code has no libc: use the file-local copy/compare helpers each `.c` file already defines (or add a file-local one); `kmalloc`/`kfree` (`mm/heap.h`), `pmm_alloc(order)`/`pmm_free(phys, order)` + `phys_to_virt()` (`mm/pmm.h`, `mm/paging.h`; `PMM_MAX_ORDER` is 14).
- Every user-visible change is documented in `docs/stdlib.md`; deliberate divergences are recorded there with their reason.

## Review Focus

1. **Empty IDE slot at boot.** When a drive is absent (true once storage-02 moves disks to AHCI), `ata_probe` must log without the word `FAILED`, or `make test` breaks. Task 4 rewords the "no drive present" line and its selftest-free boot path is checked by grepping the log.
2. **Aliasing between a partition and its disk.** A write through `/dev/sda` must be visible to a read through `/dev/sda1` of the same sector, including after a `_multi` write. Task 4's blkcache selftest pins it.
3. **Raw node I/O at the edges.** Writes that cross a sector boundary, reads at exactly the device end (return 0), writes at the end (`-ENOSPC`), `lseek` past the end (`-EINVAL`). Task 5's fpos64 extension and Task 7's userland test pin these.
4. **Values above 4 GiB wrapping.** A 5 GiB `lseek` must read back as 5 GiB from userland, not 1 GiB; `pread`/`pwrite` offsets must not be cast to 32-bit. Task 1 (kernel) and Task 7 (userland through musl) pin it.
5. **Corrupt partition tables.** A GPT with a bad primary header, a protective MBR with no valid GPT, overlapping or out-of-range entries must never register a partition that extends past the disk. Task 3's selftest pins each.

---

## File Structure

| file | status | responsibility |
|---|---|---|
| `kernel/block/blockdev.h`, `blockdev.c` | new | registry, naming, major/minor, bounds-checked `blockdev_read/write/flush`, partition blockdevs, `blockdev_selftest` |
| `kernel/block/part.c`, `part.h` | new | GPT/MBR scanner, `part_selftest` |
| `kernel/block/ramblk.c`, `ramblk.h` | new | RAM-backed hidden blockdev for selftests |
| `kernel/block/blkdev_file.c`, `blkdev_file.h` | new | `file_ops` for opened `/dev/sdX` nodes |
| `kernel/lib/crc32.c`, `crc32.h` | new | CRC-32 (reflected, `0xEDB88320`) |
| `kernel/fs/fpos64_selftest.c` | new | 64-bit position selftest |
| `kernel/fs/blkcache.c`, `.h` | modify | re-key by blockdev, 4 KiB entries, `_multi`, hermetic selftest |
| `kernel/drivers/block/ata.c`, `.h` | modify | `ata_probe()`, blockdev ops, quiet absence |
| `kernel/fs/fatfs.c`, `.h` | modify | `struct blockdev *bdev` instead of `drive`, source resolution, `-EFBIG` |
| `kernel/fs/devfs.c`, `.h` | modify | `devfs_register_blk`, root-level dynamic entries, `rdev` |
| `kernel/fs/vfs.h`, `vfs.c`, `stat.h`, `file.c`, `file.h` | modify | `VNODE_BLOCK`, `S_IFBLK`, 64-bit `size`/`pos`, `fsync` hook |
| `kernel/fs/ramfs.c`, `embedfs.c`, `procfs.c` | modify | 64-bit `pos`; procfs `/proc/partitions` |
| `kernel/sched/proc.h`, `proc.c`, `kernel/mm/vma.c`, `kernel/ipc/memfd.c`, `kernel/drivers/video/vesafb.c`, `kernel/syscall/sys_file.c` | modify | 64-bit positions, pread/pwrite staging, fsync hook, mount source length |
| `kernel/kernel.c` | modify | boot order, `/dev/sda` mounts, new selftests |
| `userland/blkdevtest.c` | new | userland raw-device test |
| `Makefile` | modify | `kernel/block` in `KERNEL_DIRS`, markers, `blkdevtest` target |
| `docs/stdlib.md`, `docs/abi-compatibility.md` | modify | ABI docs |

---

### Task 1: 64-bit file positions and sizes

**Files:**
- Modify: `kernel/sched/proc.h:46`, `kernel/fs/vfs.h:72,95-96`, `kernel/fs/file.c:22-54,90`, `kernel/ipc/memfd.c:79-122`, `kernel/drivers/video/vesafb.c:271-296`, `kernel/syscall/sys_file.c:830-860`, `kernel/fs/ramfs.c:10,89-150,182-204`, `kernel/fs/embedfs.c:70-86`, `kernel/fs/procfs.c:391-425`, `kernel/fs/devfs.c:461-469`, `kernel/fs/fatfs.c:2143-2175`, `kernel/mm/vma.c:474`, `kernel/sched/proc.c:238,291`
- Create: `kernel/fs/fpos64_selftest.c`
- Modify: `kernel/kernel.c` (call site), `Makefile` (`CORE_REQUIRED_MARKERS`)

**Interfaces:**
- Produces: `struct file_descriptor.position` is `uint64_t`; `struct vnode.size` is `uint64_t`; `vfs_ops.read/write` are `int64_t (*)(struct vnode *, uint64_t pos, void *buf, uint32_t len)` (write: `const void *`); `void fpos64_selftest(void);` in `kernel/fs/fpos64_selftest.c`, declared in `kernel/fs/vfs.h`.

- [ ] **Step 1: Write the failing selftest**

Create `kernel/fs/fpos64_selftest.c`:

```c
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
```

Declare in `kernel/fs/vfs.h` next to `void vfs_selftest(void);`:

```c
void fpos64_selftest(void);   // kernel/fs/fpos64_selftest.c
```

Call it in `kernel/kernel.c` directly after `devfs_selftest();`:

```c
    fpos64_selftest();
```

Add `"[fpos64] selftest passed" \` to `CORE_REQUIRED_MARKERS` in `Makefile` (after `"[devfs] selftest passed" \`).

If `file_bind_vnode_ops` is not declared in `kernel/fs/file.h`, add `int64_t file_bind_vnode_ops(struct file_descriptor *f);` there.

- [ ] **Step 2: Run it to verify it fails**

Run: `make test 2>&1 | tail -5`
Expected: FAIL — `[fpos64] selftest FAILED: position truncated` (the `uint32_t` field stores 5 GiB as 1 GiB) or `lseek result`.

- [ ] **Step 3: Widen the descriptor position**

`kernel/sched/proc.h:46`:

```c
    uint64_t position;  // per-fd, NOT shared across fork -- see docs/stdlib.md
```

`kernel/fs/file.c` — remove every `(uint32_t)` cast applied to a position:

```c
    int64_t n = f->vn->mount->ops->read(f->vn, f->position, buf, (uint32_t)len);
    if (n > 0) { f->position += (uint64_t)n; }
```
(same for `vnode_write`), and in `vnode_lseek`:

```c
    f->position = (uint64_t)pos;
```

In `vnode_getdents`, the readdir index stays 32-bit: `readdir(f->vn, (uint32_t)f->position, &de)`.

`kernel/ipc/memfd.c:91,111,122` and `kernel/drivers/video/vesafb.c:275,286,296`: replace `(uint32_t)` casts on assignments to `f->position` with `(uint64_t)`.

- [ ] **Step 4: Widen vnode size and the vfs_ops position**

`kernel/fs/vfs.h:72`: `uint64_t          size;`
`kernel/fs/vfs.h:95-96`:

```c
    int64_t (*read)(struct vnode *vn, uint64_t pos, void *buf, uint32_t len);
    int64_t (*write)(struct vnode *vn, uint64_t pos, const void *buf, uint32_t len);
```

Update every implementation's signature to `uint64_t pos`: `devfs_read/devfs_write`, `embedfs_read/embedfs_write`, `procfs_read/procfs_write`, `ramfs_read/ramfs_write`, `fatfs_read/fatfs_write`. Bodies:

`embedfs_read`:
```c
    uint64_t size = (uint64_t)((const char *)e->end - (const char *)e->data);
    if (pos >= size) { return 0; }
    if (pos + len > size) { len = (uint32_t)(size - pos); }
```

`procfs_read` (both branches): compare in 64-bit — `if (pos >= (uint64_t)n) { return 0; } uint32_t k = (uint32_t)((uint64_t)n - pos);` and likewise for `f.len`.

`ramfs_read`/`ramfs_write`: the cap check comes first and returns `-EFBIG` (a file-size limit, not a space shortage), then everything below is 32-bit-safe:

```c
static int64_t ramfs_read(struct vnode *vn, uint64_t pos, void *buf, uint32_t len) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (pos >= n->size) { return 0; }
    if (pos + len > n->size) { len = (uint32_t)(n->size - pos); }
    /* ... existing loop, with `uint64_t off = pos + done;` and
       `uint32_t page = (uint32_t)(off / PMM_FRAME_SIZE);` ... */
}

static int64_t ramfs_write(struct vnode *vn, uint64_t pos, const void *buf, uint32_t len) {
    struct ramfs_node *n = (struct ramfs_node *)vn->fs_private;
    if (pos + len > (uint64_t)RAMFS_MAX_PAGES * PMM_FRAME_SIZE) { return -EFBIG; }
    /* ... existing loop, same `off`/`page` changes ... */
}
```
Change `struct ramfs_node.size` to `uint64_t` and drop the `(uint32_t)` casts in `ramfs_truncate_to`.

`fatfs_read`/`fatfs_write` — FAT's on-disk size is 32-bit; keep internals 32-bit and refuse beyond:

```c
static int64_t fatfs_read(struct vnode *vn, uint64_t pos, void *buf, uint32_t len) {
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;
    if (pos >= vn->size) { return 0; }
    if (pos + len > vn->size) { len = (uint32_t)(vn->size - pos); }
    uint32_t got = fat16_read_at_v(v, n->first_cluster, (uint32_t)pos, buf, len);
    if (got == 0 && len > 0) { return -EIO; }
    return (int64_t)got;
}

static int64_t fatfs_write(struct vnode *vn, uint64_t pos, const void *buf, uint32_t len) {
    // FAT stores a file's size in 32 bits: 4 GiB - 1 is the largest file
    // it can describe. Linux's vfat answers EFBIG past it; so does this.
    if (pos + (uint64_t)len > 0xFFFFFFFFULL) { return -EFBIG; }
    /* ... existing body, passing `(uint32_t)vn->size` and `(uint32_t)pos` ... */
}
```

- [ ] **Step 5: Fix the direct callers and pread/pwrite**

`kernel/mm/vma.c:474`: `vn->mount->ops->read(vn, off, dst, PMM_FRAME_SIZE);` (drop the cast; `off` is already 64-bit — check its declaration and make it `uint64_t` if not).

`kernel/sched/proc.c:238` and `:291`: before `uint32_t size = vn->size;` add a guard using that function's existing failure path (the same one taken when the read of the image fails):

```c
    if (vn->size > 0xFFFFFFFFULL) { /* same cleanup + error return as a failed image read */ }
```

`kernel/syscall/sys_file.c` `sys_pread`/`sys_pwrite`: stage through the existing helpers (they bounce through a kernel buffer; passing the user pointer straight to `file_read` let a filesystem write user memory without the fault-tolerant copy), and keep the offset 64-bit:

```c
int64_t sys_pread(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    if ((int64_t)a->a4 < 0) { return -EINVAL; }
    if (!f->vn) { return -ESPIPE; }          // pipe, socket, tty: no position
    if (f->vn->type == VNODE_DIR) { return -EISDIR; }

    uint64_t saved = f->position;
    f->position = (uint64_t)a->a4;
    int64_t rc = read_to_user(f, (uint64_t)a->a2, (uint64_t)a->a3);
    f->position = saved;
    return rc;
}
```
and the same shape for `sys_pwrite` with `write_from_user`.

Search for any remaining narrowing: `grep -n "(uint32_t)f->position\|uint32_t saved\|(uint32_t)vn->size\|(uint32_t)off" -r kernel` must print nothing that stores a file offset.

- [ ] **Step 6: Build and run the test**

Run: `make test 2>&1 | tail -5`
Expected: `PASS: no FAILED lines, boot reached the scheduler, all suites reported`, and `grep fpos64 build/serial.log` shows `[fpos64] selftest passed`.

- [ ] **Step 7: Commit**

```bash
git add -A kernel Makefile
git commit -m "feat: 64-bit file positions and vnode sizes (storage-01 task 1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Block device registry, CRC-32, RAM test device

**Files:**
- Create: `kernel/block/blockdev.h`, `kernel/block/blockdev.c`, `kernel/block/ramblk.h`, `kernel/block/ramblk.c`, `kernel/lib/crc32.h`, `kernel/lib/crc32.c`, `kernel/block/part.h` (stub `part_scan` only, filled in Task 3)
- Modify: `Makefile:54-58` (`KERNEL_DIRS` += `kernel/block`), `Makefile` markers, `kernel/kernel.c`, `kernel/fs/devfs.h/.c` (block registration hook, minimal)

**Interfaces:**
- Produces (exact, later tasks depend on these):

```c
// kernel/block/blockdev.h
#define BLOCKDEV_MAX       32
#define BLOCKDEV_NAME_MAX  16
#define BLOCKDEV_HIDDEN    0x1u
struct blockdev;
struct blockdev_ops {
    int (*read)(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blockdev *d);
};
struct blockdev {
    char     name[BLOCKDEV_NAME_MAX];
    uint32_t sector_size;
    uint64_t sector_count;
    const struct blockdev_ops *ops;
    void    *priv;
    struct blockdev *parent;
    uint64_t start_lba;
    uint32_t major, minor;
    uint8_t  part_type[16];
    uint8_t  part_uuid[16];
    uint32_t flags;
    uint32_t partno;          // 0 for a whole disk
};
void blockdev_init(void);
int  blockdev_register_disk(struct blockdev *d);           // 0 / -EEXIST / -ENOSPC
void blockdev_unregister_disk(struct blockdev *d);         // also removes its partitions
int  blockdev_register_part(struct blockdev *disk, uint32_t partno,
                            uint64_t start_lba, uint64_t sector_count,
                            const uint8_t type[16], const uint8_t uuid[16]);
int  blockdev_alloc_name(const char *prefix, char out[BLOCKDEV_NAME_MAX]);  // "sd" only
struct blockdev *blockdev_find(const char *name);
struct blockdev *blockdev_find_path(const char *path);     // "/dev/sda1" or "sda1"
void blockdev_foreach(void (*cb)(struct blockdev *d, void *arg), void *arg);
struct blockdev *blockdev_whole(struct blockdev *d);
uint64_t blockdev_disk_lba(struct blockdev *d, uint64_t lba);
int  blockdev_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
int  blockdev_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
int  blockdev_flush(struct blockdev *d);
uint64_t blockdev_makedev(uint32_t major, uint32_t minor);  // Linux encoding
void blockdev_selftest(void);

// kernel/block/ramblk.h
struct blockdev *ramblk_create(const char *name, uint32_t sector_size, uint64_t sectors);
void     ramblk_destroy(struct blockdev *d);     // unregisters first if registered
uint8_t *ramblk_data(struct blockdev *d);
uint64_t ramblk_reads(struct blockdev *d);       // device-level read commands so far

// kernel/lib/crc32.h
uint32_t crc32(uint32_t crc, const void *buf, uint64_t len);  // crc32(0, "123456789", 9) == 0xCBF43926

// kernel/block/part.h
void part_scan(struct blockdev *disk);
void part_selftest(void);

// kernel/fs/devfs.h (added here; Task 5 gives it file ops)
int devfs_register_blk(const char *name, const struct file_ops *ops, void *priv, uint64_t rdev);
```

- [ ] **Step 1: Write the failing selftest**

In `kernel/block/blockdev.c`, write `blockdev_selftest` first (the rest of the file follows in Step 3):

```c
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
```

Add to `kernel/kernel.c` right after `blkcache_selftest();`:

```c
    blockdev_init();
    blockdev_selftest();
```
Add `#include "block/blockdev.h"` at the top. Add `"[blockdev] selftest passed" \` to `CORE_REQUIRED_MARKERS`. Add `kernel/block` to `KERNEL_DIRS`.

- [ ] **Step 2: Run it to verify it fails**

Run: `make build 2>&1 | tail -5`
Expected: FAIL to link — `undefined reference to 'ramblk_create'` (and the other functions).

- [ ] **Step 3: Implement crc32, ramblk and the registry**

`kernel/lib/crc32.c`:

```c
#include "lib/crc32.h"

static uint32_t table[256];
static int ready;

static void build(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) { c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
        table[i] = c;
    }
    ready = 1;
}

// Standard CRC-32 (IEEE 802.3, reflected), as GPT uses. Pass 0 to
// start; pass a previous result to continue over more bytes.
uint32_t crc32(uint32_t crc, const void *buf, uint64_t len) {
    if (!ready) { build(); }
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (uint64_t i = 0; i < len; i++) { crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8); }
    return ~crc;
}
```

`kernel/lib/crc32.h`: include guard, `#include <stdint.h>`, the prototype.

`kernel/block/ramblk.c`:

```c
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
    int      registered;
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
    uint64_t bytes = sector_size * sectors;
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
```
(Drop the unused `registered` field if the compiler warns.)

`kernel/block/blockdev.c` (above the selftest):

```c
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
static uint32_t         blkext_next_minor;   // major 259, shared by >15th partitions and NVMe

static int name_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int name_len(const char *s) { int n = 0; while (s[n]) { n++; } return n; }

void blockdev_init(void) { spin_init(&reg_lock, LOCK_RANK_BLOCKDEV, "blockdev"); }

uint64_t blockdev_makedev(uint32_t major, uint32_t minor) {
    // glibc/Linux dev_t encoding (sys/sysmacros.h makedev).
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

// Caller holds reg_lock. Returns a slot index or -1.
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
    d->parent = 0; d->start_lba = 0; d->partno = 0;
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    int slot = insert_locked(d);
    if (slot < 0) { spin_unlock_irqrestore(&reg_lock, fl); return slot; }
    if (d->flags & BLOCKDEV_HIDDEN)                       { d->major = 0; d->minor = 0; }
    else if (d->name[0] == 's' && d->name[1] == 'd' && name_len(d->name) == 3)
                                                          { d->major = 8; d->minor = (uint32_t)(d->name[2] - 'a') * 16; }
    else                                                  { d->major = 259; d->minor = blkext_next_minor++; }
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
    int digit_end = n > 0 && disk->name[n - 1] >= '0' && disk->name[n - 1] <= '9';
    if (digit_end) { name[j++] = 'p'; }
    char num[11]; int k = 0; uint32_t v = partno;
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
    for (int i = 0; i < 16; i++) { p->part_type[i] = type ? type[i] : 0; p->part_uuid[i] = uuid ? uuid[i] : 0; }
    int slot = insert_locked(p);
    if (slot < 0) { spin_unlock_irqrestore(&reg_lock, fl); return slot; }
    part_used[pi] = 1;
    if (p->flags & BLOCKDEV_HIDDEN)            { p->major = 0; p->minor = 0; }
    else if (disk->major == 8 && partno < 16)  { p->major = 8; p->minor = disk->minor + partno; }
    else                                       { p->major = 259; p->minor = blkext_next_minor++; }
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
    uint64_t fl = spin_lock_irqsave(&reg_lock);
    char gone[BLOCKDEV_MAX][BLOCKDEV_NAME_MAX];
    int ngone = 0;
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
```

Registration order note: the registry puts the whole disk in the table *before* registering its partitions (via `part_scan`), and array order is registration order, which `/proc/partitions` relies on. Partition slots reuse freed table slots, so after selftests unregister their RAM disks a later registration can land in an earlier slot; that is harmless because selftest disks are hidden.

`kernel/block/part.h` with the two prototypes, and a temporary `kernel/block/part.c`:

```c
#include "block/part.h"
void part_scan(struct blockdev *disk) { (void)disk; }   // Task 3
void part_selftest(void) {}                               // Task 3
```

`kernel/block/blkdev_file.h` (Task 5 implements the ops; declare now so `publish` links):

```c
#ifndef NEOOS_BLKDEV_FILE_H
#define NEOOS_BLKDEV_FILE_H
#include "fs/file.h"
extern const struct file_ops blkdev_file_ops;
#endif
```
and a temporary `kernel/block/blkdev_file.c` defining `const struct file_ops blkdev_file_ops = { .name = "blkdev" };` (Task 5 replaces it).

devfs: add `uint64_t rdev;` as the last member of `struct devfs_dev` (`kernel/fs/devfs.h`), and in `devfs.c` add, next to `devfs_register`:

```c
// A block node at the ROOT of /dev ("sda", "sda1"). Same dynamic table
// as /dev/pts/N; the entry carries VNODE_BLOCK and its dev_t.
int devfs_register_blk(const char *name, const struct file_ops *ops, void *priv, uint64_t rdev) {
    int rc = devfs_register(name, ops, priv, 0);
    if (rc != 0) { return rc; }
    uint64_t fl = spin_lock_irqsave(&dyn_lock);
    for (int i = 0; i < DEVFS_DYN_MAX; i++) {
        if (dyn[i].used && name_eq(dyn[i].path, name)) {
            dyn[i].dev.type = VNODE_BLOCK;
            dyn[i].dev.rdev = rdev;
            break;
        }
    }
    spin_unlock_irqrestore(&dyn_lock, fl);
    return 0;
}
```
Declare it in `devfs.h`. Add `VNODE_BLOCK` to `enum vnode_type` in `vfs.h:63`: `enum vnode_type { VNODE_FILE, VNODE_DIR, VNODE_DEVICE, VNODE_BLOCK };`. (Lookup, readdir and stat of these nodes come in Task 5.)

`kernel/fs/blkcache.h` needs `void blkcache_invalidate(struct blockdev *d);` for `blockdev_unregister_disk`. Until Task 4 reworks the cache, add a temporary no-op in `blkcache.c`: `void blkcache_invalidate(struct blockdev *d) { (void)d; }` with a `struct blockdev;` forward declaration in the header.

- [ ] **Step 4: Run make lock-check and the boot test**

Run: `make lock-check && make test 2>&1 | tail -5`
Expected: lock-check clean; `PASS: no FAILED lines...`; `grep '\[blockdev\] selftest' build/serial.log` → `[blockdev] selftest passed`.

- [ ] **Step 5: Commit**

```bash
git add -A kernel Makefile
git commit -m "feat(block): blockdev registry, crc32, RAM test disk (storage-01 task 2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: GPT/MBR partition scanner

**Files:**
- Modify: `kernel/block/part.c` (replace the stub), `kernel/kernel.c`, `Makefile` markers

**Interfaces:**
- Consumes: `blockdev_read`, `blockdev_register_part`, `blockdev_find`, `ramblk_*`, `crc32` (Task 2).
- Produces: `void part_scan(struct blockdev *disk)`, `void part_selftest(void)` — prints `[part] selftest passed`.

- [ ] **Step 1: Write the failing selftest**

Put this at the bottom of `kernel/block/part.c` (keeping the stub `part_scan` for now so it fails):

```c
// ---- selftest -------------------------------------------------------------
#define T_SECT 1024       // 512 KiB RAM disk
#define T_SS   512

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); } }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v >> (8 * i)); } }

struct tent { int index; uint64_t first, last; };

// Writes a GPT (primary at LBA 1, entries at 2..33; backup entries at
// T_SECT-33.., backup header at T_SECT-1) plus a protective MBR.
static void gpt_build(uint8_t *disk, const struct tent *e, int n) {
    for (uint64_t i = 0; i < (uint64_t)T_SECT * T_SS; i++) { disk[i] = 0; }
    uint8_t *mbr = disk;
    mbr[446 + 4] = 0xEE; put32(mbr + 446 + 8, 1); put32(mbr + 446 + 12, T_SECT - 1);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    uint8_t ents[128 * 128];
    for (int i = 0; i < 128 * 128; i++) { ents[i] = 0; }
    for (int i = 0; i < n; i++) {
        uint8_t *p = ents + e[i].index * 128;
        p[0] = 0xAF; p[1] = 0x3D; p[2] = 0xC6; p[3] = 0x0F;   // any non-zero type GUID
        p[16] = (uint8_t)(i + 1);                              // unique GUID
        put64(p + 32, e[i].first); put64(p + 40, e[i].last);
    }
    uint32_t ecrc = crc32(0, ents, sizeof ents);
    for (int copy = 0; copy < 2; copy++) {
        uint64_t hdr_lba = copy ? T_SECT - 1 : 1, alt = copy ? 1 : T_SECT - 1;
        uint64_t ent_lba = copy ? T_SECT - 33 : 2;
        for (int i = 0; i < 128 * 128; i++) { disk[ent_lba * T_SS + i] = ents[i]; }
        uint8_t *h = disk + hdr_lba * T_SS;
        const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
        for (int i = 0; i < 8; i++) { h[i] = (uint8_t)sig[i]; }
        put32(h + 8, 0x00010000); put32(h + 12, 92);
        put64(h + 24, hdr_lba); put64(h + 32, alt);
        put64(h + 40, 34); put64(h + 48, T_SECT - 34);
        put64(h + 72, ent_lba); put32(h + 80, 128); put32(h + 84, 128); put32(h + 88, ecrc);
        put32(h + 16, 0);
        put32(h + 16, crc32(0, h, 92));
    }
}

static void mbr_build(uint8_t *disk, const uint8_t types[4], const uint32_t start[4], const uint32_t count[4]) {
    for (uint64_t i = 0; i < (uint64_t)T_SECT * T_SS; i++) { disk[i] = 0; }
    for (int i = 0; i < 4; i++) {
        uint8_t *p = disk + 446 + 16 * i;
        p[4] = types[i]; put32(p + 8, start[i]); put32(p + 12, count[i]);
    }
    disk[510] = 0x55; disk[511] = 0xAA;
}

// Registers a hidden RAM disk built by `fill`, returns it (caller destroys).
static struct blockdev *scan_disk(const char *name, void (*fill)(uint8_t *, void *), void *arg) {
    struct blockdev *d = ramblk_create(name, T_SS, T_SECT);
    if (!d) { return 0; }
    fill(ramblk_data(d), arg);
    blockdev_register_disk(d);
    return d;
}

static int has(const char *n, uint64_t start, uint64_t count) {
    struct blockdev *p = blockdev_find(n);
    return p && p->start_lba == start && p->sector_count == count;
}

struct gcase { const struct tent *e; int n; int corrupt; };
static void fill_gpt(uint8_t *disk, void *arg) {
    struct gcase *c = (struct gcase *)arg;
    gpt_build(disk, c->e, c->n);
    if (c->corrupt & 1) { disk[1 * T_SS + 16] ^= 0xFF; }            // primary header CRC
    if (c->corrupt & 2) { disk[(T_SECT - 1) * T_SS + 16] ^= 0xFF; } // backup header CRC
}
struct mcase { uint8_t t[4]; uint32_t s[4], c[4]; };
static void fill_mbr(uint8_t *disk, void *arg) { struct mcase *m = arg; mbr_build(disk, m->t, m->s, m->c); }
static void fill_zero(uint8_t *disk, void *arg) { (void)arg; for (uint64_t i = 0; i < (uint64_t)T_SECT * T_SS; i++) { disk[i] = 0; } }

void part_selftest(void) {
    const char *why = 0;
    if (crc32(0, "123456789", 9) != 0xCBF43926u) { why = "crc32 known answer"; goto done; }

    // GPT: entries at index 0 and 2 (index 1 empty -- numbering keeps the gap).
    static const struct tent two[] = { { 0, 64, 127 }, { 2, 128, 191 } };
    struct gcase g = { two, 2, 0 };
    struct blockdev *d = scan_disk("pga", fill_gpt, &g);
    if (!d || !has("pga1", 64, 64) || !has("pga3", 128, 64) || blockdev_find("pga2")) { why = "gpt valid"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    g.corrupt = 1;                                    // bad primary, good backup
    d = scan_disk("pgb", fill_gpt, &g);
    if (!d || !has("pgb1", 64, 64) || !has("pgb3", 128, 64)) { why = "gpt backup fallback"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    g.corrupt = 3;                                    // both bad, protective MBR -> nothing
    d = scan_disk("pgc", fill_gpt, &g);
    if (!d || blockdev_find("pgc1")) { why = "protective MBR without GPT registered a partition"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    static const struct tent bad[] = { { 0, 64, 127 }, { 1, 100, 150 }, { 2, 900, 5000 } };
    struct gcase gb = { bad, 3, 0 };                  // overlap + past end
    d = scan_disk("pgd", fill_gpt, &gb);
    if (!d || !has("pgd1", 64, 64) || blockdev_find("pgd2") || blockdev_find("pgd3")) { why = "gpt bad entries"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    struct mcase m = { { 0x83, 0x83, 0x05, 0 }, { 64, 256, 512, 0 }, { 128, 128, 64, 0 } };
    d = scan_disk("pma", fill_mbr, &m);
    if (!d || !has("pma1", 64, 128) || !has("pma2", 256, 128) || blockdev_find("pma3")) { why = "mbr primaries/extended"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    struct mcase mo = { { 0x83, 0x83, 0x83, 0 }, { 64, 100, 1000, 0 }, { 128, 50, 100, 0 } };
    d = scan_disk("pmb", fill_mbr, &mo);             // overlap + past end
    if (!d || !has("pmb1", 64, 128) || blockdev_find("pmb2") || blockdev_find("pmb3")) { why = "mbr bad entries"; }
    ramblk_destroy(d);
    if (why) { goto done; }

    d = scan_disk("pza", fill_zero, 0);
    if (!d || blockdev_find("pza1")) { why = "bare disk"; }
    ramblk_destroy(d);

done:
    if (why) {
        serial_write_string("[part] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[part] selftest passed\n");
}
```

Call `part_selftest();` in `kernel.c` right after `blockdev_selftest();`, and add `"[part] selftest passed" \` to `CORE_REQUIRED_MARKERS`.

- [ ] **Step 2: Run it to verify it fails**

Run: `make test 2>&1 | tail -5`
Expected: FAIL — `[part] selftest FAILED: gpt valid` (the stub scanner registers nothing).

- [ ] **Step 3: Implement the scanner**

Replace the stub `part_scan` at the top of `kernel/block/part.c`:

```c
// Partition-table scanner: GPT first (a GPT disk's MBR is a protective
// placeholder), then MBR primaries. Runs once per whole disk, from
// blockdev_register_disk. A disk with neither is left whole.
#include "block/part.h"
#include "block/blockdev.h"
#include "block/ramblk.h"
#include "lib/crc32.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "errno.h"
#include "drivers/char/serial.h"

#define GPT_MAX_ENTRIES 128
#define GPT_ENTRY_SIZE  128
#define ENT_BYTES       (GPT_MAX_ENTRIES * GPT_ENTRY_SIZE)   // 16 KiB = 4 frames

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static void note(struct blockdev *d, const char *msg, uint32_t n) {
    if (d->flags & BLOCKDEV_HIDDEN) { return; }
    serial_write_string("part: ");
    serial_write_string(d->name);
    serial_write_string(": ");
    if (n) { serial_write_string("entry "); serial_write_hex64(n); serial_write_string(" rejected: "); }
    serial_write_string(msg);
    serial_write_string("\n");
}

struct accepted { uint64_t first, last; };

// Registers [first,last] as partition `no` unless it is empty, runs past
// the disk, or overlaps an entry already accepted on this disk.
static void accept(struct blockdev *d, struct accepted *acc, int *nacc, uint32_t no,
                   uint64_t first, uint64_t count, const uint8_t type[16], const uint8_t uuid[16]) {
    if (count == 0)                                            { note(d, "zero length", no); return; }
    if (first >= d->sector_count || count > d->sector_count - first) { note(d, "past end of disk", no); return; }
    uint64_t last = first + count - 1;
    for (int i = 0; i < *nacc; i++) {
        if (first <= acc[i].last && last >= acc[i].first)      { note(d, "overlaps another entry", no); return; }
    }
    if (blockdev_register_part(d, no, first, count, type, uuid) == 0) {
        acc[*nacc].first = first; acc[*nacc].last = last; (*nacc)++;
    }
}

// Reads and validates the GPT header at `lba` and its entry array into
// `ents`. Returns 1 if both CRCs check out.
static int gpt_load(struct blockdev *d, uint64_t lba, uint8_t *hdr, uint8_t *ents) {
    if (lba == 0 || lba >= d->sector_count) { return 0; }
    if (blockdev_read(d, lba, 1, hdr) != 0) { return 0; }
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) { if (hdr[i] != (uint8_t)sig[i]) { return 0; } }
    uint32_t hsize = get32(hdr + 12);
    if (hsize < 92 || hsize > d->sector_size) { return 0; }
    uint32_t want = get32(hdr + 16);
    uint8_t save[4]; for (int i = 0; i < 4; i++) { save[i] = hdr[16 + i]; hdr[16 + i] = 0; }
    uint32_t got = crc32(0, hdr, hsize);
    for (int i = 0; i < 4; i++) { hdr[16 + i] = save[i]; }
    if (got != want) { return 0; }

    uint32_t n = get32(hdr + 80), esz = get32(hdr + 84);
    if (esz != GPT_ENTRY_SIZE || n == 0 || n > GPT_MAX_ENTRIES) { return 0; }
    uint64_t ent_lba = get64(hdr + 72);
    uint32_t bytes = n * esz;
    uint32_t secs = (bytes + d->sector_size - 1) / d->sector_size;
    if (ent_lba >= d->sector_count || secs > d->sector_count - ent_lba) { return 0; }
    if (blockdev_read(d, ent_lba, secs, ents) != 0) { return 0; }
    return crc32(0, ents, bytes) == get32(hdr + 88);
}

static int scan_gpt(struct blockdev *d, uint8_t *sec, uint8_t *ents) {
    uint64_t backup_lba = d->sector_count - 1;
    int ok = gpt_load(d, 1, sec, ents);
    if (!ok) {
        // A primary that at least has the signature names its backup;
        // otherwise the backup sits in the last sector by definition.
        if (sec[0] == 'E' && sec[1] == 'F' && sec[2] == 'I') {
            uint64_t alt = get64(sec + 32);
            if (alt > 1 && alt < d->sector_count) { backup_lba = alt; }
        }
        ok = gpt_load(d, backup_lba, sec, ents);
        if (ok) { note(d, "primary GPT invalid, using backup", 0); }
    }
    if (!ok) { return 0; }

    struct accepted acc[GPT_MAX_ENTRIES];
    int nacc = 0;
    uint32_t n = get32(sec + 80);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = ents + i * GPT_ENTRY_SIZE;
        int empty = 1;
        for (int k = 0; k < 16; k++) { if (e[k]) { empty = 0; break; } }
        if (empty) { continue; }
        uint64_t first = get64(e + 32), last = get64(e + 40);
        if (last < first) { note(d, "last before first", i + 1); continue; }
        accept(d, acc, &nacc, i + 1, first, last - first + 1, e, e + 16);
    }
    return 1;
}

static void scan_mbr(struct blockdev *d, uint8_t *sec) {
    if (blockdev_read(d, 0, 1, sec) != 0) { return; }
    if (sec[510] != 0x55 || sec[511] != 0xAA) { return; }
    for (int i = 0; i < 4; i++) {
        if (sec[446 + 16 * i + 4] == 0xEE) { note(d, "protective MBR but no valid GPT", 0); return; }
    }
    struct accepted acc[4];
    int nacc = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = sec + 446 + 16 * i;
        uint8_t type = p[4];
        if (type == 0) { continue; }
        if (type == 0x05 || type == 0x0F || type == 0x85) { note(d, "extended partition skipped", 0); continue; }
        uint8_t t[16] = { type };
        accept(d, acc, &nacc, (uint32_t)i + 1, get32(p + 8), get32(p + 12), t, 0);
    }
}

void part_scan(struct blockdev *disk) {
    // One sector (up to 4 KiB) plus the 16 KiB entry array.
    uint64_t sec_phys = pmm_alloc(0), ent_phys = pmm_alloc(2);
    if (!sec_phys || !ent_phys) {
        if (sec_phys) { pmm_free(sec_phys, 0); }
        if (ent_phys) { pmm_free(ent_phys, 2); }
        note(disk, "scan FAILED: out of memory", 0);
        return;
    }
    uint8_t *sec = (uint8_t *)phys_to_virt(sec_phys), *ents = (uint8_t *)phys_to_virt(ent_phys);
    if (disk->sector_count >= 3 && !scan_gpt(disk, sec, ents)) { scan_mbr(disk, sec); }
    pmm_free(sec_phys, 0);
    pmm_free(ent_phys, 2);
}
```

Check: an entry array of 128 × 128 = 16 KiB is 32 sectors at 512 bytes or 4 at 4096 — both fit the 4-frame buffer. The case `pgd3` (900..5000) is rejected as past the end; `pgd2` (100..150) overlaps `pgd1`.

- [ ] **Step 4: Run the test**

Run: `make test 2>&1 | tail -5`
Expected: `PASS: ...`; `grep '\[part\]' build/serial.log` → `[part] selftest passed`. The real disks log no `part:` lines (unpartitioned FAT images: the FAT boot sector has `55 AA` at 510 but its "partition entries" are boot code — see Step 5).

- [ ] **Step 5: Guard against a FAT boot sector parsed as an MBR**

A FAT volume's sector 0 also ends in `55 AA`, and bytes 446–509 are boot code (mkfs.fat puts its "This is not a bootable disk" message there), which can decode as garbage partition entries. Use Linux's rule (`block/partitions/msdos.c`): a table in which any entry's boot indicator (byte 0 of the entry) is neither `0x00` nor `0x80` is not a partition table at all. Add at the top of `scan_mbr`, after the `55 AA` check:

```c
    // Linux's test: every boot indicator must be 0x00 or 0x80. A FAT or
    // other filesystem boot sector on an unpartitioned disk fails it,
    // because its "entries" are boot code and message text.
    for (int i = 0; i < 4; i++) {
        uint8_t boot = sec[446 + 16 * i];
        if (boot != 0x00 && boot != 0x80) { return; }
    }
```

Add a case to `part_selftest` that proves it, placed before the final `d = scan_disk("pza", ...)` block:

```c
    struct mcase mf = { { 0x83, 0, 0, 0 }, { 64, 0, 0, 0 }, { 128, 0, 0, 0 } };
    d = ramblk_create("pfa", T_SS, T_SECT);
    if (d) {
        mbr_build(ramblk_data(d), mf.t, mf.s, mf.c);
        ramblk_data(d)[446 + 16] = 'T';   // entry 2's boot byte is message text
        blockdev_register_disk(d);
        if (blockdev_find("pfa1")) { why = "boot-code sector taken for an MBR"; }
    } else { why = "ramblk pfa"; }
    ramblk_destroy(d);
    if (why) { goto done; }
```

Run: `make test 2>&1 | tail -5`, then `grep '^part:' build/serial.log`.
Expected: `PASS: ...`, and no `part:` lines for the real disks.

- [ ] **Step 6: Commit**

```bash
git add -A kernel Makefile
git commit -m "feat(block): GPT and MBR partition scanning (storage-01 task 3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: blkcache on blockdevs, ATA as a blockdev driver, FAT on /dev/sdX

These three change together: the cache API changes shape, and FAT and ATA are its only clients.

**Files:**
- Modify: `kernel/fs/blkcache.h`, `kernel/fs/blkcache.c` (rewrite), `kernel/drivers/block/ata.h`, `kernel/drivers/block/ata.c`, `kernel/fs/fatfs.c` (`struct fat_volume`, mount/umount, every `blkcache_*` call, `fat16_mount`), `kernel/fs/vfs.h:141`, `kernel/kernel.c:239-260`, `kernel/syscall/sys_file.c:368` (`sys_mount`), `Makefile` markers

**Interfaces:**
- Consumes: Task 2 registry and ramblk; Task 3 `part_scan` (called by register).
- Produces:

```c
// kernel/fs/blkcache.h
#define BLKCACHE_MAX_SECTOR 4096
#define BLKCACHE_ENTRIES    128
#define BLKCACHE_BUCKETS    64
struct blockdev;
void blkcache_init(void);
int  blkcache_read(struct blockdev *d, uint64_t lba, void *out);          // 0 / -errno
int  blkcache_write(struct blockdev *d, uint64_t lba, const void *in);    // 0 / -errno
int  blkcache_read_multi(struct blockdev *d, uint64_t lba, uint32_t count, void *out);
int  blkcache_write_multi(struct blockdev *d, uint64_t lba, uint32_t count, const void *in);
void blkcache_invalidate(struct blockdev *d);     // every entry of d's whole disk
void blkcache_stats(uint64_t *hits, uint64_t *misses);
void blkcache_selftest(void);

// kernel/drivers/block/ata.h
void ata_init(void);
void ata_probe(void);       // registers each present primary-channel drive as sdX
```

- [ ] **Step 1: Write the failing (hermetic) blkcache selftest**

Replace `blkcache_selftest` in `kernel/fs/blkcache.c` with a version that uses RAM disks only (the old one read ATA drive 0 directly):

```c
void blkcache_selftest(void) {
    const char *why = 0;
    uint8_t *a = 0, *b = 0;
    struct blockdev *d = ramblk_create("bct", 512, 256);
    struct blockdev *q = ramblk_create("bcq", 4096, 16);
    uint64_t pa = pmm_alloc(0), pb = pmm_alloc(0);
    if (!d || !q || !pa || !pb) { why = "setup"; goto out; }
    a = (uint8_t *)phys_to_virt(pa);
    b = (uint8_t *)phys_to_virt(pb);

    // One MBR partition at LBA 64, so "bct1" and "bct" alias sector 70.
    uint8_t *raw = ramblk_data(d);
    raw[446 + 4] = 0x83; raw[446 + 8] = 64; raw[446 + 12] = 128; raw[510] = 0x55; raw[511] = 0xAA;
    for (int i = 0; i < 512; i++) { raw[70 * 512 + i] = (uint8_t)i; }
    blockdev_register_disk(d);
    struct blockdev *p = blockdev_find("bct1");
    if (!p) { why = "partition not found"; goto out; }

    uint64_t r0 = ramblk_reads(d);
    if (blkcache_read(p, 6, a) != 0 || a[3] != 3)         { why = "read via partition"; goto out; }
    if (ramblk_reads(d) != r0 + 1)                        { why = "miss did not reach device"; goto out; }
    if (blkcache_read(d, 70, b) != 0 || b[3] != 3)        { why = "read via disk"; goto out; }
    if (ramblk_reads(d) != r0 + 1)                        { why = "partition/disk alias missed the cache"; goto out; }

    for (int i = 0; i < 512; i++) { a[i] = 0xA5; }
    if (blkcache_write(d, 70, a) != 0)                    { why = "write via disk"; goto out; }
    if (blkcache_read(p, 6, b) != 0 || b[0] != 0xA5)      { why = "write not visible via partition"; goto out; }

    for (int i = 0; i < 3 * 512 && i < 4096; i++) { a[i] = 0x3C; }
    if (blkcache_write_multi(d, 69, 3, a) != 0)           { why = "write_multi"; goto out; }
    if (blkcache_read(p, 6, b) != 0 || b[0] != 0x3C)      { why = "write_multi left a stale entry"; goto out; }
    if (raw[71 * 512] != 0x3C)                            { why = "write_multi missed the device"; goto out; }

    // Evict sector 70, read it again: from the device, same bytes.
    for (uint64_t lba = 100; lba < 100 + BLKCACHE_ENTRIES + 8; lba++) {
        if (blkcache_read(d, lba, b) != 0)                { why = "eviction sweep"; goto out; }
    }
    uint64_t r1 = ramblk_reads(d);
    if (blkcache_read(d, 70, b) != 0 || b[0] != 0x3C)     { why = "reread after eviction"; goto out; }
    if (ramblk_reads(d) != r1 + 1)                        { why = "evicted sector served from cache"; goto out; }

    // A 4 KiB-sector device round-trips whole sectors.
    for (int i = 0; i < 4096; i++) { a[i] = (uint8_t)(i * 7); }
    blockdev_register_disk(q);
    if (blkcache_write(q, 3, a) != 0 || blkcache_read(q, 3, b) != 0) { why = "4Kn io"; goto out; }
    for (int i = 0; i < 4096; i++) { if (b[i] != (uint8_t)(i * 7)) { why = "4Kn data"; goto out; } }

out:
    if (pa) { pmm_free(pa, 0); }
    if (pb) { pmm_free(pb, 0); }
    ramblk_destroy(d);
    ramblk_destroy(q);
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    hits = 0; misses = 0;      // the sweep's misses would drown the boot's real numbers
    spin_unlock_irqrestore(&cache_lock, flags);
    if (why) {
        serial_write_string("[blkcache] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[blkcache] selftest passed\n");
}
```

Move the `blkcache_selftest();` call in `kernel.c` to after `part_selftest();` (it needs the registry), and add `"[blkcache] selftest passed" \` to `CORE_REQUIRED_MARKERS`.

- [ ] **Step 2: Verify it fails**

Run: `make build 2>&1 | grep -m3 error`
Expected: compile errors — `blkcache_read` takes `uint8_t drive`, `blkcache_write_multi` undefined.

- [ ] **Step 3: Rewrite the cache**

`kernel/fs/blkcache.h`: keep the existing header comment (it still holds), replace the declarations with the Interfaces block above, and add to the comment: "Keyed by (whole-disk blockdev, whole-disk LBA): a sector read through /dev/sda1 and through /dev/sda is one entry, so the two can never disagree."

`kernel/fs/blkcache.c` — the structure stays; the changes:

```c
#include "fs/blkcache.h"
#include "block/blockdev.h"
#include "block/ramblk.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sync/lock.h"
#include "errno.h"
#include "drivers/char/serial.h"

struct blk_buf {
    uint8_t *data;              // BLKCACHE_MAX_SECTOR bytes, from the pool
    struct blockdev *dev;       // WHOLE disk
    uint64_t lba;               // whole-disk LBA
    uint8_t  valid;
    uint64_t last_used;
    struct blk_buf *hash_next;
};

static unsigned bucket_of(struct blockdev *dev, uint64_t lba) {
    uint32_t h = ((uint32_t)lba * 2654435761u) ^ (uint32_t)((uintptr_t)dev >> 4);
    return (unsigned)(h % BLKCACHE_BUCKETS);
}
```

`blkcache_init`: after the existing initialisation, back the entries with one 512 KiB block — `uint64_t pool = pmm_alloc(7);` (128 frames); if 0, print `[blkcache] init FAILED: no memory for the pool` and return; `entries[i].data = (uint8_t *)phys_to_virt(pool) + i * BLKCACHE_MAX_SECTOR;`.

`lookup`, `claim`, `unlink_buf`: replace `(drive, lba)` with `(dev, lba)`; compare `b->dev == dev`.

Public functions (every one translates to the whole disk first):

```c
static int resolve(struct blockdev *d, uint64_t lba, uint32_t count,
                   struct blockdev **whole, uint64_t *dlba) {
    if (count == 0 || lba >= d->sector_count || count > d->sector_count - lba) { return -EIO; }
    *whole = blockdev_whole(d);
    *dlba  = blockdev_disk_lba(d, lba);
    return 0;
}

int blkcache_read(struct blockdev *d, uint64_t lba, void *out) {
    struct blockdev *w; uint64_t dl;
    int rc = resolve(d, lba, 1, &w, &dl);
    if (rc) { return rc; }
    uint32_t ss = w->sector_size;
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    struct blk_buf *b = lookup(w, dl);
    if (b) {
        b->last_used = ++clock_tick;
        mem_copy(out, b->data, ss);
        hits++;
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }
    misses++;
    spin_unlock_irqrestore(&cache_lock, flags);

    // Device I/O with the lock DROPPED (unchanged rationale: it is a
    // long busy-wait, and two racing readers install the same bytes).
    rc = blockdev_read(w, dl, 1, out);
    if (rc) { return rc; }

    flags = spin_lock_irqsave(&cache_lock);
    b = claim(w, dl);
    mem_copy(b->data, out, ss);
    b->last_used = ++clock_tick;
    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int blkcache_write(struct blockdev *d, uint64_t lba, const void *in) {
    return blkcache_write_multi(d, lba, 1, in);
}

int blkcache_read_multi(struct blockdev *d, uint64_t lba, uint32_t count, void *out) {
    // Straight to the device: large transfers would evict the metadata
    // the cache exists for. Correct because the cache is write-through
    // -- no cached sector can be newer than the disk.
    struct blockdev *w; uint64_t dl;
    int rc = resolve(d, lba, count, &w, &dl);
    return rc ? rc : blockdev_read(w, dl, count, out);
}

int blkcache_write_multi(struct blockdev *d, uint64_t lba, uint32_t count, const void *in) {
    struct blockdev *w; uint64_t dl;
    int rc = resolve(d, lba, count, &w, &dl);
    if (rc) { return rc; }
    // Disk first, then cache: a failed write must not leave the cache
    // holding bytes the disk never received.
    rc = blockdev_write(w, dl, count, in);
    uint32_t ss = w->sector_size;
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    if (count == 1 && rc == 0) {
        struct blk_buf *b = claim(w, dl);
        mem_copy(b->data, in, ss);
        b->last_used = ++clock_tick;
    } else {
        // Update (or, on failure, drop) any cached sector in the range.
        for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
            struct blk_buf *b = &entries[i];
            if (!b->valid || b->dev != w || b->lba < dl || b->lba >= dl + count) { continue; }
            if (rc == 0) { mem_copy(b->data, (const uint8_t *)in + (b->lba - dl) * ss, ss); }
            else         { unlink_buf(b); }
        }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
    return rc;
}

void blkcache_invalidate(struct blockdev *d) {
    struct blockdev *w = blockdev_whole(d);
    uint64_t flags = spin_lock_irqsave(&cache_lock);
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        if (entries[i].valid && entries[i].dev == w) { unlink_buf(&entries[i]); }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
}
```
`mem_copy`'s length parameter becomes `uint32_t`/`uint64_t` as needed. Remove the old `#include "drivers/block/ata.h"` and the temporary no-op `blkcache_invalidate` from Task 2.

- [ ] **Step 4: ATA as a blockdev driver**

`kernel/drivers/block/ata.h` shrinks to:

```c
#ifndef NEOOS_ATA_H
#define NEOOS_ATA_H
#include <stdint.h>
#define ATA_SECTOR_SIZE 512
// Legacy IDE (primary channel, PIO, LBA28). Registers each drive it
// finds as a blockdev ("sdX"); nothing outside this file calls it
// directly any more.
void ata_init(void);
void ata_probe(void);
#endif
```

In `ata.c`: make `ata_lock` `static`; make `struct ata_identify_info` file-local; in `ata_identify_locked`, change the "no drive present" message so an absent drive is not a failure:

```c
    if (inb(ATA_STATUS) == 0) {
        serial_write_string("[ata] drive ");
        serial_write_hex64(drive);
        serial_write_string(" not present\n");
        return 0;
    }
```
Replace the three public locked wrappers with the blockdev glue:

```c
#include "block/blockdev.h"
#include "errno.h"

#define ATA_LBA28_LIMIT (1ULL << 28)

struct ata_disk { struct blockdev bdev; uint8_t drive; };
static struct ata_disk disks[2];

static int ata_flush_locked(uint8_t drive) {
    outb(ATA_DRIVE_HEAD, 0xE0 | ((drive & 1) << 4));
    if (!ata_wait_status(ATA_STATUS_BSY, 0)) { return 0; }
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_status_bounded(ATA_STATUS_BSY, 0, ATA_POLL_TIMEOUT_NS_FLUSH);
}

// The command set is LBA28 with an 8-bit sector count: split into
// commands of at most 255 sectors, each under the lock (one command
// sequence at a time on the shared registers), releasing between them.
static int ata_xfer(struct blockdev *d, uint64_t lba, uint32_t count, void *buf, int wr) {
    uint8_t drive = ((struct ata_disk *)d)->drive;
    if (lba + count > ATA_LBA28_LIMIT) { return -EIO; }
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint8_t n = count > 255 ? 255 : (uint8_t)count;
        uint64_t f = spin_lock_irqsave(&ata_lock);
        int ok = wr ? ata_write_sectors_locked(drive, (uint32_t)lba, n, p)
                    : ata_read_sectors_locked(drive, (uint32_t)lba, n, p);
        spin_unlock_irqrestore(&ata_lock, f);
        if (!ok) { return -EIO; }
        lba += n; count -= n; p += (uint32_t)n * ATA_SECTOR_SIZE;
    }
    return 0;
}
static int ata_bread(struct blockdev *d, uint64_t lba, uint32_t c, void *b) { return ata_xfer(d, lba, c, b, 0); }
static int ata_bwrite(struct blockdev *d, uint64_t lba, uint32_t c, const void *b) { return ata_xfer(d, lba, c, (void *)b, 1); }
static int ata_bflush(struct blockdev *d) {
    uint64_t f = spin_lock_irqsave(&ata_lock);
    int ok = ata_flush_locked(((struct ata_disk *)d)->drive);
    spin_unlock_irqrestore(&ata_lock, f);
    return ok ? 0 : -EIO;
}
static const struct blockdev_ops ata_ops = { .read = ata_bread, .write = ata_bwrite, .flush = ata_bflush };

void ata_probe(void) {
    for (uint8_t drive = 0; drive < 2; drive++) {
        struct ata_identify_info info;
        uint64_t f = spin_lock_irqsave(&ata_lock);
        int ok = ata_identify_locked(drive, &info);
        spin_unlock_irqrestore(&ata_lock, f);
        if (!ok) { continue; }
        struct ata_disk *ad = &disks[drive];
        ad->drive = drive;
        if (blockdev_alloc_name("sd", ad->bdev.name) != 0) { return; }
        ad->bdev.sector_size = ATA_SECTOR_SIZE;
        ad->bdev.sector_count = info.sector_count;
        ad->bdev.ops = &ata_ops;
        ad->bdev.priv = ad;
        blockdev_register_disk(&ad->bdev);
    }
}
```
(The write path still flushes after every write, as before; `flush` exists for callers such as `fsync` on a raw node.)

- [ ] **Step 5: FAT on blockdevs**

In `kernel/fs/fatfs.c`:

1. `struct fat_volume`: replace `uint8_t  drive;` with `struct blockdev *bdev;`. Add `#include "block/blockdev.h"`.
2. Add after the struct:

```c
// FAT reads and writes one 512-byte sector at a time through the
// cache. These keep the driver's existing 1-on-success convention, so
// the conversion below is a pure rename at every call site.
static int fat_rd(struct fat_volume *v, uint32_t lba, void *buf)       { return blkcache_read(v->bdev, lba, buf) == 0; }
static int fat_wr(struct fat_volume *v, uint32_t lba, const void *buf) { return blkcache_write(v->bdev, lba, buf) == 0; }
```
3. Mechanically rename every call:

```bash
sed -i 's/blkcache_read(v->drive, /fat_rd(v, /g; s/blkcache_write(v->drive, /fat_wr(v, /g' kernel/fs/fatfs.c
grep -n "blkcache_\|->drive" kernel/fs/fatfs.c
```
The grep must show only the two helper definitions, the umount invalidation, and the mount log line; fix any remaining call that used a variable other than `v` by hand, following the same rename.

4. `fatfs_mount_op`:

```c
static int fatfs_mount_op(struct vfs_mount *m, const char *source) {
    struct blockdev *bdev = blockdev_find_path(source);
    if (!bdev) { return -ENODEV; }
    // FAT's BPB allows larger sectors; this driver assumes 512 throughout.
    if (bdev->sector_size != 512) { return -EINVAL; }
    /* ... slot search unchanged ... */
    struct fat_volume *v = &volumes[slot];
    v->bdev = bdev;
    if (!fat_read_bpb(v)) { return -ENODEV; }
    /* ... */
    serial_write_string("[fatfs] mounted ");
    serial_write_string(bdev->name);
    /* ... variant / sectors_per_cluster as before ... */
}
```
5. `fatfs_umount_op`: `if (v) { blkcache_invalidate(v->bdev); }`.
6. `fat16_mount`: `v->bdev = blockdev_find("sda"); if (!v->bdev) { serial_write_string("[fat16] mount FAILED: no sda\n"); return 0; }`.

- [ ] **Step 6: Boot wiring, vfs comment, mount source length**

`kernel/kernel.c` storage block becomes:

```c
    ata_init();   // before the first ata_* call

    // Before the first sector read of the boot: every filesystem read
    // below goes through it.
    blkcache_init();
    blockdev_init();
    blockdev_selftest();
    part_selftest();
    blkcache_selftest();
    // Controller probes register disks (and, through the partition
    // scan, their partitions). Later: AHCI and NVMe probe first.
    ata_probe();

    fat16_mount();
    fat16_selftest();
    fat16_write_selftest();

    vfs_init();
    flock_init();          // POSIX record locks (fcntl F_SETLK)
    vfs_mount_fs("/dev/sda", "/",    "fat");
    ...
    vfs_mount_fs("/dev/sdb", "/mnt", "fat");
```
Remove the `struct ata_identify_info ata_info; ata_identify(0, &ata_info);` lines.

`kernel/fs/vfs.h:141` comment: `// fstype is "fat", "ramfs", "devfs", "procfs" or "embedfs". For "fat", source is a block device: "/dev/sda1" or "sda1".`

`kernel/syscall/sys_file.c` `sys_mount`: `char source[64], ...` (was 16) so `/dev/nvme0n1p1` fits.

Add `"[blockdev] sda: sectors=" \` to `CORE_REQUIRED_MARKERS` (proves the probe registered the boot disk).

- [ ] **Step 7: Run the tests**

Run: `make lock-check && make test 2>&1 | tail -5`
Expected: `PASS: ...`. Also:

```bash
grep -E '^\[blockdev\] sd|\[fatfs\] mounted|\[ata\] drive|blkcache\] selftest' build/serial.log
```
Expected: `[blockdev] sda: sectors=...`, `[blockdev] sdb: sectors=...`, two `[fatfs] mounted sda`/`sdb` lines, `[blkcache] selftest passed`, no `FAILED`.

Then `make sqlitetest` (it writes and re-reads the FAT disk across two boots: the strongest end-to-end check of the cache and ATA write path).
Expected: `PASS sqlitetest`.

Then prove an empty IDE slot is not a failure (Review Focus 1) by booting with only the first disk. `/mnt` will fail to mount in this boot, and the FAT32 selftests will say so; only the `[ata]` lines are under test:

```bash
timeout 60 qemu-system-x86_64 -cpu Nehalem -smp 2 -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -display none -serial file:build/onedisk.log -no-reboot
grep '^\[ata\]' build/onedisk.log
```
Expected: a `[ata] drive 0x1 not present` line (or `0x0000000000000001`, per `serial_write_hex64`'s format), and no `[ata]` line containing `FAILED`. Rebuild the disk images afterwards (`make fresh-disks disk-image`), since the boot's write selftest modified `build/disk.img`.

- [ ] **Step 8: Commit**

```bash
git add -A kernel Makefile
git commit -m "feat(block): blkcache on blockdevs, ATA registers sdX, FAT mounts /dev/sdX (storage-01 task 4)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Raw block nodes — lookup, stat, read/write/lseek/ioctl/fsync

**Files:**
- Modify: `kernel/fs/devfs.c` (root lookup/readdir/read_inode for dynamic root entries), `kernel/fs/stat.h`, `kernel/fs/vfs.c:477-516` (`vfs_stat_vnode`), `kernel/syscall/sys_file.c` (statx rdev, `sys_fsync`), `kernel/fs/file.c:173` (`file_bind_vnode_ops`), `kernel/fs/file.h` (`fsync` member)
- Replace: `kernel/block/blkdev_file.c`
- Modify: `kernel/fs/fpos64_selftest.c` (block-node half)

**Interfaces:**
- Consumes: `blkcache_*` (Task 4), `struct blockdev` (Task 2), `devfs_register_blk` (Task 2).
- Produces: `const struct file_ops blkdev_file_ops;` whose `priv` is the `struct blockdev *`; `file_ops.fsync` — `int (*fsync)(struct file_descriptor *f);`, nullable.

- [ ] **Step 1: Write the failing test**

Append to `fpos64_selftest` in `kernel/fs/fpos64_selftest.c`, before the final PASS print (and restructure so `why` from the ramfs half short-circuits):

```c
#include "block/blockdev.h"
#include "block/ramblk.h"
#include "block/blkdev_file.h"

static const char *blk_half(void) {
    struct blockdev *d = ramblk_create("fpb", 4096, 16);       // 64 KiB, 4Kn
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
        if (!why && file_lseek(&f, size - 4, 0) == size - 4 && file_write(&f, msg, 10) != 4) { why = "write crossing end not short"; }
    }
    ramblk_destroy(d);
    return why;
}
```
and in `fpos64_selftest`, replace the Task 1 placeholder comment (just before `if (why) {`) with `if (!why) { why = blk_half(); }`.

- [ ] **Step 2: Verify it fails**

Run: `make test 2>&1 | tail -5`
Expected: FAIL — the placeholder `blkdev_file_ops` has no `.lseek`, so the kernel faults or prints `[fpos64] selftest FAILED: SEEK_END != size`. (If it faults, that is the expected failure.)

- [ ] **Step 3: Implement blkdev_file.c**

```c
// File operations for an opened /dev/<block device>. Byte-granular,
// like Linux's buffered block devices: whole aligned sectors go
// straight to the device; a partial head or tail sector is
// read-modified-written through the cache.
#include "block/blkdev_file.h"
#include "block/blockdev.h"
#include "fs/blkcache.h"
#include "fs/vfs.h"
#include "sync/poll_head.h"
#include "mm/uaccess.h"
#include "errno.h"

#define BLKGETSIZE    0x1260
#define BLKSSZGET     0x1268
#define BLKGETSIZE64  0x80081272

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
            if (n > 256) { n = 256; }
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

static int64_t b_read(struct file_descriptor *f, void *buf, uint64_t len)        { return xfer(f, (uint8_t *)buf, len, 0); }
static int64_t b_write(struct file_descriptor *f, const void *buf, uint64_t len) { return xfer(f, (uint8_t *)buf, len, 1); }

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
    .name = "blkdev", .read = b_read, .write = b_write, .lseek = b_lseek,
    .getdents = b_getdents, .ioctl = b_ioctl, .poll = b_poll, .poll_head = b_poll_head,
    .dup = b_dup, .close = b_close, .fsync = b_fsync,
};
```
Note: the fpos64 selftest passes a kernel buffer to `b_ioctl`'s neighbours only, never to `b_ioctl` itself, so `copy_to_user` is only reached from real syscalls.

`kernel/fs/file.h`, inside `struct file_ops` after `close`:

```c
    // Optional (nullable). fsync/fdatasync on this fd. NULL means the
    // object has nothing to flush (the block cache is write-through),
    // which is what the syscall did for every fd before block nodes.
    int (*fsync)(struct file_descriptor *f);
```

`kernel/syscall/sys_file.c` `sys_fsync` (also serves `fdatasync`):

```c
int64_t sys_fsync(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    if (f->ops && f->ops->fsync) { return f->ops->fsync(f); }
    return 0;
}
```

- [ ] **Step 4: Make the nodes visible and stat-able**

`kernel/fs/file.c:173`: `if (f->vn && (f->vn->type == VNODE_DEVICE || f->vn->type == VNODE_BLOCK)) {`

`kernel/fs/devfs.c`:
- `devfs_read_inode`, dynamic branch: `out->type = dyn[slot].dev.type;` (was hard-coded `VNODE_DEVICE`).
- `devfs_lookup`, root branch, after the static-table loop and before `return -ENOENT;`:

```c
    // Root-level dynamic entries: block devices ("sda", "sda1").
    if (dyn_lock_ready) {
        uint64_t fl = spin_lock_irqsave(&dyn_lock);
        for (int i = 0; i < DEVFS_DYN_MAX; i++) {
            if (dyn[i].used && !devfs_dir_len(dyn[i].path) && name_eq(dyn[i].path, name)) {
                *out_inode_id = DEVFS_DYN_BASE + (uint64_t)i;
                spin_unlock_irqrestore(&dyn_lock, fl);
                return 0;
            }
        }
        spin_unlock_irqrestore(&dyn_lock, fl);
    }
```
- `devfs_readdir`, root branch: after the static loop (where it returns `-ENOENT`), continue the index into root-level dynamic entries:

```c
    if (dyn_lock_ready) {
        uint64_t fl = spin_lock_irqsave(&dyn_lock);
        for (int i = 0; i < DEVFS_DYN_MAX; i++) {
            if (!dyn[i].used || devfs_dir_len(dyn[i].path)) { continue; }
            if (count == index) {
                devfs_copy_name(out, dyn[i].path);
                out->type = dyn[i].dev.type == VNODE_BLOCK ? DT_BLK : DT_CHR;
                out->ino  = DEVFS_DYN_BASE + (uint64_t)i;
                spin_unlock_irqrestore(&dyn_lock, fl);
                return 0;
            }
            count++;
        }
        spin_unlock_irqrestore(&dyn_lock, fl);
    }
    return -ENOENT;
```
(`devfs_dir_len` is defined after the dynamic block; move its definition above `dyn_open` if the compiler needs it earlier.)

`kernel/fs/stat.h`: `#define S_IFBLK  0060000` after `S_IFCHR`.

`kernel/fs/vfs.c` `vfs_stat_vnode`, new case (needs `#include "fs/devfs.h"` if not present):

```c
    case VNODE_BLOCK: {
        // Linux reports st_size 0 for a block node; the size comes from
        // BLKGETSIZE64. 0660 is reported, not enforced -- NeoOS has no
        // permission model yet (docs/stdlib.md).
        const struct devfs_dev *dev = (const struct devfs_dev *)vn->fs_private;
        out->st_mode  = S_IFBLK | 0660;
        out->st_nlink = 1;
        out->st_rdev  = dev ? dev->rdev : 0;
        break;
    }
```

`sys_statx` (`sys_file.c`, next to `sx.stx_mode`): Linux splits `st_rdev` into major/minor:

```c
    sx.stx_rdev_major = (uint32_t)(((st.st_rdev >> 32) & 0xfffff000u) | ((st.st_rdev >> 8) & 0xfffu));
    sx.stx_rdev_minor = (uint32_t)(((st.st_rdev >> 12) & 0xffffff00u) | (st.st_rdev & 0xffu));
```

- [ ] **Step 5: Run the tests**

Run: `make test 2>&1 | tail -5`
Expected: `PASS: ...`, `[fpos64] selftest passed`.

- [ ] **Step 6: Commit**

```bash
git add -A kernel
git commit -m "feat(block): raw /dev block nodes -- rw, lseek, ioctls, fsync, S_IFBLK (storage-01 task 5)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: /proc/partitions

**Files:**
- Modify: `kernel/fs/procfs.c` (inode, lookup, read_inode, read, readdir)

**Interfaces:**
- Consumes: `blockdev_foreach` (Task 2).
- Produces: `/proc/partitions` in Linux's format.

- [ ] **Step 1: Failing check**

Task 7's userland test reads `/proc/partitions`; before this task it fails with `ENOENT`. As a kernel-side check, append to `vfs_selftest`'s end in `kernel/fs/vfs.c`, before its PASS line, a resolve of `/proc/partitions` that must succeed and read back a first line starting with `major minor`:

```c
    {
        int perr = 0;
        struct vnode *pp = vfs_resolve("/proc/partitions", &perr);
        char pb[16] = {0};
        if (!pp || pp->mount->ops->read(pp, 0, pb, 11) != 11 ||
            pb[0] != 'm' || pb[6] != 'm') {
            serial_write_string("[vfs] selftest FAILED: /proc/partitions\n");
            if (pp) { vnode_put(pp); }
            return;
        }
        vnode_put(pp);
    }
```
(match the surrounding code's exact failure-print style; `vfs_selftest` runs with the VFS unlocked and resolves paths the same way elsewhere).

Run: `make test 2>&1 | tail -3` → Expected: `[vfs] selftest FAILED: /proc/partitions`.

- [ ] **Step 2: Implement**

In `procfs.c`:

```c
#define PROC_INO_PARTITIONS 4
#include "block/blockdev.h"

struct part_ctx { char *out; int cap; int at; };

// Right-aligns `v` in a field of `width`, as printf("%*u") would.
static int put_padded(char *out, int cap, int at, uint64_t v, int width) {
    char d[21]; int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = n; i < width && at < cap - 1; i++) { out[at++] = ' '; }
    while (n && at < cap - 1) { out[at++] = d[--n]; }
    return at;
}

static void part_line(struct blockdev *d, void *arg) {
    struct part_ctx *c = (struct part_ctx *)arg;
    if (d->flags & BLOCKDEV_HIDDEN) { return; }
    // Linux: "%4d  %7d %10llu %s\n" -- #blocks in 1 KiB units.
    c->at = put_padded(c->out, c->cap, c->at, d->major, 4);
    c->at = put_str(c->out, c->cap, c->at, "  ");
    c->at = put_padded(c->out, c->cap, c->at, d->minor, 7);
    c->at = put_str(c->out, c->cap, c->at, " ");
    c->at = put_padded(c->out, c->cap, c->at, d->sector_count * d->sector_size / 1024, 10);
    c->at = put_str(c->out, c->cap, c->at, " ");
    c->at = put_str(c->out, c->cap, c->at, d->name);
    c->at = put_str(c->out, c->cap, c->at, "\n");
}

static int render_partitions(char *out, int cap) {
    struct part_ctx c = { out, cap, 0 };
    c.at = put_str(out, cap, 0, "major minor  #blocks  name\n\n");
    blockdev_foreach(part_line, &c);
    out[c.at] = 0;
    return c.at;
}
```
Wire it the same way `meminfo` is wired:
- `procfs_lookup` root: `if (streq(name, "partitions")) { *out_inode_id = PROC_INO_PARTITIONS; return 0; }`
- `procfs_read_inode`: include `PROC_INO_PARTITIONS` in the fixed-file condition.
- `procfs_read`: include it in the first branch; enlarge `tmp` to `char tmp[2048];` (32 devices × ~40 bytes exceeds 1024) and select `render_partitions` for it.
- `procfs_readdir` root: add `"partitions"` to `root_files` and `PROC_INO_PARTITIONS` to `root_inos` **before** `"self"` (so `"self"` stays last and keeps its special case), and change the `4`s to `5` (`index < 5`, `index == 4` for self, `index -= 5`).

Confirm `put_str`'s exact signature at `procfs.c:66` matches the use above.

- [ ] **Step 3: Run the test**

Run: `make test 2>&1 | tail -3` → Expected: `PASS: ...`.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/procfs.c kernel/fs/vfs.c
git commit -m "feat(procfs): /proc/partitions (storage-01 task 6)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: userland `make blkdevtest`

**Files:**
- Create: `userland/blkdevtest.c`
- Modify: `Makefile` (target, in the `m0test` pattern at `Makefile:1373-1387`)

**Interfaces:**
- Consumes: everything user-visible from Tasks 1, 4, 5, 6 through musl.

- [ ] **Step 1: Write the test program**

```c
// blkdevtest -- storage-01 from userland, through musl: raw block
// nodes, their ioctls, unaligned I/O, fsync, /proc/partitions, and a
// 64-bit lseek. Prints "PASS blkdevtest" or a FAILED line.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define BLKGETSIZE    0x1260
#define BLKSSZGET     0x1268
#define BLKGETSIZE64  0x80081272UL

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("blkdevtest: FAILED: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    struct stat st;
    CHECK(stat("/dev/sda", &st) == 0, "stat /dev/sda errno=%d", errno);
    CHECK(S_ISBLK(st.st_mode), "/dev/sda not S_IFBLK mode=%o", st.st_mode);
    CHECK(major(st.st_rdev) == 8 && minor(st.st_rdev) == 0, "sda rdev %u:%u", major(st.st_rdev), minor(st.st_rdev));
    CHECK(stat("/dev/sdb", &st) == 0 && minor(st.st_rdev) == 16, "sdb minor");

    int fd = open("/dev/sda", O_RDONLY);
    CHECK(fd >= 0, "open /dev/sda errno=%d", errno);
    uint64_t sz = 0; int ss = 0; unsigned long secs = 0;
    CHECK(ioctl(fd, BLKGETSIZE64, &sz) == 0 && sz == 64ULL << 20, "BLKGETSIZE64 %llu", (unsigned long long)sz);
    CHECK(ioctl(fd, BLKSSZGET, &ss) == 0 && ss == 512, "BLKSSZGET %d", ss);
    CHECK(ioctl(fd, BLKGETSIZE, &secs) == 0 && secs == (64UL << 20) / 512, "BLKGETSIZE %lu", secs);
    unsigned char s0[512];
    CHECK(pread(fd, s0, 512, 0) == 512 && s0[510] == 0x55 && s0[511] == 0xAA, "sda LBA0 boot signature");
    CHECK(lseek(fd, 0, SEEK_END) == (off_t)sz, "SEEK_END");
    CHECK(read(fd, s0, 1) == 0, "read at end not EOF");
    CHECK(lseek(fd, (off_t)sz + 1, SEEK_SET) == -1 && errno == EINVAL, "seek past end not EINVAL");
    close(fd);

    // Non-destructive unaligned round trips near the end of sdb: one
    // crossing a sector boundary, one large enough to take the aligned
    // multi-sector path in the middle.
    fd = open("/dev/sdb", O_RDWR);
    CHECK(fd >= 0, "open /dev/sdb errno=%d", errno);
    uint64_t sz2 = 0;
    ioctl(fd, BLKGETSIZE64, &sz2);
    struct { off_t off; size_t len; } cases[] = { { (off_t)sz2 - 4096 + 400, 300 }, { (off_t)sz2 - 16384 + 100, 9000 } };
    static unsigned char saved[9000], pat[9000], back[9000];
    for (int c = 0; c < 2; c++) {
        size_t n = cases[c].len;
        for (size_t i = 0; i < n; i++) { pat[i] = (unsigned char)(i * 13 + c); }
        CHECK(pread(fd, saved, n, cases[c].off) == (ssize_t)n, "case %d save", c);
        CHECK(pwrite(fd, pat, n, cases[c].off) == (ssize_t)n, "case %d pwrite", c);
        CHECK(fsync(fd) == 0, "case %d fsync errno=%d", c, errno);
        CHECK(pread(fd, back, n, cases[c].off) == (ssize_t)n && memcmp(back, pat, n) == 0, "case %d readback", c);
        CHECK(pwrite(fd, saved, n, cases[c].off) == (ssize_t)n, "case %d restore", c);
    }
    close(fd);

    // /proc/partitions lists both disks.
    char pb[2048] = {0};
    fd = open("/proc/partitions", O_RDONLY);
    CHECK(fd >= 0 && read(fd, pb, sizeof pb - 1) > 0, "read /proc/partitions");
    if (fd >= 0) { close(fd); }
    CHECK(strncmp(pb, "major minor  #blocks  name", 26) == 0, "partitions header");
    CHECK(strstr(pb, " sda\n") && strstr(pb, " sdb\n"), "partitions list:\n%s", pb);

    // A 64-bit offset through musl and the shim, not truncated to 32 bits.
    fd = open("/tmp/blkdevtest.big", O_CREAT | O_RDWR, 0644);
    off_t five = (off_t)5 << 30;
    CHECK(fd >= 0 && lseek(fd, five, SEEK_SET) == five, "lseek 5GiB");
    CHECK(lseek(fd, 0, SEEK_CUR) == five, "SEEK_CUR after 5GiB");
    if (fd >= 0) { close(fd); unlink("/tmp/blkdevtest.big"); }

    if (fails == 0) { printf("PASS blkdevtest\n"); }
    return fails ? 1 : 0;
}
```
(`printf` goes to the console; if other solo tests write to `/dev/kmsg` to reach the serial log, mirror what `userland/m0test.c` does — read it first and copy its output setup.)

- [ ] **Step 2: Add the make target**

After the `m0test` target in `Makefile`:

```make
# blkdevtest -- storage-01 from userland: /dev/sdX nodes, block ioctls,
# unaligned raw I/O, fsync, /proc/partitions, 64-bit lseek. Booted alone.
$(BUILD_DIR)/blkdevtest.elf: $(USERLAND_DIR)/blkdevtest.c
	@mkdir -p $(BUILD_DIR)
	$(NEOOS_HOSTED)gcc -O2 -static -o $@ $(USERLAND_DIR)/blkdevtest.c

.PHONY: blkdevtest
blkdevtest: iso disk-image $(BUILD_DIR)/blkdevtest.elf
	./tools/nexify.sh $(BUILD_DIR)/blkdevtest.elf $(BUILD_DIR)/blkdevtest.nex
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/blkdevtest.nex ::blkdevtest.nex
	@printf '%s\n' '# generated by `make blkdevtest`' 'wait /blkdevtest.nex' > $(BUILD_DIR)/disk-src/INITTAB.solo
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/INITTAB.solo ::etc/inittab
	@tools/boot_until.sh $(BUILD_DIR)/blkdevtest.log '^PASS blkdevtest|FAILED' $(BOOT_TIMEOUT) -- $(QEMU_COMMON) -display none
	@grep -aE '^blkdevtest:|^PASS blkdevtest|PANIC|\[exception\]' $(BUILD_DIR)/blkdevtest.log || true
	@grep -aq '^PASS blkdevtest' $(BUILD_DIR)/blkdevtest.log || { echo "BLKDEVTEST FAILED"; exit 1; }
```
Add `blkdevtest` to the `.PHONY` list at `Makefile:69` if other solo targets are listed there.

- [ ] **Step 3: Run it**

Run: `make blkdevtest`
Expected: `PASS blkdevtest`. If a check fails, fix the kernel side (not the test) unless the test contradicts the spec.

- [ ] **Step 4: Commit**

```bash
git add userland/blkdevtest.c Makefile
git commit -m "test: make blkdevtest -- raw block devices from userland (storage-01 task 7)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Docs, ABI refresh, full verification

**Files:**
- Modify: `docs/stdlib.md` (lines ~75-77 mount; ~1880-1904 stat table; the 32-bit position notes; new "Block devices" section), `docs/abi-compatibility.md` (lines ~60, 146, 357, 420, 1395 and a new block-device section), `kernel/fs/stat.h:65-69` comment (FAT now keeps timestamps)

- [ ] **Step 1: docs/stdlib.md**

- `mount(2)`: `source` for `"fat"` is a block device path (`/dev/sda`, `/dev/sda1`, or the bare name); `hd0`/`hd1` are gone (no compatibility alias — nothing used them). Source strings up to 63 bytes.
- New section **Block devices** covering: names (`sdX`, partitions `sdXN`, `nvmeXnYpN` rule), majors/minors (8 / `idx*16+part`, 259 beyond partition 15 and for NVMe), `S_IFBLK|0660` with `st_size` 0 and `st_rdev`, byte-granular read/write with read-modify-write of partial sectors, `lseek` bounded by the device size (`EINVAL` past it), ioctls `BLKGETSIZE64`/`BLKSSZGET`/`BLKGETSIZE` (others `ENOTTY`), `fsync` flushes the device cache, `/proc/partitions` format.
- Divergences, each with its reason: no `BLKRRPART` (partitions are scanned once at registration); extended/logical MBR partitions ignored; FAT refuses devices whose sector size is not 512; no `O_DIRECT` (accepted flag is ignored on block nodes: all I/O goes through the write-through cache); mode 0660 reported, not enforced (no permission model yet — storage-06).
- File positions: remove the note that positions/sizes are 32-bit; record that FAT caps a file at 4 GiB − 1 (`EFBIG`, as Linux vfat) and ramfs at 16 KiB (`EFBIG`).
- The stat table: timestamps come from FAT directory entries (the "timestamps are 0" text is stale).

- [ ] **Step 2: docs/abi-compatibility.md**

Refresh the sections listed above: what is implemented (block nodes, partitions, 64-bit offsets), stubbed (BLKRRPART, O_DIRECT), diverging (as in stdlib.md), and what a ported tool would still hit (`fdisk` can read and write tables but the kernel will not rescan until reboot; `mkfs.*`/`e2fsck` work on raw nodes). Fix the stale "timestamps are 0" line at ~146.

- [ ] **Step 3: Full verification**

```bash
make test 2>&1 | tail -3
make blkdevtest 2>&1 | tail -3
make m0test 2>&1 | tail -3
make sqlitetest 2>&1 | tail -3
tools/gauntlet.sh
```
Run these one at a time, nothing else building meanwhile (the gauntlet needs an exclusive build). Expected: every target prints its PASS line; the gauntlet reports 15/15 with zero retries (the project bar). If the gauntlet shows a flaky failure, compare it against the known baseline flakes before attributing it to this work, and report it either way.

- [ ] **Step 4: Commit**

```bash
git add docs kernel/fs/stat.h
git commit -m "docs: storage-01 -- block devices, partitions, 64-bit offsets in stdlib and ABI notes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
