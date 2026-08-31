# FDC (floppy) driver — design

**Date:** 2026-08-31
**Status:** design (solo brainstorm — see roadmap). Isolated new driver
behind a block-device seam that this milestone also introduces.
**Context:** `kernel/dev/ata.c` (the only existing storage driver),
`kernel/fs/blkcache.c` (calls `ata_read_sectors` directly, keyed by an
8-bit `drive`), `kernel/fs/fatfs.c` `fatfs_mount_op` (parses
`source` = `"hd0"`/`"hd1"`).

## Problem

There is exactly one storage driver (ATA PIO) and no abstraction over
it: `blkcache_read/write(drive, lba, buf)` calls `ata_read_sectors`
directly. Adding a second storage device means either bolting it into
`blkcache` with an `if (drive >= X)` or building the seam that should
have been there.

Nobody has asked for floppy support for its own sake — it is the
**cheapest possible second block driver**: a fully-documented legacy
device (82077AA), no PCI enumeration, no interrupt-storm handling, and
a natural fit for boot-floppy images and for exercising the block seam
before the audio milestone needs a similar `struct` discipline.

## Goals

1. **A minimal block-device seam.** `struct blockdev` with
   `read(lba, count, buf)` / `write(lba, count, buf)` / `nsectors` /
   `sector_size` (512 for both ATA and 1.44M floppy). `blockdev_register
   (int id, const struct blockdev *)`; `blkcache` dispatches through the
   registered device for that id instead of calling `ata_*` directly.
   ATA registers ids 0-3; FDC registers ids 0x80, 0x81.
2. **An 82077AA-compatible FDC driver** (`kernel/dev/fdc.c`): motor
   control + spin-up, recalibrate/seek, single-sector read and write
   via ISA DMA channel 2, IRQ 6.
3. **A mountable floppy.** `fatfs_mount_op` accepts `source = "fd0"`
   → blockdev id 0x80, so `mount fd0 /mnt fat` works against a
   `qemu -fda` image.
4. A selftest: read a known sector from the boot floppy image; on a
   writable image, round-trip one sector.

## Non-goals

- Multiple floppy geometries / auto-detection beyond 1.44 MB
  (80 cyl × 2 heads × 18 sectors, 512 B). A 1.44 M image is what QEMU
  and everyone else uses. Other geometries: recorded as unsupported.
- Multi-sector / multi-track DMA transfers. One sector per request is
  slow but simple and correct; `blkcache` already turns FAT's access
  pattern into mostly-cache-hits.
- Formatting a floppy (`FORMAT TRACK` command). Read/write existing
  media only.
- Making floppy the boot device. GRUB/Multiboot2 boot stays off the
  CD-ROM; the floppy is a data volume.
- A generic `/dev/fd0` block-special file in devfs (could be added
  later once the seam exists; not needed for `mount`).

## Design

### 1. `struct blockdev` seam (`kernel/fs/blockdev.h` + `blkcache.c`)

```c
struct blockdev {
    int      (*read)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int      (*write)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
    void      *ctx;
    uint32_t   nsectors;
    uint16_t   sector_size;   // 512 for now; asserted
};

int  blockdev_register(int id, const struct blockdev *dev);
const struct blockdev *blockdev_get(int id);
```

- `blkcache_read/write(id, lba, buf)`: `blockdev_get(id)`; if none,
  return 0. Call `dev->read(dev->ctx, lba, 1, buf)`. Cache logic
  unchanged.
- ATA: `ata_init` (or a small shim) registers ids 0-3 with a `blockdev`
  whose `read`/`write` wrap `ata_read_sectors`/`ata_write_sectors`. The
  `blkcache` -> `ata_*` direct calls become `blkcache` -> `blockdev` ->
  `ata_*`. One indirection; measured cost nil (cache hit rate is high).
- Registration table: fixed array `[8]` under `blockdev_lock` (rank
  `LOCK_RANK_BLOCKDEV` or a new leaf just above it — it is read on
  every cache miss). Register-once at boot; no unregister.

### 2. FDC driver (`kernel/dev/fdc.c`)

Ports (primary controller): `0x3F0-0x3F7`.
- `0x3F2` DOR (digital output): motor-on bits, drive select, DMA/IRQ
  enable, controller reset.
- `0x3F4` MSR (main status), `0x3F5` FIFO (data), `0x3F7` CCR (data
  rate).

**Init (`fdc_init`):**
1. Reset via DOR (clear then set bit 2), wait for the reset IRQ,
   `SENSE INTERRUPT STATUS` ×4.
2. `CONFIGURE` (implied seek on, FIFO on, threshold), `LOCK`.
3. `SPECIFY` (SRT/HLT/HUT step timings for 1.44 M).
4. Set CCR data rate to 500 kbps.
5. `RECALIBRATE` drive 0, wait IRQ, sense.
6. Register `struct blockdev` id 0x80: `nsectors = 2880`.

**Motor management:** `fdc_motor_on(drive)` sets the DOR bit and, if it
was off, sleeps ~300 ms for spin-up (via `waitq_sleep_timeout` on a
dummy queue, or a `timer`-driven delay). A deferred `fdc_motor_off`
after ~2 s of inactivity (timer callback) so the motor is not left
spinning. Motor state under `fdc_lock`.

