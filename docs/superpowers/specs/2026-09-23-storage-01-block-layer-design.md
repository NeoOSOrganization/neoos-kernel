# storage-01: block device layer, partitions, 64-bit file positions

Date: 2026-09-23. Status: **approved in brainstorming**, awaiting spec
review. Series: [storage-00 roadmap](2026-09-23-storage-00-roadmap.md).

## Goal

Put a real block-device layer between filesystems and disk controllers,
so that nothing above a controller driver knows which controller a disk
is on, disks are named the way Linux names them, partitions are found by
reading the disk, and user programs can use a disk the way `fdisk`,
`mkfs` and `dd` expect. At the same time, remove the 32-bit limit on
file positions and sizes everywhere it exists.

Afterwards the machine boots exactly as today (FAT16 on the first IDE
disk at `/`, FAT32 on the second at `/mnt`), but via `/dev/sda` and
`/dev/sdb`.

## Non-goals

AHCI and NVMe (storage-02/03). Filesystem registry, probing, `"auto"`,
`root=`, fstab (storage-04). GPT-partitioned images and ext2
(storage-05). Partition rescan (`BLKRRPART`), extended/logical MBR
partitions, `O_DIRECT`, a page cache, interrupt-driven I/O.

## 1. Block device core — `kernel/block/blockdev.{c,h}`

```c
struct blockdev;

struct blockdev_ops {
    // Return 0 or a negative errno. `count` sectors of `sector_size`.
    int (*read)(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blockdev *d);
};

struct blockdev {
    char     name[16];          // "sda", "sda1", "nvme0n1p1"
    uint32_t sector_size;       // logical: 512 or 4096
    uint64_t sector_count;
    const struct blockdev_ops *ops;
    void    *priv;              // driver state
    struct blockdev *parent;    // whole disk, for a partition; NULL otherwise
    uint64_t start_lba;         // offset inside parent (0 for a whole disk)
    uint32_t major, minor;      // Linux numbering, see §4
    uint8_t  part_type[16];     // GPT type GUID; MBR type in [0]; else zero
    uint8_t  part_uuid[16];     // GPT partition GUID; else zero
};
```

- **Registry**: a static table of `BLOCKDEV_MAX = 32` entries under a
  spinlock at `LOCK_RANK_BLOCKDEV` (7), taken only to register or look
  up, never across I/O.
- `int blockdev_register_disk(struct blockdev *d)` — a controller driver
  calls it for each whole disk. It assigns major/minor, creates the
  `/dev` node (§4), then runs the partition scan (§2), which registers
  each partition through the internal `blockdev_register_part()`.
- `int blockdev_alloc_name(const char *prefix, char out[16])` — `"sd"`
  yields `sda`, `sdb`, … `sdz` (no `sdaa` — 26 is far beyond any
  NeoOS machine); `"nvme"` is handled by the NVMe driver itself
  (`nvme<ctrl>n<ns>`), so this call is `sd`-only.
- Partition names: `<disk><n>`, with a `p` separator when the disk name
  ends in a digit (`sda1`, `nvme0n1p1`) — Linux's rule.
- Lookup: `blockdev_find(name)`, `blockdev_find_path(path)` (accepts
  `"/dev/sda1"` or `"sda1"`), `blockdev_foreach(cb, arg)`.
- A partition's ops are generic: bounds-check `lba + count` against its
  `sector_count` (`-EIO` when out of range), add `start_lba`, call the
  parent. `flush` forwards to the parent.
- Helpers used by everything above the drivers:
  `blockdev_whole(d)` (itself or its parent) and
  `blockdev_disk_lba(d, lba)` (translates to the whole-disk LBA).

### Legacy ATA as a blockdev driver

`kernel/drivers/block/ata.c` gains `void ata_probe(void)`: IDENTIFY
master and slave on the primary channel; for each drive present,
register a blockdev named via `blockdev_alloc_name("sd")`,
`sector_size = 512`. Its ops split requests into ≤255-sector PIO
commands and reject LBAs above 2^28 (LBA28, what the driver speaks) with
`-EIO`. `flush` issues FLUSH CACHE (today's write path already flushes
after every write; that stays, so `flush` is cheap and correct).
`ata_read_sectors`/`ata_write_sectors`/`ata_identify` become `static`;
`ata.h` shrinks to `ata_init`, `ata_probe`, and `ata_lock` (still needed
by `blkcache_selftest`'s rank assertion — if that assertion moves to
blockdev terms, `ata_lock` goes static too).

