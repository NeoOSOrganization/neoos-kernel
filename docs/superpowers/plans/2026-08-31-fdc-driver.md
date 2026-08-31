# FDC (Floppy) Driver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A `struct blockdev` seam over the storage layer, and an
82077AA floppy driver behind it, so `mount fd0 /mnt fat` works against
a `qemu -fda` image.

**Architecture:** `blkcache` stops calling `ata_*` directly and
dispatches through a per-id `struct blockdev`. ATA registers ids 0-3;
`fdc.c` registers 0x80/0x81. The FDC driver is a classic PIO+ISA-DMA
device: motor/seek/recalibrate state machine, IRQ 6, DMA channel 2,
one sector per request via a fixed low bounce page.

**Tech Stack:** C (freestanding), NASM, x86-64, GNU Make, headless
QEMU (`-fda`), the parallel gauntlet (CONC=3).

**Spec:** `docs/superpowers/specs/2026-08-31-fdc-driver-design.md`

## Global Constraints

- **No host unit tests.** Each task's test is a kernel selftest
  (`blkcache_read(0x80, ...)`) or a userland `mount` step, verified by
  `make test` + the parallel gauntlet (`PGAUNTLET PASSED: 15/15`; ×3
  after the write path).
- **Lock ranks are enforced.** New locks: `blockdev_lock` (write-once
  table; a leaf, rank `LOCK_RANK_BLOCKDEV` — same as `blkcache`, taken
  sequentially not nested), and `fdc_lock` (a **`struct mutex`** — the
  op sleeps for the motor and the IRQ; rank `LOCK_RANK_DRIVER`).
- **ISA DMA limits:** the transfer buffer must be < 16 MiB physical and
  must not cross a 64 KiB boundary. A fixed page allocated at init from
  a low pool satisfies both.
- **Work on `main`, one commit per task**, standard trailer.
- **No `make` while the gauntlet runs.**

## File Structure

| File | Change |
|---|---|
| `kernel/fs/blockdev.h` (new) | `struct blockdev`, `blockdev_register/get` |
| `kernel/fs/blkcache.c` | dispatch through `blockdev_get` instead of `ata_*` |
| `kernel/dev/ata.c` | register ids 0-3 with a `blockdev` wrapper at init |
| `kernel/dev/fdc.c` / `.h` (new) | the driver |
| `kernel/dev/isadma.c` / `.h` (new, or inline in fdc.c) | 8237 channel programming + the low bounce page |
| `kernel/arch/idt.c` or the IRQ table | route IRQ 6 → `fdc_irq` |
| `kernel/kernel.c` | `fdc_init()` in the device-init sequence |
| `kernel/fs/fatfs.c` | `fatfs_mount_op`: accept `"fd0"`/`"fd1"` |
| `kernel/dev/fdc_selftest.c` (new) | read + round-trip |
| `Makefile` | `-fda build/floppy.img`; build the image; `[fdc]` markers |
| `userland/mounttst.c` or `vfstest.c` | `mount fd0` step |

---

## Task 1: `struct blockdev` seam

**Files:** `kernel/fs/blockdev.h` (new), `kernel/fs/blkcache.c`,
`kernel/dev/ata.c`.

**Interfaces — Produces:**
```c
struct blockdev {
    int (*read)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
    void *ctx;
    uint32_t nsectors;
    uint16_t sector_size;   // asserted == 512
};
int  blockdev_register(int id, const struct blockdev *dev);
const struct blockdev *blockdev_get(int id);   // lock-free after boot
```

