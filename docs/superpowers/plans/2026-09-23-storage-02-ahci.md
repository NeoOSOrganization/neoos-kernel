# storage-02: AHCI (SATA) driver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the user chose inline execution) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive SATA disks and ATAPI drives through AHCI with NCQ and scatter-gather DMA, and move `build/disk.img` onto it so that `/` is on `sda via ahci`.

**Architecture:**
- An uncached MMIO window (`kernel/mm/mmio.c`) maps ABAR.
- `kernel/drivers/block/ahci/` layers HBA bring-up (`hba.c`), a multi-slot polled command engine with NCQ and error recovery (`port.c`), and device classes: disks (`disk.c`), ATAPI (`atapi.c`) and port multipliers (`pmp.c`, unverified).
- Disks register with the storage-01 block layer, which gains read-only / no-partition flags, `srN` naming and a `driver` tag.

**Tech Stack:** freestanding C (gnu11, `-mcmodel=kernel`), x86_64, QEMU 10.2 `pc` machine + `ich9-ahci`, musl userland (`blkdevtest`).

**Spec:** `docs/superpowers/specs/2026-09-23-storage-02-ahci-design.md` (read it first; section numbers below refer to it).

## Global Constraints

- **Tests.** No host-runnable unit tests. Every test is a boot selftest printing `... selftest passed` or a line containing `FAILED`, or a userland `PASS <name>`. `make test` fails on any `FAILED`/`PANIC` and on any missing `CORE_REQUIRED_MARKERS` entry.
- **One build at a time.** Never run two `make` invocations concurrently.
- **Commits.** Work on `main`. Commit after each task, and end every message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Do NOT push.
- **Lock ranks.** Driver/port locks are `LOCK_RANK_DRIVER` (8). No lock is held across a poll-wait. Run `make lock-check` after adding a lock.
- **Waits.** Every hardware wait is bounded by `ktime_after_ns()` / `ktime_get_ns()`, never by an iteration count.
- **Log wording.** Expected conditions (no device, not a disk, injected media error) must never print `FAILED`.
- **ABI values.** `sr` major 11; `BLKROGET` = `0x125E`; `EROFS` = 30; `ENOMEDIUM` = 123.
- **QEMU layout** (spec §6), identical in every launcher: AHCI at `addr=0x7`, placed after AC97; `disk.img` on `sata.0`; the ISO read-only on `sata.1`; `disk2.img` as the plain first IDE `-drive`.
- **Libc.** Kernel code has no libc. Use file-local helpers.

## Review Focus