Boot order in `kmain`: `ata_init()`, `blkcache_init()`, `blockdev_init()`,
`ata_probe()` (later: AHCI and NVMe probes run before it), then
`vfs_init()` and mounts. `devfs` must accept block-node registration
before it is mounted — `devfs_register_blk` only fills its table, so
this already holds; verified during planning.

## 2. Partition scanning — `kernel/block/part.c`

`void part_scan(struct blockdev *disk)`, run once per whole disk from
`blockdev_register_disk`.

1. **GPT.** Read LBA 1. On signature `EFI PART`: verify the header CRC32
   (over `HeaderSize` bytes, CRC field zeroed) and the entry-array CRC32.
   If either fails, read the backup header at the LBA the primary names
   (or `sector_count - 1` if the primary is unreadable garbage) and use
   it if valid. For each entry with a non-zero type GUID (up to
   `NumberOfPartitionEntries`, capped at 128): register partition *n*
   (1-based entry index, so gaps keep their numbers, as Linux does) with
   `FirstLBA`, `LastLBA - FirstLBA + 1` sectors, the type GUID and the
   partition GUID.
2. **MBR.** If no valid GPT: read LBA 0; require `0x55 0xAA` at 510.
   If any entry is type `0xEE` (protective), log
   `part: <disk>: protective MBR but no valid GPT` and register nothing.
   Otherwise register each non-empty primary entry *n* (1–4), MBR type
   in `part_type[0]`. Types `0x05`/`0x0F`/`0x85` (extended) are logged
   and skipped.
3. **Neither**: register nothing; the whole disk remains usable. Today's
   FAT images take this path.

Validation: an entry with a zero length, extending past the disk's end,
or overlapping an earlier accepted entry is logged
(`part: <disk>: entry <n> rejected: <reason>`) and skipped. Each
accepted partition logs `part: <name> start=<lba> sectors=<n>`.

CRC32 is the standard reflected polynomial `0xEDB88320`. `kernel/lib/`
has none today, so it gains `crc32.{c,h}` (table-driven) with a
known-answer check in `part_selftest`.

## 3. Block cache — `kernel/fs/blkcache.{c,h}` rework

- Key: `(whole-disk blockdev *, whole-disk LBA)`. A read through `sda1`
  and one through `sda` of the same sector hit the same entry; the
  partition offset is applied before lookup.
- Entries hold up to 4096 bytes (`BLKCACHE_MAX_SECTOR`) and record the
  device's sector size. `BLKCACHE_ENTRIES` stays 128 → 512 KiB,
  allocated from the PMM at `blkcache_init` rather than grown in `.bss`.
- API (one logical sector per call, as today; returns 0 or `-errno`,
  changing from today's 1/0 convention — every call site is touched
  anyway):
  - `int blkcache_read(struct blockdev *d, uint64_t lba, void *out)`
  - `int blkcache_write(struct blockdev *d, uint64_t lba, const void *in)`
  - `int blkcache_read_multi(struct blockdev *d, uint64_t lba, uint32_t count, void *out)`
  - `int blkcache_write_multi(struct blockdev *d, uint64_t lba, uint32_t count, const void *in)`
  - `void blkcache_invalidate(struct blockdev *d)` — every entry of `d`'s
    whole disk.
- The `_multi` calls go straight to the device for the whole range (one
  command, not `count` cache round trips) and keep the cache coherent:
  a multi-write updates any cached entries in range; a multi-read
  serves nothing from the cache but is correct because the cache is
  write-through (no dirty entries can exist).
- Still write-through; the header comment's rationale stands.
- Lock ranks unchanged: cache lock BLOCKDEV (7), driver locks DRIVER
  (8), device I/O done with the cache lock dropped, as today.

## 4. Raw block nodes — devfs