- [ ] **Step 1: holding test** — `blkcache_selftest` already round-
  trips a sector on drive 0. Keep it green through this task; it is the
  regression cover (a guard test, not fail-first — legitimate per the
  SDD ledger's ruling 5).
- [ ] **Step 2: implement** the header + a fixed `struct blockdev
  *table[8]` guarded by `blockdev_lock` for register, read with an
  acquire load for `get`.
- [ ] **Step 3: route `blkcache`** — `blkcache_read/write(id, lba,
  buf)`: `const struct blockdev *d = blockdev_get(id); if (!d) return
  0; return d->read(d->ctx, lba, 1, buf);`. ATA's `ata_init` (or a
  shim called from it) registers ids 0-3 with `{ata_bd_read,
  ata_bd_write, (void*)(long)id, <size>, 512}` where the wrappers call
  `ata_read_sectors(id, lba, count, buf)`.
- [ ] **Step 4** — `make test`: `blkcache` selftest + `vfstest` +
  `mounttst` + the fat16/fat32 write selftests all green. Gauntlet
  15/15. Commit `"storage: a struct blockdev seam; blkcache dispatches through it"`.

---

## Task 2: FDC controller bring-up (no transfers yet)

**Files:** `kernel/dev/fdc.c`/`.h` (new), IRQ routing, `kernel/kernel.c`.

- [ ] **Step 1** — port I/O helpers for `0x3F0-0x3F7`; the command/
  result FIFO protocol (`fdc_send_byte` polls MSR RQM+!DIO;
  `fdc_read_byte` polls RQM+DIO).
- [ ] **Step 2** — `fdc_irq` (IRQ 6): set `fdc_irq_fired = 1`,
  `waitq_wake_all(&fdc_wait)`. `fdc_wait_irq()`:
  `waitq_sleep_timeout(&fdc_wait, ..., deadline)`; on timeout, return
  -EIO.
- [ ] **Step 3** — `fdc_init`: reset via DOR (clear→set bit 2), wait
  IRQ, `SENSE INTERRUPT` ×4; `CONFIGURE` (impl-seek on, FIFO on,
  threshold 8), `LOCK`; `SPECIFY` (SRT=8, HLT=5, HUT=0, DMA);
  CCR = 0 (500 kbps); `RECALIBRATE` drive 0, wait IRQ, sense, verify
  cyl 0. Register `blockdev` id 0x80 with a `read` that returns -EIO
  and `nsectors = 2880`.
- [ ] **Step 4** — `[fdc] controller ready` in the boot log, added to
  `REQUIRED_MARKERS`. `make test` green (no floppy image needed yet;
  the stub `read` is never called). Gauntlet 15/15. Commit
  `"fdc: 82077AA controller bring-up (reset, configure, recalibrate)"`.

---

## Task 3: ISA DMA + the read path

**Files:** `kernel/dev/isadma.c`/`.h` (or inline), `kernel/dev/fdc.c`,
`kernel/dev/fdc_selftest.c` (new), `Makefile`.

- [ ] **Step 1: the floppy image + failing test**

`Makefile`: `build/floppy.img`: `mkfs.fat -F 12 -C build/floppy.img
1440` then `mcopy` a `FD0.TXT`. `QEMU_COMMON += -fda build/floppy.img`.
`fdc_selftest`: `blkcache_read(0x80, 0, sec)`; assert `sec[510]==0x55
&& sec[511]==0xAA` (FAT12 boot signature). `[fdc] selftest passed` /
`FAILED`, added to `REQUIRED_MARKERS`. `make test` → FAIL (read stub
returns -EIO).

- [ ] **Step 2: ISA DMA** — allocate a fixed 512-byte-aligned page
  below 16 MiB at init (from the low pool / a reserved static array in
  `.boot.bss`-adjacent low memory, or `pmm_alloc` constrained — check
  what `pmm` offers; if nothing, add `pmm_alloc_low`). `isadma_setup
  (chan, phys, len, to_memory)`: mask the channel, clear the flip-flop
  (`0x0C`), set mode (`0x0B`: single + inc + read/write), write addr
  (`0x04`) low/high + page (`0x81` for ch2), write count (`0x05`)
  low/high (len-1), unmask.