**IRQ 6 handler:** sets a completion flag and wakes `fdc_wait` (a
`waitq`). The driver's command functions `waitq_sleep` on it. The
handler does the minimum; the command function reads result bytes.

**Read/write one sector (`fdc_rw(drive, lba, buf, write)`):**
1. `fdc_lock` (a mutex — the whole op sleeps for the motor and the
   IRQ; must not be a spinlock). Rank: a sleeping `struct mutex`
   `LOCK_RANK_DRIVER`.
2. LBA → (cyl, head, sector): `sector = lba % 18 + 1`,
   `head = (lba / 18) % 2`, `cyl = lba / 36`.
3. Motor on; `SEEK` to `cyl`; wait IRQ + sense; verify cylinder.
4. Program the 8237 DMA controller, channel 2: mask, clear flip-flop,
   mode (read = write-to-memory / write = read-from-memory,
   single, address-increment), set address + count (512-1), unmask.
   The DMA buffer is a **fixed bounce page below 16 MB** (ISA DMA
   cannot address above 24 bits and cannot cross a 64 KB boundary) —
   allocate one at init from a low pool, like the design the ATA path
   would need for DMA. Copy in for writes; copy out for reads.
5. Issue `READ DATA` / `WRITE DATA` (MFM, MT off), wait IRQ, read the
   7 result bytes, check ST0/ST1/ST2 for errors; retry up to 3× with a
   recalibrate between.
6. Copy the bounce buffer to/from `buf`. Release `fdc_lock`.

### 3. Mount wiring (`kernel/fs/fatfs.c`)

`fatfs_mount_op`: extend the source parse —
```
"hd0" -> 0   "hd1" -> 1
"fd0" -> 0x80   "fd1" -> 0x81
```
No other change: `v->drive` is already an `int` passed straight to
`blkcache_*`, which now dispatches.

## Testing

- `qemu` line gains `-fda build/floppy.img` (built by the Makefile:
  `mkfs.fat -F 12 -C build/floppy.img 1440`, seed a `FD0.TXT`).
- `kernel/dev/fdc_selftest.c` from the boot sequence: `blkcache_read
  (0x80, 0, sec)` and check the FAT12 boot signature `0x55 0xAA` at
  offset 510; then read the root-dir sector and find `FD0.TXT`. On the
  writable image: write a sector, invalidate the cache, read it back,
  compare. `[fdc] selftest passed`, added to `REQUIRED_MARKERS`.
- A userland `mount fd0 /flop fat && cat /flop/FD0.TXT` step folded
  into `mounttst` or `vfstest`.
- Gauntlet: FDC uses IRQ 6 and ISA DMA; run ×3 to shake out IRQ races
  with the other selftests. The motor-off timer is the concurrency
  point to watch.

## ABI / stdlib impact

- None directly (block devices are not user-visible except through
  `mount`). `docs/stdlib.md`: `mount` accepts `fd0`/`fd1` sources.
- `docs/abi-compatibility.md`: unchanged.
- `docs/optimization-summary.md`: note the `struct blockdev` seam (also
  the groundwork for a future AHCI/NVMe driver).

## Risks

1. **ISA DMA on QEMU vs metal.** QEMU's 8237 model is forgiving; the
   64 KB-boundary and 16 MB-address constraints must still be honored
   or a real machine silently transfers garbage. The fixed low bounce
   page sidesteps both.
2. **IRQ 6 sharing / spurious resets.** The FDC raises an IRQ on
   reset, on seek completion, and on transfer completion — the
   command functions must consume exactly the right number of
   `SENSE INTERRUPT` results or the controller desyncs. Follow the
   82077AA datasheet state machine exactly.
3. **Motor-off timer racing a new request.** `fdc_motor_off` fires
   from timer context; a `fdc_rw` starting at the same instant must
   re-assert the motor and re-do spin-up. Guard motor state + the
   pending-off deadline under `fdc_lock` (the mutex's guard spinlock is
   fine for the short state check; the long waits are outside it).
4. **The `blockdev` seam touches the hot path.** `blkcache` miss ->
   `blockdev_get` -> indirect call. Keep `blockdev_get` lock-free
   after boot (the table is write-once): a plain array read with an
   acquire load is enough.

## Plan sketch (for `writing-plans`)

1. `struct blockdev` + `blockdev_register/get`; route `blkcache`
   through it; ATA registers ids 0-3. No behaviour change. Gauntlet.
2. `fdc.c`: init, reset, recalibrate, IRQ handler, `SENSE`/`SEEK`
   plumbing. Register id 0x80 with a stub `read` returning -EIO.
   `[fdc] controller ready` log. Gauntlet.
3. DMA bounce page + `fdc_rw` read path. `fdc_selftest` read half.
   `-fda` in the Makefile. Gauntlet.
4. `fdc_rw` write path + retry/recalibrate. `fdc_selftest` round-trip.
   Motor-off timer. Gauntlet ×3.
5. `fatfs_mount_op` `fd0`/`fd1`; `mount fd0` userland step. Docs.