1. **A buffer spanning a page boundary whose two pages are not physically adjacent** (a kmalloc'd or kernel-stack buffer). DMA must land in both pages. Task 6's PRDT selftest maps two non-adjacent frames at adjacent virtual pages through `mmio_map` and checks that the builder emits two entries with the right addresses. The concurrent read test uses kernel-stack buffers.
2. **Several CPUs hitting the same port at once** (raw `/dev/sda` I/O plus FAT misses). Slot ownership must never be shared. Task 6's concurrent-submit selftest plus the gauntlet (SMP 4) pin it.
3. **The error path on a busy queue.** One bad sector among concurrent NCQ reads must fail only its own request. Task 7's `make ahcitest` pins it with blkdebug.
4. **Empty IDE channel or ATAPI on IDE.** No `FAILED`. Task 2 checks it by booting `make ahcitest`, which has no IDE disks.
5. **Opening `/dev/sr0` for write, or writing through the block layer.** Must give `EROFS`. Task 8's `blkdevtest` additions pin it.

---

## File Structure

| file | status | responsibility |
|---|---|---|
| `kernel/mm/mmio.c`, `mmio.h` | new | uncached MMIO window, selftest |
| `kernel/mm/paging.h`, `paging.c` | modify | `PAGE_PWT`/`PAGE_PCD`, `paging_leaf_flags()` |
| `kernel/drivers/block/ata_id.c`, `ata_id.h` | new | IDENTIFY parsing shared by ATA and AHCI |
| `kernel/drivers/block/ata.c` | modify | uses ata_id; floating-bus / ATAPI treated as absent |
| `kernel/fs/blkcache.c` | modify | `write_gen` race fix + selftest |
| `kernel/block/ramblk.c`, `.h` | modify | `ramblk_set_read_hook` |
| `kernel/block/blockdev.c`, `.h` | modify | `driver`, `BLOCKDEV_RO`, `BLOCKDEV_NOPART`, `sr` names, `-EROFS` |
| `kernel/block/blkdev_file.c` | modify | `BLKROGET` |
| `kernel/syscall/sys_file.c` | modify | `EROFS` on write-open of an RO block node |
| `kernel/drivers/block/ahci/ahci.h` | new | registers, structs, internal API |
| `kernel/drivers/block/ahci/hba.c` | new | discovery, BOHC, reset, port bring-up, classification |
| `kernel/drivers/block/ahci/port.c` | new | memory, PRDT, submit/wait/poll, NCQ, error recovery |
| `kernel/drivers/block/ahci/disk.c` | new | `sdX` blockdev |
| `kernel/drivers/block/ahci/atapi.c` | new | `srN` blockdev |
| `kernel/drivers/block/ahci/pmp.c` | new | port multiplier (unverified) |
| `kernel/drivers/block/ahci/selftest.c` | new | `[ahci] selftest`, scratch selftest |
| `kernel/kernel.c` | modify | `mmio_selftest`, `ahci_probe` before `ata_probe`, root line |
| `Makefile` | modify | `KERNEL_DIRS`, QEMU layout, markers, `ahcitest` |
| `tools/gauntlet.sh`, `tools/screenshot.sh` | modify | QEMU layout |
| `tools/ahci-blkdebug.conf` | new | error injection |
| `userland/blkdevtest.c` | modify | `sr0` + `BLKROGET` checks |
| `docs/stdlib.md`, `docs/abi-compatibility.md` | modify | ABI docs |
| `../neoos-os-builder/Makefile`, `scripts/qemu-run.sh.template` | modify | QEMU layout |

---

### Task 1: Uncached MMIO window

**Files:** Create `kernel/mm/mmio.{c,h}`. Modify `kernel/mm/paging.{h,c}` and `kernel/kernel.c` (call `mmio_selftest()` after `pci_selftest()`). Modify `Makefile` (`CORE_REQUIRED_MARKERS += "[mmio] selftest passed"`).

**Interfaces:**
- Produces:
  - `volatile void *mmio_map(uint64_t phys, uint64_t len);`
  - `void mmio_init(void); void mmio_selftest(void);`
  - `uint64_t paging_leaf_flags(uint64_t virt);` returns the leaf PTE's flag bits (entry & ~PAGE_ADDR_MASK) for the kernel PML4, or 0.
  - `#define PAGE_PWT (1ULL<<3)`, `#define PAGE_PCD (1ULL<<4)`.

- [ ] **Step 1: Selftest first.** `mmio_selftest()`:
  1. `pmm_alloc(0)` a page and `mmio_map` it.
  2. Write `0xA5A5DEADBEEF5A5AULL` through the mapping, then read it back via `phys_to_virt`.
  3. Check `paging_leaf_flags(v) & (PAGE_PCD|PAGE_PWT)` equals both bits.
  4. `mmio_map` of an unaligned phys (page + 0x10) returns base + 0x10.

  Prints `[mmio] selftest passed` / `[mmio] selftest FAILED: <why>`.
- [ ] **Step 2: Implement.**
  - `MMIO_VIRT_BASE = PHYSMAP_BASE + (256ULL << 30)` and `MMIO_VIRT_SIZE = 1ULL << 30`.
  - A bump pointer under a spinlock (`LOCK_RANK_DRIVER`, name "mmio").
  - `mmio_map` rounds phys down and len up to pages, then calls `paging_map_range(virt, phys_base, len, PAGE_PRESENT|PAGE_WRITABLE|PAGE_NO_EXECUTE|PAGE_PCD|PAGE_PWT)`, and returns `virt + (phys & 0xFFF)`.
  - `paging_leaf_flags` walks with `table_entry` (4 KiB-only is correct here: the window is 4 KiB-mapped).
  - The comment explains PML4[256] sharing and the latent FB_VIRT_BASE problem.
- [ ] **Step 3:** `make lock-check && make test`. Expect PASS with the new marker.
- [ ] **Step 4:** Commit `feat(mm): uncached MMIO window for memory-BAR drivers (storage-02 task 1)`.

### Task 2: Shared IDENTIFY parser; ATA absent/non-disk fix (deferred item b)

**Files:** Create `kernel/drivers/block/ata_id.{c,h}`. Modify `kernel/drivers/block/ata.c`.

**Interfaces:**
- Produces: `struct ata_id` and `void ata_id_parse(const uint16_t w[256], struct ata_id *out);`, exactly as spec §2. Also `void ata_id_selftest(void);`, called from `kernel.c` next to `blockdev_selftest`, with marker `[ata_id] selftest passed`.

- [ ] **Step 1: Selftest.** Build a synthetic 256-word IDENTIFY and check the parsed fields:

  | words set | expected |
  |---|---|
  | 60–61 = 0x0FFFFFFF | used only when LBA48 is off |
  | 83 bit 10, 100–103 = 0x1_0000_0000 | `sectors = 1<<32`, `lba48` set |
  | 106 = 0x5000 with 117–118 = 2048 words | `logical_sector` = 4096 |
  | 76 bit 8, 75 = 31 | `ncq`, `queue_depth` = 32 |
  | 84 bit 6 | `fua` |
  | 85 bit 5 | `write_cache` |
  | model "QEMU HARDDISK", byte-swapped, space-padded | `model` = "QEMU HARDDISK" |

  Then a second record with LBA48 clear gives `sectors == 0x0FFFFFFF`.
- [ ] **Step 2: Implement `ata_id_parse`.** Strings: each word holds two chars, high byte first; trim trailing spaces.
- [ ] **Step 3: Update `ata.c`.**
  - `ata_identify_locked` reads IDENTIFY into `uint16_t w[256]` and calls `ata_id_parse` (the sector count stays LBA28-capped for PIO: `min(sectors, 1<<28)`).
  - Before issuing IDENTIFY: after the drive select, read status. If it is `0xFF` or `0`, log `[ata] drive N not present` and return 0.
  - After IDENTIFY, if status has ERR, or (`inb(ATA_LBA_MID)`, `inb(ATA_LBA_HIGH)`) is `(0x14,0xEB)` or `(0x69,0x96)`, log `[ata] drive N: not an ATA disk (sig MM/HH), skipped` and return 0.
  - Set `bdev.driver = "ata"` (field added in Task 4; if doing tasks in order, add the field here).
- [ ] **Step 4:** `make test`. PASS, and `[ata] drive identified` still shows two drives (the layout hasn't changed yet).
- [ ] **Step 5:** Commit `feat(ata): shared IDENTIFY parser; an empty or ATAPI channel is absent, not FAILED`.

### Task 3: blkcache read-miss generation check (deferred item a)

**Files:** Modify `kernel/fs/blkcache.c` and `kernel/block/ramblk.{c,h}`.

**Interfaces:**
- Produces: `void ramblk_set_read_hook(struct blockdev *d, void (*fn)(struct blockdev *, uint64_t lba, void *arg), void *arg);`. The hook is called at the start of each device read, before the copy-out.

- [ ] **Step 1: Failing selftest**, appended to `blkcache_selftest` before `out:`:
  1. Evict LBA 200 of `d` (a fresh LBA).
  2. Install a hook that, the first time it runs, sets a 512-byte buffer to `0x77` and calls `blkcache_write(d, 200, buf)`, then clears itself.
  3. `blkcache_read(d, 200, b)`: the hook runs inside the device read, and the read returns either the old or the new bytes. Both are allowed, so don't check.
  4. Then `blkcache_read(d, 200, b)` again must give `b[0] == 0x77`. Before the fix it returned the stale cached bytes.

  `why = "stale read-miss install"`.
- [ ] **Step 2:** Run `make test` and confirm the `[blkcache] selftest FAILED: stale read-miss install` line appears.
- [ ] **Step 3: Implement.**
  - `static uint64_t write_gen;` under `cache_lock`.
  - `blkcache_write_multi` does `write_gen++` under the lock before `blockdev_write`, and increments it again in its post-write locked section.
  - `blkcache_invalidate` increments it too.
  - `blkcache_read`: `gen = write_gen` in the first locked section. After the device read, relock; if `write_gen != gen`, skip `claim()` and return 0.
  - `ramblk`'s read op calls the hook first, then copies from its data array. The copy-out then sees the hook's write, which is fine either way.
- [ ] **Step 4:** `make test` → `[blkcache] selftest passed`.
- [ ] **Step 5:** Commit `fix(blkcache): a write racing a read miss can no longer leave stale bytes cached`.

### Task 4: Block-layer additions — driver tag, read-only, no-partition, `srN`, EROFS, BLKROGET, root line

**Files:** Modify `kernel/block/blockdev.{c,h}`, `kernel/block/part.c`, `kernel/block/blkdev_file.c`, `kernel/block/ramblk.c`, `kernel/syscall/sys_file.c`, `kernel/kernel.c` and `Makefile`.

**Interfaces:**
- Produces:
  - `const char *driver;` in `struct blockdev`; partitions copy it from the disk.
  - `#define BLOCKDEV_RO 0x2u`, `#define BLOCKDEV_NOPART 0x4u`.
  - `blockdev_alloc_name("sr", out)` gives `sr0`..`sr9`.
  - `srN` gets major 11, minor N.
  - `blockdev_write` on an RO device returns `-EROFS`.
  - `BLKROGET` = `0x125E`.

- [ ] **Step 1: Selftest additions** in `blockdev_selftest`, using a `ramblk_create("tsr", 2048, 64)` with `flags |= BLOCKDEV_RO|BLOCKDEV_NOPART`, registered:
  - `blockdev_write(...)` returns `-EROFS`.
  - No `tsr1` is registered, even with a valid MBR written into its data.
  - `blockdev_alloc_name("sr", nm)` gives `"sr0"` when no sr exists.
  - An unregistered ramblk named `"sr0"` gets major 11, minor 0 when registered non-hidden... ramblk is hidden, so test the naming function `blockdev_major_minor_for(name, &maj, &min)` (factored out of `register_disk`) directly: `"sr3"` gives 11/3 and `"sdb"` gives 8/16.
- [ ] **Step 2: Implement** the flags, `blockdev_major_minor_for`, and the `sr` alloc. `part_scan` skips when `NOPART`. `blockdev_register_part` sets `p->driver = disk->driver`. `ramblk` sets `driver = "ram"` and `ata.c` sets `"ata"`.
- [ ] **Step 3: `sys_file.c`.** After the O_EXCL check: if `vn->type == VNODE_BLOCK && (bd->flags & BLOCKDEV_RO) && (flags & O_ACCMODE) != O_RDONLY`, return `-EROFS` (with the same cleanup as EBUSY). `blkdev_file.c`: `BLKROGET` writes `int` 1/0.
- [ ] **Step 4: `kernel.c`.** After `vfs_mount_fs("/dev/sda", "/", "fat")`, log `[boot] root: <d->name> via <d->driver>` using `blockdev_find("sda")`.
- [ ] **Step 5:** `make test`. It passes, and the log shows `[boot] root: sda via ata`. The marker isn't required yet; Task 5 flips it.
- [ ] **Step 6:** Commit `feat(block): read-only and no-partition devices, srN naming, BLKROGET, driver tag`.

### Task 5: AHCI bring-up, command engine (non-queued), disks; move disk.img to AHCI

**Files:**
- Create `kernel/drivers/block/ahci/{ahci.h,hba.c,port.c,disk.c}`.
- Modify `kernel/kernel.c` (call `ahci_probe()` before `ata_probe()`) and `Makefile` (`KERNEL_DIRS += kernel/drivers/block/ahci`, `QEMU_COMMON`, the 862/1160 lines, markers).
- Modify `tools/gauntlet.sh` and `tools/screenshot.sh`.

**Interfaces** (all in `ahci.h`):
- `struct ahci_hba { volatile uint8_t *abar; uint32_t cap, cap2, pi; uint8_t nslots, s64a, sncq, spm, sclo; int index; };`
- `struct ahci_port { struct ahci_hba *hba; int num; volatile uint8_t *regs; uint64_t cl_phys, fis_phys, ct_phys; uint8_t *cl, *ct; struct spinlock lock; struct ahci_req *slot_req[32]; uint32_t busy; /* slots in flight */ uint32_t ncq_mask; int exclusive; int active_pmp; int pmp_attached; };`
- `struct ahci_link { struct ahci_port *port; uint8_t pmp; };`
- `struct ahci_req` exactly as spec §4.3.
- Functions:
  - `int ahci_port_setup(struct ahci_port *p);` (memory + stop)
  - `int ahci_port_comreset(struct ahci_port *p);`
  - `int ahci_port_start(struct ahci_port *p);` and `void ahci_port_stop(struct ahci_port *p);`
  - `int ahci_submit(struct ahci_link *l, struct ahci_req *r);`, `int ahci_wait(struct ahci_link *l, struct ahci_req *r);`, `int ahci_exec(struct ahci_link *l, struct ahci_req *r);`
  - `int ahci_build_prdt(uint8_t *table, const void *buf, uint32_t len, int s64a);` returns the entry count or `-EINVAL`/`-E2BIG`/`-EIO`.
  - `void ahci_fis_rw(uint8_t fis[20], uint8_t cmd, uint64_t lba, uint32_t count, uint8_t pmp);` (LBA48 layout; `count` in bytes 12–13).
  - `void ahci_fis_ncq(uint8_t fis[20], int write, uint64_t lba, uint32_t count, int tag, int fua, uint8_t pmp);`
  - `int ahci_disk_attach(struct ahci_link *l);` and `int ahci_atapi_attach(struct ahci_link *l);` (the latter is a stub returning -ENODEV until Task 8)
  - `int ahci_pmp_attach(struct ahci_port *p);` (a stub returning -ENODEV until Task 9)
  - `void ahci_probe(void);` and `void ahci_selftest(void);`

- [ ] **Step 1: Test first (the boot's own markers).** Change `QEMU_COMMON` to the spec §6 layout, and add `CORE_REQUIRED_MARKERS` `"[ahci] port 0: sda "` and `"[boot] root: sda via ahci"`. Run `make test` and confirm it FAILS with `MISSING EXPECTED RESULT: [ahci] port 0: sda`. On IDE, `disk2.img` became `sda`, so FAT mounts the wrong disk; confirm that from the log. This proves the markers catch a fall-back.
- [ ] **Step 2: `hba.c`**, per spec §4.1. Register offsets:

  | global | offset |
  |---|---|
  | CAP | 0x00 |
  | GHC | 0x04 (AE bit 31, IE bit 1, HR bit 0) |
  | IS | 0x08 |
  | PI | 0x0C |
  | VS | 0x10 |
  | CAP2 | 0x24 |
  | BOHC | 0x28 (BOS bit 0, OOS bit 1, BB bit 4) |

  | port register (at 0x100 + 0x80·n) | offset |
  |---|---|
  | CLB / CLBU | 0x00 / 0x04 |
  | FB / FBU | 0x08 / 0x0C |
  | IS | 0x10 |
  | IE | 0x14 |
  | CMD | 0x18 (ST 0, SUD 1, CLO 3, FRE 4, CCS 12:8, PMA 17, FR 14, CR 15) |
  | TFD | 0x20 |
  | SIG | 0x24 |
  | SSTS | 0x28 |
  | SCTL | 0x2C |
  | SERR | 0x30 |
  | SACT | 0x34 |
  | CI | 0x38 |

  CAP bits: NP 4:0, NCS 12:8, SCLO 24, SSS 27, SPM 17, SNCQ 30, S64A 31. CAP2: BOH bit 0.

  Up to 4 HBAs and 32 ports each; `struct ahci_port` storage is static.
- [ ] **Step 3: `port.c`, non-queued path.**

  Command header (32 B at `cl + slot*32`):
  | dword | contents |
  |---|---|
  | 0 | CFL (5 dwords) bits 4:0, A bit 5, W bit 6, C bit 10, R bit 8, PMP 15:12, PRDTL 31:16 |
  | 1 | PRDBC (zeroed) |
  | 2–3 | CTBA |

  Command table: CFIS at 0x00, ACMD at 0x40, PRDT at 0x80.
  PRDT entry: DBA (lo/hi), reserved, DBC (byte count − 1) in bits 21:0, I bit 31 = 0.

  Submit, under the lock:
  1. Wait while `exclusive` is set, or while (for a non-NCQ request) `busy != 0`.
  2. Pick the lowest free slot below `nslots`.
  3. Build the table and header, then `wmb` (`__asm__ volatile("" ::: "memory")` plus `sfence`).
  4. For NCQ, write `SACT = 1<<slot`. Then write `CI = 1<<slot`.

  `port_poll` per spec §4.3. Error recovery is a stub in this task (`-EIO` for everything outstanding, log `[ahci] port N: error ...`); Task 7 replaces it.
- [ ] **Step 4: `disk.c`.**
  - IDENTIFY via `ahci_exec` with a 512-byte kernel buffer, then `ata_id_parse`.
  - Register `sdX` with `driver="ahci"`.
  - Log `[ahci] port %d: %s %llu sectors ... ncq=%d`. The hex-formatting helpers are `serial_write_hex64`/`serial_write_string`; use decimal via a local `put_dec`.
  - Read/write in this task: READ/WRITE DMA EXT, split at 240 KiB, plus FLUSH CACHE EXT after a write when there is a write cache and no FUA.
- [ ] **Step 5: Wire it up.** `ahci_probe()` in `kernel.c` goes before `ata_probe()`. Update the two Makefile hand-written QEMU lines, `gauntlet.sh` (per-run copy on `sata.0`, the ISO on `sata.1`, `d2` as the IDE `-drive`) and `screenshot.sh`.
- [ ] **Step 6: Verify.**
  - `make lock-check && make test`: PASS, with the log showing `[ahci] port 0: sda`, `[ahci] port 1: ... not a disk` (ATAPI until Task 8), `[boot] root: sda via ahci`, `sdb` on `ata`, and the FAT selftests passing on AHCI.
  - Then `make blkdevtest`: PASS. It writes to `sdb` (IDE) and reads `sda` (AHCI).
- [ ] **Step 7:** Commit `feat(ahci): AHCI HBA bring-up and disks; disk.img moves to SATA (storage-02 task 5)`.

### Task 6: NCQ, concurrent slots, FUA; `[ahci] selftest`

**Files:** Modify `ahci/port.c` and `ahci/disk.c`. Create `ahci/selftest.c`. Modify `kernel/kernel.c` (call `ahci_selftest()` after `ahci_probe()`) and `Makefile` (marker `"[ahci] selftest passed"`).

- [ ] **Step 1: Selftest.**
  - FIS checks:
    - `ahci_fis_ncq(f, 0, 0x123456789A, 16, 5, 0, 0)` gives f[0]=0x27, f[1]=0x80, f[2]=0x60, f[3]=16 (count lo in FEATURES), f[11]=0 (features hi), f[12]=5<<3, f[4..6]=9A 78 56, f[8..10]=34 12 00, f[7]=0x40.
    - With FUA, f[7]=0xC0.
    - `pmp=3` makes f[1]=0x83.
  - PRDT checks:
    - 8192 bytes at `phys_to_virt(p)` of an order-1 block gives 1 entry of 8192.
    - Two order-0 frames a and b (b != a+4096) mapped at adjacent virtual pages via `mmio_map(a)` then `mmio_map(b)` (the bump allocator makes them adjacent): a 4096-byte buffer at virt+2048 gives 2 entries, `a+2048`/2048 and `b`/2048.
    - An odd address gives -EINVAL.
    - `ahci_build_prdt(t, phys_to_virt(p), 65 * 0x400000, 1)` walks at most 64 entries and gives -E2BIG. The pages beyond physical RAM are never touched; the physmap translation of a large range is just arithmetic.
  - Concurrent reads: on the first `driver=="ahci"` non-RO disk, 8 `struct ahci_req` of 16 sectors at LBA `i*997` go through `ahci_submit`, then all get `ahci_wait`, and are compared with `blockdev_read` of the same ranges. Log `[ahci] ncq: max in flight N`.
- [ ] **Step 2: Run it.** `make test` should fail on the in-flight check... as a TDD step, `ahci_fis_ncq` doesn't exist yet, so this is a build failure. Accept the build failure as the red step.
- [ ] **Step 3: Implement NCQ.**
  - `r->ncq` is set by `disk.c` when `hba->sncq && id.ncq`.
  - The tag is the slot. Limit in-flight NCQ to `min(nslots, queue_depth)`.
  - FUA when `id.fua && id.write_cache`. Without NCQ, use WRITE DMA FUA EXT (0x3D) when FUA is supported.
  - `disk.c` read/write: submit every chunk, then wait on all of them, and return the first error.
- [ ] **Step 4:** `make test` → PASS with `[ahci] selftest passed`.
- [ ] **Step 5:** Commit `feat(ahci): native command queuing across slots, FUA writes, AHCI selftest`.

### Task 7: Error recovery; `make ahcitest` with injected errors

**Files:** Modify `ahci/port.c`. Add to `ahci/selftest.c`. Create `tools/ahci-blkdebug.conf`. Modify `Makefile` (the `ahcitest` target).

- [ ] **Step 1: Test first.**
  - `tools/ahci-blkdebug.conf`:
    ```
    [inject-error]
    event = "read_aio"
    errno = "5"
    sector = "4096"
    once = "off"
    ```
  - Target:
    ```make
    AHCI_SCRATCH := $(BUILD_DIR)/ahci-scratch.img
    ahcitest: iso disk-image
    	rm -f $(AHCI_SCRATCH); truncate -s 8M $(AHCI_SCRATCH)
    	@tools/boot_until.sh $(BUILD_DIR)/ahcitest.log '^\[ahci\] scratch selftest (passed|FAILED)' $(BOOT_TIMEOUT) -- $(QEMU_COMMON) -display none \
    	  -drive driver=blkdebug,config=tools/ahci-blkdebug.conf,image.filename=$(AHCI_SCRATCH),image.driver=file,if=none,id=sata2 \
    	  -device ide-hd,drive=sata2,bus=sata.2,serial=NEOOSSCRATCH
    	@grep -q '^\[ahci\] scratch selftest passed' $(BUILD_DIR)/ahcitest.log || { grep '\[ahci\]' $(BUILD_DIR)/ahcitest.log; exit 1; }
    	@echo "PASS ahcitest"
    ```
  - `ahci_scratch_selftest(struct ahci_link *, struct blockdev *)` is called from `disk.c` attach when `id.serial` equals `"NEOOSSCRATCH"`, deferred until after `ahci_probe` completes (it's recorded, and `ahci_selftest` runs it). It follows spec §6.
  - Before recovery exists, run `make ahcitest` and expect FAILED: all 4 reads fail, not just one.
- [ ] **Step 2: Implement recovery**, spec §4.7.
  - READ LOG EXT FIS: cmd 0x2F, LBA low = 0x10, count = 1, 512 bytes, non-queued.
  - Re-queue by moving requests back to a pending list, then resubmitting after the port restarts. The submitter doesn't know; its `done` flag simply isn't set yet.
  - Log which recovery path ran: `[ahci] port N: ncq error, tag T from log 10h` or `... single-stepping K commands`.
- [ ] **Step 3:** `make ahcitest` → `PASS ahcitest`, then `make test` still passes.
- [ ] **Step 4:** Commit `feat(ahci): libata-style NCQ error recovery; make ahcitest injects a media error`.

### Task 8: ATAPI (`srN`), blkdevtest

**Files:** Create/replace `ahci/atapi.c`. Modify `ahci/selftest.c`, `userland/blkdevtest.c` and `Makefile` (marker `"[ahci] port 1: sr0 "`).

- [ ] **Step 1: Tests first.**
  - Add the `sr0` check to `ahci_selftest`: read block 16 and compare bytes 0..5 with `01 'C' 'D' '0' '0' '1'`; `blockdev_write` gives -EROFS.
  - Add to `blkdevtest.c`:
    ```c
    #define BLKROGET 0x125E
    CHECK(stat("/dev/sr0", &st) == 0 && S_ISBLK(st.st_mode) && major(st.st_rdev) == 11 && minor(st.st_rdev) == 0, "sr0 node");
    errno = 0; fd = open("/dev/sr0", O_RDWR);
    CHECK(fd < 0 && errno == EROFS, "sr0 O_RDWR fd=%d errno=%d", fd, errno);
    fd = open("/dev/sr0", O_RDONLY);
    int ro = 0; CHECK(ioctl(fd, BLKROGET, &ro) == 0 && ro == 1, "sr0 BLKROGET %d", ro);
    CHECK(ioctl(fd, BLKSSZGET, &ss) == 0 && ss == 2048, "sr0 BLKSSZGET %d", ss);
    unsigned char pvd[6];
    CHECK(pread(fd, pvd, 6, 32768) == 6 && memcmp(pvd, "\x01" "CD001", 6) == 0, "sr0 PVD");
    close(fd);
    ```
    Plus `sda` BLKROGET 0, and `/proc/partitions` containing `" sr0\n"`.
  - `make blkdevtest` should fail (no /dev/sr0).
- [ ] **Step 2: Implement `atapi.c`**, spec §4.5.
  - The PACKET FIS has features bit 0 = DMA, and header A=1 with the CDB in ACMD.
  - READ CAPACITY(10) gives a big-endian last LBA and block length.
  - Register with `BLOCKDEV_RO|BLOCKDEV_NOPART`.
- [ ] **Step 3:** `make test` (with the sr0 marker) and `make blkdevtest` → PASS.
- [ ] **Step 4:** Commit `feat(ahci): ATAPI drives as read-only srN`.

### Task 9: Port multipliers (unverified)

**Files:** Create/replace `ahci/pmp.c`. Modify `ahci/hba.c` (SPM path).

- [ ] **Step 1:** Extend the FIS test in `ahci_selftest`: `ahci_fis_pmp_read(f, port=2, reg=0)` gives cmd 0xE4, f[1] = 0x8F (C bit + PMP 15), features = 0 (register), device = 2 (port), and a write variant with 0xE8 and the value in count/LBA bytes.
- [ ] **Step 2:** Implement spec §4.6: soft reset (two H2D FISes: the first with control SRST 0x04 and header R+C bits, the second with control 0), GSCR reads, per-fan-out-port reset/classify/attach on `struct ahci_link { port, pmp=n }`, and command-based switching via `active_pmp` in submit. File banner: `UNVERIFIED: no hardware or emulator ...`.
- [ ] **Step 3:** `make test`. PASS, and the log shows no PM path (QEMU has CAP.SPM clear).
- [ ] **Step 4:** Commit `feat(ahci): port multiplier support (command-based switching; unverified)`.

### Task 10: os-builder, docs, gauntlet

**Files:** `../neoos-os-builder/Makefile`, `../neoos-os-builder/scripts/qemu-run.sh.template`, `docs/stdlib.md`, `docs/abi-compatibility.md`.

- [ ] **Step 1: os-builder.** Replace `-drive file=…disk1.img,format=raw` with `-device ahci,id=sata,addr=0x7 -drive file=…disk1.img,format=raw,if=none,id=sata0 -device ide-hd,drive=sata0,bus=sata.0`, placed after the other `-device` lines. `disk2.img` stays a plain IDE `-drive`. Check the AC97 addr in the template, and commit in that repo.
- [ ] **Step 2: Docs.** `docs/stdlib.md` "Block devices" section per spec §7, and the `docs/abi-compatibility.md` storage-02 refresh.
- [ ] **Step 3: Verify.** `make test`, `make blkdevtest`, `make ahcitest`, then the gauntlet `tools/gauntlet.sh` (15/15 zero-retry), and `make desktop` boots (screenshot sanity) if time permits.
- [ ] **Step 4:** Commit `docs: storage-02 -- AHCI, srN, BLKROGET in stdlib and ABI notes`.