- [ ] **Step 3: `fdc_rw` read** — `mutex_lock(&fdc_lock)`; LBA →
  (cyl, head, sec); motor on + spin-up; `SEEK cyl`, wait IRQ, sense,
  verify; `isadma_setup(2, bounce_phys, 512, /*to_memory=*/1)`;
  `READ DATA` command (9 bytes: cmd 0xE6 (MFM|MT? MT off, so 0xC6 |
  MFM 0x40 = 0x46... use 0x46 for read), then head/drive, cyl, head,
  sec, sectorsize=2, EOT=18, GAP=0x1B, DTL=0xFF); wait IRQ; read 7
  result bytes; check ST0 top bits == 0 and ST1/ST2 clean; `memcpy(buf,
  bounce_virt, 512)`. On error: recalibrate + retry ×3.
  `mutex_unlock`. Wire it as the `blockdev` id 0x80 `read`.

- [ ] **Step 4** — `[fdc] selftest passed`; suite green. Gauntlet
  15/15. Commit `"fdc: ISA DMA + single-sector read"`.

---

## Task 4: the write path + motor timeout

**Files:** `kernel/dev/fdc.c`, `kernel/dev/fdc_selftest.c`.

- [ ] **Step 1: extend `fdc_selftest`** — write a known pattern to a
  scratch sector (well past the FAT/root, e.g. LBA 2000),
  `blkcache_invalidate_drive(0x80)` (or a direct re-read), compare.
  `[fdc] write round-trip passed`. FAIL today (write stub).
- [ ] **Step 2: `fdc_rw` write** — same as read but
  `isadma_setup(..., to_memory=0)`, copy `buf`→bounce first, command
  `WRITE DATA` (0x45 | MFM 0x40 = 0xC5). Register as the id 0x80
  `write`.
- [ ] **Step 3: motor-off timer** — a `timer`-scheduled callback
  `fdc_motor_off` armed for ~2 s after each op; `fdc_rw` re-arms it and
  re-does spin-up if the motor was off. Motor state + deadline under
  `fdc_lock`'s guard.
- [ ] **Step 4** — suite green; gauntlet **×3** (IRQ 6 + DMA + the
  motor timer are the concurrency surface). Commit
  `"fdc: single-sector write; deferred motor-off"`.

---

## Task 5: mount wiring + docs

- [ ] **Step 1** — `fatfs_mount_op`: `"fd0"` → 0x80, `"fd1"` → 0x81
  alongside the existing `"hd0"`/`"hd1"`.
- [ ] **Step 2** — `mounttst.c` (or `vfstest.c`): `mount("fd0",
  "/flop", "fat")`, `open("/flop/FD0.TXT")`, read, compare, `umount`.
  A `REQUIRED_MARKER` if the file's success line is not already one.
- [ ] **Step 3** — `docs/stdlib.md`: `mount` accepts `fd0`/`fd1`.
  `docs/optimization-summary.md`: the `struct blockdev` seam + FDC.
  Gauntlet 15/15. Commit `"fdc: mountable as fd0/fd1; docs"`.

## Self-Review

**Spec coverage:** seam (§1) → Task 1; controller (§2) → Task 2; DMA +
read (§2) → Task 3; write + motor (§2) → Task 4; mount (§3) → Task 5;
selftest (§ testing) → Tasks 3-5. ✓

**Placeholder scan:** command byte encodings are given as concrete hex
with the datasheet field breakdown; the one genuinely uncertain bit
("does `pmm` expose a low allocator?") is a "check, and add
`pmm_alloc_low` if not" step. No bare "write tests" / "handle errors"
(the retry/recalibrate is spelled out).

**Type consistency:** `blockdev_register/get`, `struct blockdev`
fields, `isadma_setup`, `fdc_rw`, `fdc_lock` (a mutex), `fdc_wait`
consistent across tasks. IDs 0x80/0x81 for floppy throughout.
