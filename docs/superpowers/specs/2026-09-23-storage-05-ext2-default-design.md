# storage-05: ext2 driver, and ext2 as the default filesystem

Date: 2026-09-23. Status: **approved in brainstorming**. Series:
[storage-00 roadmap](2026-09-23-storage-00-roadmap.md). Taken **before**
storage-03 (NVMe, deferred) and storage-04 (fs registry, `root=`,
fstab), at the user's direction.

## Goal

Add a complete ext2 (revision 1) read/write driver, and make it the
filesystem both disks use by default. Everything the FAT images carry
today moves onto GPT-partitioned ext2 images. FAT (FAT16/FAT32) stays
compiled in and mountable, and keeps its own tests (`make fattest`),
but no default boot mounts it.

Afterwards:

| mount | device | filesystem | controller |
|---|---|---|---|
| `/` | `/dev/sda1` | ext2 | AHCI port 0 |
| `/mnt` | `/dev/sdb1` | ext2 | AHCI port 2 (moved off IDE) |

## Decisions taken in brainstorming

1. **"Full ext2" means the complete on-disk format, not Unix semantics**
   (choice A).
   - The driver reads and writes everything a `mke2fs -t ext2` volume
     holds, and preserves modes, owners, link counts and symlinks
     correctly.
   - `stat` reports the real mode, link count, uid and gid. Path lookup
     follows symlinks, and `readlink(2)` returns their target.
   - **Not here:** `chmod`, `chown`, `link`, `symlink`, `utimensat`.
     These are storage-06, and new files get fixed defaults (§2.6).
2. **Mounts stay hard-coded** in `kernel.c`; only the type and device
   change (choice A). The registry, auto-detection, `root=` and fstab
   are storage-04.
3. **GPT, one Linux-filesystem partition per disk** (roadmap D2).
4. **`disk2.img` moves to AHCI port 2.** Legacy IDE carries only the
   GRUB boot CD in the default boot. Legacy ATA keeps a test
   (`make atatest`).

## Non-goals

- storage-06 syscalls (see above).
- Journaling (ext3): a volume whose `has_journal` or `needs_recovery`
  flag demands replay is refused. `has_journal` without recovery is a
  compat feature; the driver ignores the journal inode, as Linux's
  ext2 does.
- Extended attributes: they are never read or written. An xattr block
  is freed when its last inode goes away, because otherwise it leaks
  space and `e2fsck` complains.
- Quotas, the filesystem registry (storage-04), NVMe (storage-03).
- `O_CREAT` through a dangling symlink creating the target (it gets
  `ENOENT`; recorded as a divergence).

## 1. VFS additions

### 1.1 Real Unix metadata in the vnode

`struct vnode` gains `uint32_t mode` (full `st_mode`, including the
type bits), `uint32_t nlink`, `uint32_t uid` and `uint32_t gid`.

- `vfs_stat_vnode` uses them when `mode != 0`. Otherwise it falls back
  to today's synthesised values, which is what FAT, ramfs, devfs,
  procfs and embedfs keep getting.
- `statx`, `fstat` and `newfstatat` all go through `vfs_stat_vnode`,
  so they follow.
- `st_blocks` for ext2 comes from `i_blocks` (512-byte units, Linux's
  meaning), stored in a new `uint64_t blocks512` vnode field (0 means
  "derive from size", as today). `st_blksize` is the filesystem block
  size (a new `uint32_t blksize` field, 0 → 512).

### 1.2 Symlinks