- New `enum vnode_type` member `VNODE_BLOCK`. `vfs_stat_vnode` reports
  `S_IFBLK | 0660`, `st_rdev = makedev(major, minor)`, `st_size = 0`
  (Linux reports 0 for block nodes; the size comes from the ioctl).
  `stat.h` gains `S_IFBLK` (`0060000`, Linux's value).
- `int devfs_register_blk(struct blockdev *d)` — called by
  `blockdev_register_disk`/`_part`. Node name = `d->name`.
- Numbering (Linux's):
  - `sd`: major 8, minor `disk_index * 16 + part` (part 0 = whole disk).
    Partitions 16 and above use major 259 with the next free minor, as
    Linux's `blkext` does.
  - NVMe (storage-03): major 259, minors allocated in registration
    order.
- File ops on an opened block node (`kernel/block/blkdev_file.c`):
  - `read`/`write` at any byte offset and length. Whole aligned sectors
    go through `blkcache_*_multi`; an unaligned head or tail sector does
    read-modify-write through `blkcache_read`/`blkcache_write`.
    Reads at or past the end return 0; writes past the end return
    `-ENOSPC` (Linux), a write that crosses the end is short.
  - `lseek`: `SEEK_SET`/`SEEK_CUR`/`SEEK_END` (end = device size in
    bytes). Negative results → `-EINVAL`.
  - `ioctl`: `BLKGETSIZE64` (`0x80081272`, `uint64_t` bytes),
    `BLKSSZGET` (`0x1268`, `int` sector size), `BLKGETSIZE` (`0x1260`,
    `unsigned long` count of 512-byte units). Anything else `-ENOTTY`.
  - `fsync`/`fdatasync` on a block fd call `ops->flush`. (Today these
    syscalls return 0 unconditionally; they gain a per-file hook —
    `file_ops.fsync`, nullable, NULL keeps today's behaviour.)
  - I/O runs under the global `fs_lock`, like all file I/O today.
- No permission model exists yet (storage-06), so `0660` is reported but
  not enforced; that is the existing, documented state for all files.

## 5. `/proc/partitions`

procfs gains `/proc/partitions` in Linux's format:

```
major minor  #blocks  name

   8        0      65536 sda
   8        1      65535 sda1
```

`#blocks` is the size in 1 KiB units. One line per registered blockdev,
in registration order.

## 6. FAT on blockdevs

- `fatfs_mount_op` resolves its source with `blockdev_find_path()`;
  unknown source → `-ENODEV`, a blockdev whose `sector_size != 512` →
  `-EINVAL` (FAT's BPB would allow larger sectors; this driver assumes
  512 throughout, and that stays a recorded limit).
- `struct fat_volume` holds `struct blockdev *bdev` instead of
  `uint8_t drive`; every `blkcache_*` call site (≈98) follows. The
  legacy `fat16_*` path binds to `blockdev_find("sda")`.
- `"hd0"`/`"hd1"` stop being accepted (roadmap D1).
- `kernel.c`: `vfs_mount_fs("/dev/sda", "/", "fat")`,
  `vfs_mount_fs("/dev/sdb", "/mnt", "fat")`. Still hard-coded;
  storage-04 removes that.
- `sys_mount` source length limit (16 today) rises to 64 so
  `/dev/nvme0n1p1`-style paths fit.

## 7. 64-bit file positions and sizes

Everything that carries a file offset or size becomes 64-bit:

- `struct file_descriptor.position` (`kernel/sched/proc.h:46`):
  `uint32_t` → `uint64_t`. Includes `sys_file.c:840,856` (`saved`),
  `memfd.c`, `vesafb.c`, `fd_table.c`, `flock.c`, `file.c`.
- `struct vnode.size` (`vfs.h:72`) → `uint64_t`.
- `vfs_ops.read`/`write`: `uint32_t pos` → `uint64_t pos`
  (`len` stays `uint32_t`; syscalls already clamp a single transfer).
- Every filesystem adapts: fatfs (reject a write that would take a file
  past `0xFFFFFFFF` with `-EFBIG`; its internals stay 32-bit), ramfs
  (existing cap → `-EFBIG`), embedfs, procfs, devfs.
- Direct `ops->read` callers: ELF loader (`kernel/sched/proc.c:246,298`),
  file-backed mmap faults (`kernel/mm/vma.c:474`).
- Syscalls audited for residual truncation: `lseek`, `pread64`,
  `pwrite64`, `ftruncate`, `sendfile`, `mmap` offset, `stat`/`statx`
  `st_size`, `getdents64` `d_off`, `fcntl` locks (`flock.c` ranges).
- `lseek` result above `INT64_MAX` or negative → `-EINVAL`; on a
  regular file, a seek past the filesystem's max file size is allowed
  (Linux allows it; the later write fails with `-EFBIG`).

## 8. Testing

Boot selftests (each prints a PASS/FAILED line; `make test` fails on
FAILED, and the PASS lines become `CORE_REQUIRED_MARKERS`):

- `blockdev_selftest` — a RAM-backed test blockdev (not registered in
  `/dev`, name `ram0`, freed after): naming (`sd` sequence, digit-suffix
  `p` rule), `find_path`, partition bounds (`-EIO` past end),
  partition-to-disk LBA translation.
- `part_selftest` — builds images in a RAM blockdev: valid GPT (2
  entries, one gap index), GPT with corrupt primary + valid backup,
  GPT with corrupt both + protective MBR (→ no partitions), MBR with 2
  primaries + 1 extended (extended skipped), overlapping entry
  (rejected), entry past end (rejected), bare disk (none).
- `blkcache_selftest` (extended) — same sector via partition and whole
  disk is one entry; a 4096-byte-sector RAM device round-trips;
  `_multi` write updates a cached entry.
- `fpos64_selftest` — ramfs file: `lseek` to 5 GiB returns 5 GiB; a
  `write` there fails `-EFBIG` without wrapping to a low offset; raw
  RAM block node: `lseek(SEEK_END)` returns its size exactly.

Userland `make blkdevtest` (`userland/blkdevtest.c`, solo boot in the
`make m0test` pattern):

- `stat("/dev/sda")`: `S_ISBLK`, `major == 8`, `minor == 0`.
- `ioctl(BLKGETSIZE64)` equals the image size (64 MiB);
  `BLKSSZGET` = 512.
- Read LBA 0 of `/dev/sda`: bytes 510–511 are `55 AA` (FAT boot sector).
- On `/dev/sdb`, non-destructively: `pread` 300 bytes at the unaligned
  offset `size - 4096 + 100` (spanning two sectors) and keep them;
  `pwrite` a pattern there, `fsync` (returns 0), `pread` it back and
  compare; `pwrite` the saved bytes back. The FAT32 volume is unchanged
  afterwards wherever that offset lands.
- `lseek(fd, 0, SEEK_END)` equals the size; `lseek` to `size + 1`
  followed by `read` returns 0.
- `/proc/partitions` lists `sda` and `sdb`.
- prints `PASS blkdevtest`.

`make test` and the gauntlet (15/15 zero-retry) must pass unchanged in
behaviour; the new selftest markers are added to `CORE_REQUIRED_MARKERS`.

## 9. ABI, docs, os-builder

- musl shim: no new entries expected — `ioctl`, `lseek`, `pread64`,
  `pwrite64`, `fsync` already forward. `blkdevtest` also `lseek`s a
  `/tmp` (ramfs) file to 5 GiB from userland and checks the returned
  offset, proving the shim forwards 64-bit offsets intact.
- `docs/stdlib.md`: block device nodes (majors/minors, ioctls, `S_IFBLK`,
  `st_size = 0`); `mount(2)` sources are block device paths, `hd0/hd1`
  gone; `/proc/partitions`; divergences recorded: no `BLKRRPART`,
  extended/logical MBR partitions ignored, FAT needs 512-byte sectors,
  no `O_DIRECT`, block node mode `0660` not enforced; the 32-bit file
  position divergence is removed.
- `docs/abi-compatibility.md`: refreshed at the end (block devices,
  partitions, 64-bit offsets; stale "timestamps are 0" lines fixed while
  there, since fatfs reports real ones).
- `storage-00` roadmap: "Known constraints" entry about 32-bit
  positions removed (done here).
- neoos-os-builder: no change (images and QEMU flags unchanged until
  storage-02).

## Decision log

| decision | reason |
|---|---|
| Linux names/majors, `hd0` dropped without alias | names are ABI-visible (roadmap D1); no application used `hd0` |
| cache keyed by whole disk | a partition and its disk must never hold two differing copies of one sector |
| 4 KiB cache entries | NVMe namespaces may be 4Kn; one entry size keeps the pool simple (512 KiB total) |
| `_multi` bypasses the cache | large transfers (raw `dd`, ext2 blocks) would evict the metadata the cache exists for |
| raw node I/O with RMW edges | Linux's buffered block devices accept any offset; `dd`/`fdisk` rely on it |
| 64-bit widening done here | raw nodes would otherwise stop at 4 GiB, and ext2 needs it next; user asked for it |
| GPT before MBR, protective-only → nothing | a GPT disk's MBR is a placeholder; trusting it would expose one bogus whole-disk partition |
| extended MBR skipped | no NeoOS image uses it; logged, recorded as a divergence |