- **Type and op.** A new `enum vnode_type` value, `VNODE_SYMLINK`, and
  a new op, `int (*readlink)(struct vnode *vn, char *buf, uint32_t
  len)`, which returns the byte count (no NUL) or -errno. Every
  existing driver gets a stub returning `-EINVAL` (the "no op pointer
  is ever NULL" rule).
- **Resolution** (`resolve_walk`) keeps the canonical path of the
  prefix it has walked. When a component resolves to a symlink that is
  intermediate, or final when the caller follows:
  1. Build `target + "/" + rest` (absolute target) or
     `walked-parent + "/" + target + "/" + rest` (relative target).
  2. Canonicalise it with `vfs_path_canonicalise`, so `..` stays
     textual, as today.
  3. Restart resolution from `mount_for`, so a symlink can cross
     mounts.
  4. After 40 expansions, fail with `ELOOP` (Linux's `MAXSYMLINKS`).
- **New entry points:**
  - `vfs_resolve_nofollow(path, &err)`: the final component is not
    followed.
  - `vfs_resolve_parent` never follows the final component, but does
    follow intermediate ones.
- **Syscalls:**

  | syscall | behaviour |
  |---|---|
  | `lstat`, `newfstatat(AT_SYMLINK_NOFOLLOW)`, `statx(AT_SYMLINK_NOFOLLOW)` | use `vfs_resolve_nofollow` |
  | `readlink` | nofollow resolve; `EINVAL` if not a symlink; otherwise the target, truncated to `bufsiz` without a NUL (Linux) |
  | `open` with `O_NOFOLLOW` (`0400000`) | `ELOOP` when the final component is a symlink |
  | `open` without `O_NOFOLLOW` | follows |
  | `unlink` and `rename` | act on the link itself (already true: they use `vfs_resolve_parent`) |

- **`getdents64`** reports `DT_LNK` for symlinks.

### 1.3 Delete on last close: an `evict` op

Today the VFS has no unlink-while-open semantics: FAT frees a file's
clusters at unlink, even with an fd still on it. For ext2 that would
let a later write through that fd allocate into a freed, and possibly
reused, inode.

- `struct vfs_ops` gains `void (*evict)(struct vnode *vn)`. `vnode_put`
  calls it (with no VFS locks held beyond `fs_lock`, which every
  caller of the fs ops already holds) when a vnode's refcount reaches
  0, before the vnode is freed. Every existing driver gets a no-op
  stub.
- ext2's `unlink`:
  1. `vnode_get`s the victim, which materialises it if it is not
     cached.
  2. Removes the directory entry and decrements `i_links_count` (and
     the vnode's `nlink`).
  3. `vnode_put`s it.

  ext2's `evict` frees the inode and its blocks when `nlink == 0`. So
  the delete happens at unlink when nothing else holds the file, and
  at the last close otherwise, as on Linux. A replaced rename target
  takes the same path.

### 1.4 statfs

`struct vfs_ops` gains `int (*statfs)(struct vfs_mount *m, struct
vfs_statfs *out)`.

- ext2 fills in the real values: `f_type = 0xEF53`, the block size,
  total, free and available blocks (free minus reserved), total and
  free inodes, and a 255-byte `f_namelen`.
- Every other driver returns `-ENOSYS`, and `sys_statfs` then falls
  back to today's pmm-based answer.

## 2. The ext2 driver — `kernel/fs/ext2/`

| file | responsibility |
|---|---|
| `ext2.h` | on-disk structures (superblock, group descriptor, inode, dir entry), in-memory volume state, internal API |
| `super.c` | mount/umount, feature policy, group descriptor table, superblock and GDT write-back, `statfs` |
| `inode.c` | read/write inodes, logical→physical block mapping (direct, single, double, triple indirect), block and inode allocation/free via bitmaps, truncate (including freeing indirect blocks), sparse holes |
| `dir.c` | lookup, readdir, add entry, remove entry, mkdir, rmdir, rename, `dir_index` handling |
| `file.c` | read, write, truncate_to, symlinks (fast and slow), `readlink` |
| `ops.c` | `ext2_ops` (the `vfs_ops` table) gluing the above to vnodes |
| `selftest.c` | hermetic selftest on an embedded image (§5.1) |

### 2.1 Mount and feature policy

- **Validation.** Read the superblock at byte 1024 of the device.
  `s_magic` must be `0xEF53` and `s_rev_level` at most 1 (revision 0
  is accepted, with its 128-byte inodes and first inode 11). The block
  size is `1024 << s_log_block_size` and must be 1, 2 or 4 KiB. The
  inode size must be a power of two between 128 and the block size.
- **Features:**
  - **incompat:** only `FILETYPE` (0x2) is allowed. Anything else
    (`COMPRESSION`, `RECOVER`, `JOURNAL_DEV`, `META_BG`, `EXTENTS`,
    `64BIT`, `MMP`, `FLEX_BG`, …) means `-EINVAL` and
    `[ext2] <dev>: unsupported incompat features 0x…`. Directories
    written by a filesystem without FILETYPE are still read, with
    `d_type` then derived from the inode.
  - **ro_compat:** `SPARSE_SUPER` (0x1) and `LARGE_FILE` (0x2) are
    allowed. Anything else (`HUGE_FILE`, `GDT_CSUM`, `DIR_NLINK`,
    `EXTRA_ISIZE`, `METADATA_CSUM`, …) means `-EINVAL`: the driver has
    no read-only mount mode to fall back on.
  - **compat:** all accepted. `DIR_INDEX` directories are read
    linearly, and the `EXT2_INDEX_FL` (0x1000) inode flag is cleared on
    the first modification of such a directory, exactly as Linux's ext2
    driver does (the htree root block is a valid, if large, linear
    entry). `RESIZE_INODE`: inode 7's blocks are already marked in use
    in the bitmaps and are never touched. `EXT_ATTR`: see Non-goals.
- **State.** If `s_state` lacks VALID, or has ERROR set, log a warning
  and mount anyway (Linux's ext2 does too). On mount: clear VALID, bump
  `s_mnt_count`, set `s_mtime`, and write the superblock. On umount:
  set VALID and write it again.
- **Group descriptors** are read whole into memory: 32 bytes each,
  starting in the block after the superblock's block. They are written
  back (the primary copy only, as Linux does) whenever free counts
  change. `s_free_blocks_count` and `s_free_inodes_count` are kept in
  step.
- **Claiming the device.** `blockdev_claim` on mount and
  `blockdev_release` plus `blkcache_invalidate` on umount, as FAT does.

### 2.2 Inodes and vnode ids

- **Vnode id = inode number**, except that inode 2 (the root) maps to
  vnode id 0, which the VFS reserves for every mount's root.
  `lookup("..")` in the root, or an entry that names inode 2, returns
  0. Ids are stable, so `vfs_vnode_rekey` is never needed.
- **Reading an inode** (`read_inode`) fills in the type from
  `i_mode & 0xF000`: DIR → `VNODE_DIR`, LNK → `VNODE_SYMLINK`, REG →
  `VNODE_FILE`, anything else → `VNODE_FILE` with its real mode (FIFOs
  and device nodes on disk are not opened as devices).
- **The other fields:**
  - size: `i_size`, plus `i_size_high` for regular files
    (`i_dir_acl` in revision 1 with `LARGE_FILE`)
  - the three timestamps
  - mode, nlink, uid and gid (the 16-bit low halves plus the Linux
    osd2 high halves)
  - `blocks512` from `i_blocks`
  - `blksize` = the block size
- **Writing an inode back** (`sync_inode`, and after any change) writes
  the whole on-disk record, leaving fields the driver does not model
  untouched: it reads the record, modifies it, and writes it.

### 2.3 Block mapping and allocation

- **Mapping.** `bmap(inode, lblk, alloc)` walks `i_block[0..11]`
  (direct), `[12]` (single indirect), `[13]` (double) and `[14]`
  (triple).
  - A zero pointer is a hole. Reading a hole gives zeros.
  - With `alloc`, missing indirect blocks and the data block are
    allocated and zeroed, and the inode's `i_blocks` goes up by
    `blocksize/512` for each block, counting indirect blocks, as Linux
    does.
- **Block allocation.** The goal is the block after the file's
  previous one, else the first block of the inode's group. Search that
  group's bitmap from the goal, then every other group in order.
  Updates the bitmap, the group's `bg_free_blocks_count` and the
  superblock count. No space left → `-ENOSPC`.
- **Inode allocation.** Directories go to the group with the most
  free inodes whose free-block count is at least average (a simplified
  Orlov). Files go to the parent's group, then the first group with a
  free inode. A new directory bumps the group's `bg_used_dirs_count`.
  Inodes below `s_first_ino` are never allocated. The new inode is
  zeroed before use.
- **Truncate** (to 0 or to a length) frees whole blocks past the new
  end, including indirect blocks that become empty, and zeroes the
  tail of a partial last block. It also sets `i_size`, `i_blocks` and
  `mtime`/`ctime`.
- **Delete** happens in `evict`, once the last link is gone and the
  last reference is released (§1.3): free all blocks, free the xattr
  block (`i_file_acl`) with refcount handling, set `i_dtime`, set
  `i_links_count` to 0, and clear the bit in the inode bitmap. A crash
  in between leaves an unattached inode, as it does with Linux's ext2,
  and `e2fsck` reclaims it.

### 2.4 Directories

- **Reading.** Directory blocks hold variable-length entries: inode,
  `rec_len`, `name_len`, file type, name. An entry with inode 0 is
  unused.
  - `readdir` walks entries by index, skipping `.`, `..` and unused
    entries, which matches what FAT's readdir returns today (the file
    layer adds nothing).
  - `lookup` compares names exactly: ext2 is **case-sensitive**.
- **Adding an entry.**
  1. Find an entry whose `rec_len` exceeds its own real length (8 +
     `name_len` rounded up to 4) by at least the new entry's length,
     or an unused entry that is large enough, and split it.
  2. Otherwise, append a new block to the directory, as one entry
     spanning the whole block.
  3. Set the file type byte when FILETYPE is on.
  4. Clear the directory's `INDEX_FL`.
- **Removing an entry.** Merge it into the previous entry's `rec_len`,
  or, for the first entry in a block, set its inode to 0.
- **mkdir.** Allocate an inode (mode `S_IFDIR | 0755`, links 2) and one
  block holding `.` and `..`. The parent's link count goes up by one.
- **rmdir.** Fails with `ENOTEMPTY` unless only `.` and `..` remain.
  The parent's link count goes down by one, and the inode is freed.
- **rename** (same mount, as the VFS requires). Existing-target rules
  match FAT's current behaviour and Linux's: a file replaces a file,
  an empty directory replaces a directory, and anything else gives
  `EISDIR`/`ENOTDIR`/`ENOTEMPTY`.
  - Moving a directory to a new parent rewrites its `..` and moves one
    link count from the old parent to the new one.
  - Renaming onto itself is a no-op.
  - Moving a directory into its own subtree gives `EINVAL`. This is
    checked by walking `..` up from the target parent.
- **unlink.** A directory gives `EISDIR`. Otherwise remove the entry,
  decrement `i_links_count`, and delete the inode at 0 links (§2.3).
  A symlink is unlinked like a file.

### 2.5 Files and symlinks

- **Read and write** map ranges block by block. Whole-block runs within
  one contiguous physical extent go through `blkcache_read_multi` /
  `blkcache_write_multi`; partial blocks are read, patched and written.
  - A write past `i_size` extends the file, leaving holes unallocated
    when it seeks past the end.
  - `mtime`/`ctime` are updated on write, and `atime` never (noatime,
    because it costs a write per read).
  - Setting `LARGE_FILE` the first time a file passes 2 GiB − 1 also
    sets the ro_compat bit in the superblock.
- **Size limit.** Files up to the triple-indirect limit are supported.
  The maximum is capped at `2^32 × blocksize` bytes of `i_blocks`
  accounting (`i_blocks` in 512-byte units must fit 32 bits). A write
  past that gives `EFBIG`.
- **Symlinks.**
  - `readlink`: a fast symlink (`i_blocks == 0` apart from an xattr
    block, and size < 60) stores its target in `i_block[]`; otherwise
    the target is in the first data block.
  - Symlinks are never created in this milestone (no `symlink(2)`);
    they only arrive on images built by `mke2fs -d` or `debugfs
    symlink`.

### 2.6 Defaults for newly created objects

`create` and `mkdir` have no mode argument in `vfs_ops` today:

| object | mode | uid/gid |
|---|---|---|
| file | `S_IFREG \| 0644` | 0/0 |
| directory | `S_IFDIR \| 0755` | 0/0 |

Timestamps are set from the RTC (the source FAT uses). **Divergence:**
`open(O_CREAT, mode)` and `mkdir(path, mode)` ignore `mode` (storage-06
wires it through).

## 3. Disk images

### 3.1 Layout

Both images are 64 MiB, with a GPT carrying one partition: type
`8300` (Linux filesystem), starting at sector 2048 (1 MiB) and
running to the end, built with `sgdisk`.

- The filesystem is made with `mke2fs -q -t ext2 -b 1024 -d <staging>
  -E offset=1048576 <img> <size-in-KiB>`.
- **Why `-b 1024`:** so that the default boot exercises single,
  double and triple indirect mapping on files of modest size. With
  1 KiB blocks, double indirect starts at 268 KiB and triple indirect
  at about 64 MiB.
- `mke2fs -t ext2` sets `ext_attr`, `resize_inode`, `dir_index`,
  `filetype` and `sparse_super`, all of which the driver must handle.

### 3.2 `tools/imgput.sh`

This replaces every mtools call in the Makefile (86 `mcopy`, 34 `mmd`,
1 `mdel`). Each subcommand runs through `debugfs -w
"<img>?offset=1048576"`:

| subcommand | does |
|---|---|
| `imgput.sh put <img> <host-file> <abs-path>` | copy in, replacing an existing file |
| `imgput.sh mkdir <img> <abs-path>` | `mkdir -p` semantics, silent when it exists |
| `imgput.sh rm <img> <abs-path>` | remove, silent when absent |
| `imgput.sh symlink <img> <abs-path> <target>` | create a symlink (fixtures only) |

A failed debugfs command makes the script exit non-zero. The offset is
a named constant in the script and in the Makefile (`EXT2_OFFSET`).

### 3.3 Contents

- **`disk.img`** (`/`): everything its FAT build carries today, in the
  same paths (all lower-case already). Its fixtures add:
  - `/usr/share/test/sym-rel` → `hello.txt`
  - `/usr/share/test/sym-abs` → `/usr/share/test/dir/nested.txt`
  - `/usr/share/test/loop-a` ↔ `loop-b` (an `ELOOP` pair)
  - `/usr/share/test/sparse.bin`: a 1 MiB file with only its last
    block written
- **`disk2.img`** (`/mnt`): the FAT32 image's contents (`fat32.txt`,
  `sub/f32nest.txt`). The file names stay, because tests read them, and
  the text becomes "Hello from the ext2 /mnt volume!".

### 3.4 FAT images for `make fattest`

- `build/fat16.img`: the old `disk.img` FAT16 recipe, fixtures only.
- `build/fat32.img`: the old `disk2.img` recipe.

Both are built with `mkfs.fat` plus mtools, which stays a dependency
of `fattest` only.

## 4. Boot, selftests and targets

### 4.1 `kernel.c`

`vfs_mount_fs("/dev/sda1", "/", "ext2")` and `("/dev/sdb1", "/mnt",
"ext2")`. The root line becomes `[boot] root: sda1 ext2 via ahci`,
plus a matching `[boot] /mnt: sdb1 ext2 via ahci`.

### 4.2 FAT boot selftests

- `fat16_mount` (the legacy pre-VFS path) looks for the first
  registered whole disk or partition with a valid FAT boot sector,
  instead of `sda`. When there is none it logs `[fat16] no FAT volume,
  legacy selftests skipped` and `fat16_selftest` /
  `fat16_write_selftest` return at once, with no FAILED.
- Under `fattest`, the FAT16 image is present and they run for real.

### 4.3 `vfs_selftest`

- Its fixtures move to ext2: `/usr/share/test/hello.txt`,
  `/mnt/fat32.txt` and `/mnt/sub/f32nest.txt` keep their names.
- New checks:
  - `sym-rel` and `sym-abs` resolve and read.
  - `vfs_resolve_nofollow` gives a `VNODE_SYMLINK`.
  - `loop-a` gives `-ELOOP`.
  - `stat` of `hello.txt` gives `S_IFREG` with a real mode, and nlink
    ≥ 1.
  - `statfs` of `/` gives `0xEF53`.

### 4.4 Required markers (`CORE_REQUIRED_MARKERS`)

Added:
- `[ext2] selftest passed`
- `[boot] root: sda1 ext2 via ahci`
- `[boot] /mnt: sdb1 ext2 via ahci`

Removed:
- `[fat16]` markers, if any are required.
- `[boot] root: sda via ahci`

### 4.5 `make test`: host `e2fsck` after the boot

After QEMU exits, `make test` runs `e2fsck -fn "<img>?offset=1048576"`
on both images. Any non-zero exit fails the test with
`EXT2 CONSISTENCY FAILED` and the `e2fsck` output. The gauntlet runs
the same check on each run's disk copies before deleting them.

### 4.6 Targets

- **`make fattest`:** the default layout, plus `fat16.img` on
  `sata.3` and `fat32.img` on `sata.4`, becoming `sdc` and `sdd`.
  - The legacy FAT selftests run against `sdc`.
  - A new userland program `fattest.nex` mounts `/dev/sdd` at
    `/mnt/fat` as `"fat"` and `/dev/sdc` at `/mnt/fat16`. It then
    reads fixtures, creates/writes/reads back, renames, unlinks, does
    mkdir/rmdir with a nested file, and checks that a FAT lookup is
    case-insensitive (`/mnt/fat/FAT32.TXT`).
  - It stops on `PASS fattest` or FAILED.
  - The target runs `make fresh-disks` itself, because the FAT write
    selftest needs fresh images.
- **`make atatest`:** both ext2 images on legacy IDE (the old
  `-drive` form), the AHCI controller with only the CD. It must reach
  `[boot] root: sda1 ext2 via ata` and the scheduler with no FAILED.
- **`make ahcitest`:** the scratch disk moves to `sata.3`.
- **Launchers:** `blkdevtest` and every launcher now attach `disk2.img`
  on `sata.2` (`QEMU_AHCI`, `gauntlet.sh`, `screenshot.sh`, the two
  hand-written Makefile QEMU lines, and os-builder's Makefile and
  `qemu-run.sh.template`).

### 4.7 `blkdevtest` updates

- `sda`'s size check is unchanged (64 MiB).
- LBA 0's boot signature is now the protective MBR's (still `55 AA`).
- It gains checks that `sda1` and `sdb1` exist with major 8 and minors
  1 and 17.
- The non-destructive writes near the end of `sdb` now land in the
  GPT backup area, inside the last 33 sectors. Those are restored,
  but the backup GPT could be corrupted mid-test. The cases move to
  just below `sdb1`'s end, inside unused ext2 space, and are **still
  restored**.

## 5. Proof

### 5.1 `[ext2] selftest` (hermetic, every boot)

**Build-time image.** `tools/mk-ext2-fixture.sh` builds a 2 MiB
`mke2fs -t ext2 -b 1024 -N 64` image with:
- a 300 KiB file (direct + single + double indirect)
- a sparse file
- fast and slow symlinks
- a directory of 200 entries, which forces multiple blocks and, via
  `dir_index`, an htree root

It is linked into the kernel as a blob (`objcopy -I binary`, the way
embedfs data is linked). The selftest copies it onto a `ramblk` and
mounts that directly through `ext2_ops`. It then checks:

1. Reads match known patterns, including a hole reading as zeros.
2. `readlink` of both symlink kinds.
3. Lookup of the 200th entry.
4. Create, write across the indirect boundary, and read back.
5. Unlink frees blocks: the superblock free count is back to its
   earlier value after unlink.
6. Rename a file across directories, and a directory with its `..`
   rewritten.
7. mkdir/rmdir, with the parent's link count checked.
8. Adding an entry to the `dir_index` directory clears `INDEX_FL`.
9. Umount, remount, and every change is still there.
    Unlink-while-open: a file whose vnode is still held keeps its data
    readable after unlink, and its blocks come back only after the
    last put.
10. Free counts (superblock vs. the sum of group descriptors vs. a
    bitmap popcount) agree.

### 5.2 Host `e2fsck` (§4.5)

This is the external judge that the driver never corrupts a real
volume: the root image is written by the boot's selftests and by
several userland tests on every `make test`.

### 5.3 Other suites

- `blkdevtest`, `make ahcitest`, `make fattest`, `make atatest`
- the gauntlet (15/15, zero retries)
- `wm-shot`

## 6. ABI and documentation

- **`docs/stdlib.md`:**
  - ext2 is the default.
  - Names are **case-sensitive** now: a behaviour change from FAT for
    any program that relied on FAT's folding.
  - Real `st_mode`, `st_nlink`, uid/gid and `st_blocks`.
  - Symlinks are followed, with `ELOOP` at 40.
  - `lstat`, `readlink`, `O_NOFOLLOW` and `AT_SYMLINK_NOFOLLOW` now do
    what Linux's do.
  - `statfs` `f_type` is `0xEF53`, with real counts.
  - **Divergences:**
    - `..` after a symlink is textual.
    - `mode` is ignored on create/mkdir.
    - No `chmod`/`chown`/`link`/`symlink`/`utimensat` (storage-06).
    - No `O_CREAT` through a dangling symlink.
    - `atime` is never updated.
- **`docs/abi-compatibility.md`:** a storage-05 refresh.
- **Roadmap:**
  - storage-03 deferred.
  - storage-05 done ahead of 04.
  - D3's layout: `disk2` on `sata.2`.
  - D6's check: `/mnt` on `sdb1`.

## 7. Risks

| risk | mitigation |
|---|---|
| a userland test relied on FAT case-folding | it fails loudly on the first `make test`; fix the test's path, not the driver |
| `debugfs` `write` sets odd modes or owners from the host file | modes are preserved from the host file; owners are 0 (debugfs default) — nothing enforces them yet |
| `dir_index` htree internal blocks read as linear garbage | they are valid ext2 entries by design (fake `rec_len` spanning the block); covered by the 200-entry fixture |
| ext2 metadata I/O through a 512-byte-sector cache is slow | `mke2fs -b 1024`; metadata blocks are 2 sectors; measured by boot time in the gauntlet (must stay near ~2 min) |
| `e2fsck` flags harmless differences (e.g. the dir_index flag) | the driver clears INDEX_FL itself, exactly like Linux; any remaining complaint is a real bug |
