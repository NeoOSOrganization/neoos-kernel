# storage-02: AHCI (SATA) driver — NCQ, ATAPI, port multipliers

Date: 2026-09-23. Status: **approved in brainstorming** (the user chose
full scope, scatter-gather with multiple slots, and NCQ, and said to go
ahead without further section review). Series:
[storage-00 roadmap](2026-09-23-storage-00-roadmap.md); builds on
[storage-01](2026-09-23-storage-01-block-layer-design.md).

## Goal

Drive SATA disks and SATA optical drives through an AHCI 1.x host bus
adapter, with native command queuing and zero-copy scatter-gather DMA,
and move `build/disk.img` from legacy IDE onto it. Afterwards `/` is
FAT on `/dev/sda`, which is the first disk on the AHCI controller.
`/mnt` stays FAT on `/dev/sdb`, which is `disk2.img` still on legacy
IDE until storage-03. `make test` proves both of these rather than
assuming them.

Scope was chosen as "full": it covers real-hardware bring-up (BIOS
handoff, HBA reset, staggered spin-up, COMRESET), NCQ with libata-style
error recovery, ATAPI (`srN`), and port multipliers. **Port multiplier
support is built to the spec but not verified.** QEMU emulates no port
multiplier, and its ICH9 AHCI never sets CAP.SPM, so no boot here
reaches that code. It is marked as unverified in its source, in
`docs/abi-compatibility.md` and in this spec (§6).

## Non-goals

- **Interrupt-driven completion, and sleeping waits (roadmap D4).**
  Completion is polled. `vfs.c` calls `read_inode` under the vnode-hash
  spinlock, so disk I/O must not sleep.
- **Hot-plug.** PxIS.PCS/PRCS are ignored; the port set is fixed at
  boot.
- **FIS-based switching (FBS) for port multipliers.** Command-based
  switching only.
- **Media change on ATAPI.** Capacity is read once, at probe time.
- **SG_IO and CD-ROM ioctls; writing to optical media; TRIM.**
- **Power management (ALPM, DevSleep); SEMB devices; the q35 machine.**
- **The deferred minor items listed in the handoff**
  (FAT/ramfs short writes at the cap, part.c's silent `-ENOSPC`,
  blkdev_file's position read before the lock). They are not part of
  this milestone.

## 1. Uncached MMIO mapping — `kernel/mm/mmio.{c,h}`

AHCI is the first memory-BAR device NeoOS drives (virtio-net and AC97
both use I/O BARs).

The physmap maps memory write-back. MMIO read through it is only
correct because firmware MTRRs happen to mark the PCI hole uncacheable,
which is true of SeaBIOS but should not be relied on.

```c
// Maps [phys, phys+len) uncached (PCD|PWT → PAT entry 3 = UC) and
// returns its virtual address, or 0. Mappings are permanent.
volatile void *mmio_map(uint64_t phys, uint64_t len);
```

- **The window lives inside PML4[256]**, at
  `MMIO_VIRT_BASE = PHYSMAP_BASE + 256 GiB`, with 1 GiB reserved and a
  bump allocator handing out page-aligned ranges.
  - Only PML4[256] (the physmap) and PML4[511] (the kernel image) are
    copied into process address spaces (`sched/proc.c`).
  - A mapping made under an unshared PML4 slot would vanish the first
    time a driver ran on a process's CR3. That is a latent bug in
    `FB_VIRT_BASE` (PML4[384]), noted but not fixed here.
  - New page tables created below the shared PDPT are visible to every
    address space at once.
- **Page flags:** `PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE |
  PAGE_PCD | PAGE_PWT`. `PAGE_PCD` (bit 4) and `PAGE_PWT` (bit 3) are
  new names in `paging.h`.
- **Locking:** a spinlock at `LOCK_RANK_DRIVER`. It is only used during
  probing.
- **Selftest (`[mmio] selftest passed`):** map a RAM page, write
  through the UC alias, read the value back through the physmap, and
  check that the leaf PTE carries PCD|PWT (walked with
  `paging_translate_in`'s table walk, exposed as
  `paging_leaf_flags(virt)`).

## 2. Shared IDENTIFY parsing — `kernel/drivers/block/ata_id.{c,h}`

```c
struct ata_id {
    uint64_t sectors;          // LBA48 count if supported, else LBA28
    uint32_t logical_sector;   // bytes; 512 unless word 106 says otherwise
    uint8_t  lba48, ncq, fua, write_cache;
    uint8_t  queue_depth;      // 1..32 (word 75 + 1) when ncq
    uint8_t  atapi;            // from word 0 bit 15 (IDENTIFY PACKET data)
    char     serial[21], model[41];   // byte-swapped, trailing spaces trimmed
};
void ata_id_parse(const uint16_t w[256], struct ata_id *out);
```

| field | source |
|---|---|
| `lba48` | word 83 bit 10 |
| `sectors` | words 100–103, or words 60–61 |
| `logical_sector` | if word 106 bits 15:14 = 01 and bit 12 is set, words 117–118 give the size in words (×2 for bytes) |
| `ncq` | word 76 bit 8 |
| `queue_depth` | word 75 bits 4:0 + 1 |
| `fua` | word 84 bit 6 (WRITE DMA FUA EXT / NCQ FUA) |
| `write_cache` | word 85 bit 5 |

Legacy `ata.c` uses it for its sector count and model name (it stays
LBA28 PIO), so both drivers describe a disk the same way.

## 3. Block layer additions — `kernel/block/`

- **`struct blockdev` gains `const char *driver`** (`"ahci"`, `"ata"`,
  `"ram"`; partitions inherit it) and two flags:
  - `BLOCKDEV_RO`: `blockdev_write` returns `-EROFS`, and so does
    `open(2)` for writing (below).
  - `BLOCKDEV_NOPART`: no partition scan. Linux's `sr` driver has one
    minor, so it never has partitions.
- **`blockdev_alloc_name("sr", out)`** returns `sr0`, `sr1`, … and
  `blockdev_register_disk` gives `srN` major 11, minor N (Linux's
  `SCSI_CDROM_MAJOR`). `sd` naming is unchanged. The first free letter
  goes to whichever driver registers first, which is exactly libata's
  probe-order rule.
- **`open(2)` on a `BLOCKDEV_RO` node** with `O_WRONLY` or `O_RDWR`
  fails with `EROFS` (Linux's `sr_block_open` / `cdrom_open`).
  **`BLKROGET`** (`0x125E`, `int`) returns 1 for such a node and 0
  otherwise.
- **`/proc/partitions` lists `sr0`** with its size in 1 KiB blocks, as
  Linux does. No code change: it already uses sector_count ×
  sector_size.
- **Probe order in `kernel.c`:** `ahci_probe()` runs before
  `ata_probe()`.
  - `disk.img` (SATA port 0) becomes `sda` and `disk2.img` (IDE primary
    master) becomes `sdb`. The `/` and `/mnt` mount lines don't change.
  - After mounting `/`, `kernel.c` logs
    `[boot] root: <name> via <driver>`. `make test` requires
    `[boot] root: sda via ahci`, so a silent fall-back to IDE fails the
    test.

## 4. AHCI driver — `kernel/drivers/block/ahci/`

The Makefile's `KERNEL_DIRS` gains the directory. Its files are
`ahci.h` (register layout, internal API), `hba.c`, `port.c`, `disk.c`,
`atapi.c`, `pmp.c` and `selftest.c`.

### 4.1 Discovery and HBA bring-up (`hba.c`)

**Discovery.** For every PCI function with class 01, subclass 06 and
prog-if 01:
- BAR5 must be a memory BAR of non-zero size (otherwise log and skip).
- Map it with `mmio_map`, then set `PCI_CMD_MEM_SPACE` and call
  `pci_enable_bus_master`.

Controllers are numbered in PCI order, and there are at most 4.

**Bring-up**, following AHCI 1.3.1 §10.1 and libata:

1. **BIOS/OS handoff.** If CAP2.BOH is set: set BOHC.OOS, wait up to
   25 ms for BOHC.BOS to clear, and if BOHC.BB is set, wait up to 2 s
   more. On timeout, log `[ahci] BIOS handoff timed out` and continue
   (libata does the same).
2. **Enable and save.** Set GHC.AE, then save CAP and PI.
3. **Reset.** Set GHC.HR and wait up to 1 s for it to clear. On
   timeout, log `[ahci] hba N: reset FAILED` and skip the controller.
4. **Restore.** Set GHC.AE again. If CAP or PI came back as 0, rewrite
   them from the saved values; they are write-once on some firmware.
5. **Disable interrupts and clear state.** Set GHC.IE = 0 and clear IS.
6. **Record capabilities.** From CAP: NP, NCS + 1 (slots), S64A, SNCQ,
   SSS, SPM, SCLO and ISS.

**Port bring-up**, for each bit set in PI:

1. **Stop.** Clear PxCMD.ST and wait up to 500 ms for CR to clear.
   Clear FRE and wait up to 500 ms for FR to clear.
2. **Allocate and program memory** (§4.2): CLB/CLBU and FB/FBU. Then
   clear PxSERR (write all ones) and PxIS, set PxIE = 0, and set FRE.
3. **Spin up.** If CAP.SSS is set, set PxCMD.SUD, one port at a time.
4. **COMRESET.** Set PxSCTL.DET = 1, wait 1 ms or more, set DET = 0,
   then wait up to 1 s for PxSSTS.DET = 3.
   - DET = 0 means `[ahci] port N: no device`.
   - DET = 1 (device present but no link) means
     `[ahci] port N: link down`.
   - Neither is a failure; the port is left stopped.
5. **Clear errors.** Clear PxSERR again.
6. **Wait for the device.** Wait up to 10 s for PxTFD BSY/DRQ to clear.
   On timeout, log `[ahci] port N: device never ready -- FAILED`.
7. **Start.** Set PxCMD.ST.
8. **Classify** by PxSIG, or, when CAP.SPM is set, by the signature
   from a soft reset aimed at PMP 15 (§4.6):

   | PxSIG | device | handled in |
   |---|---|---|
   | `0x00000101` | disk | §4.4 |
   | `0xEB140101` | ATAPI | §4.5 |
   | `0x96690101` | port multiplier | §4.6 |
   | `0xC33C0101` (SEMB) or anything else | unsupported | logged, port stopped |

**Timing.** Every wait is bounded in `ktime`, never in loop iterations
(the lesson from `ata.c`). A failed port logs its registers (CMD, TFD,
SSTS, SERR, IS) and is abandoned on its own; the other ports and the
boot carry on.

### 4.2 Per-port DMA memory

| structure | size / alignment | allocation |
|---|---|---|
| command list | 32 × 32 B = 1 KiB, 1 KiB-aligned | one `pmm_alloc(0)` page shared with the received-FIS area |
| received FIS | 256 B, 256-aligned (at +2 KiB in the same page) | ↑ |
| command tables | 32 × (128 B header + 64 × 16 B PRDT) = 32 × 1152 B, 128-aligned | one `pmm_alloc(4)` (64 KiB) block |

`pmm` never hands out frames above 4 GiB (`PMM_MAX_FRAMES`), so the
DMA structures are always reachable. CAP.S64A is still honoured:
without it, the PRDT builder rejects any buffer address above 4 GiB
with `-EIO` and a log line.

### 4.3 The command engine (`port.c`)

Only NCS slots are used (32 on QEMU).

```c
struct ahci_req {
    uint8_t  fis[20];          // H2D register FIS (built by the caller)
    uint8_t  cdb[16];          // ATAPI packet, when atapi
    uint8_t  atapi, write, ncq, pmp;
    void    *buf;              // kernel virtual; any alignment ≥ 2
    uint32_t len;              // bytes
    // filled by the engine:
    int      slot;             // tag
    int      status;           // 0, -EIO, -ENOMEDIUM, -ETIMEDOUT
    uint8_t  done;
    uint64_t deadline;
};
int  ahci_submit(struct ahci_link *l, struct ahci_req *r);  // non-blocking once a slot is free
int  ahci_wait(struct ahci_link *l, struct ahci_req *r);    // polls; returns r->status
int  ahci_exec(struct ahci_link *l, struct ahci_req *r);    // submit + wait
```

A **link** is a (port, PMP number) pair. A directly attached device is
link PMP 0; a device behind a multiplier is (port, fan-out port).

**Scatter-gather**
- The PRDT is built by walking the buffer one page at a time and
  translating each page through the current CR3's tables
  (`paging_translate_current`, which handles the 2 MiB physmap and
  kernel-image pages).
- Physically adjacent pages merge into one entry of up to 4 MiB.
- An entry's byte count must be even and its address word-aligned, so
  an odd buffer address gets `-EINVAL` plus a log line.
- A request that needs more than 64 entries is refused with `-E2BIG`.
  The disk and ATAPI layers split transfers at 60 pages (240 KiB), which
  can never need more than 61.
- Buffers are always kernel memory: `sys_read`/`sys_write` stage through
  a kernel buffer (`sys_file.c`), so there is never a user page to pin.

**Slots and queueing**
- The per-port lock (`LOCK_RANK_DRIVER`) protects slot allocation, the
  `CI`/`SACT` writes, completion scanning and error recovery. It is
  never held while waiting.
- NCQ commands (`r->ncq`) may be outstanding together, on any number of
  slots. `ahci_submit` sets the tag's bit in PxSACT, then in PxCI.
- A non-queued command is **exclusive**. `ahci_submit` refuses new
  commands while an exclusive command is pending, waits for the port to
  drain, issues it alone, and releases the port when it completes. This
  matches the SATA rule: a device must not receive a non-NCQ command
  while NCQ commands are outstanding.
- **With a port multiplier (command-based switching)**, all outstanding
  commands on a port must target one link. A submit for a different
  link waits for the port to drain.
- When no slot is free, `ahci_submit` polls completions and retries
  until one is.

**Completion**
- `port_poll()` runs under the lock:
  1. Read PxIS. If any error bit is set (TFES, HBFS, HBDS, IFS), run
     error recovery (§4.7).
  2. Read PxSACT and PxCI. Every in-flight slot whose bit has cleared
     in both is marked `done`, with status 0.
- `ahci_wait` calls `port_poll()` until the request is done or its
  deadline passes. The deadline is 30 s, Linux's default SCSI and ATA
  command timeout. It drops the lock between polls and runs `pause`, so
  interrupts and other CPUs keep running.
- A request past its deadline triggers a **port reset** (COMRESET, then
  restart). Every outstanding request fails with `-ETIMEDOUT`, logged
  as `[ahci] port N: command timeout -- FAILED`.

### 4.4 Disks (`disk.c`) — `sdX`

**Probe:** IDENTIFY DEVICE (0xEC, non-queued, 512-byte buffer), then
`ata_id_parse`.
- Logical sector sizes other than 512 and 4096 are refused.
- The disk registers as `sdX` with `driver = "ahci"`.
- Log line:
  `[ahci] port N: sda <sectors> sectors <size> model "<model>" ncq=<depth>`.

**Read and write:**

| command | when |
|---|---|
| READ/WRITE FPDMA QUEUED (0x60 / 0x61), tag in count[7:3], sector count in features | CAP.SNCQ and IDENTIFY NCQ |
| READ/WRITE DMA EXT (0x25 / 0x35) | LBA48 without NCQ |
| READ/WRITE DMA (0xC8 / 0xCA) | LBA28 only |

- One blockdev request is split at 240 KiB. Each chunk is submitted
  before any is waited on, so a large request fills the queue: in-flight
  commands are capped at `min(NCS, queue_depth)`.
- **Write durability matches legacy ATA**, which flushes after every
  write so that a completed write is on the media:
  - Writes carry FUA (NCQ device bit 7, or WRITE DMA FUA EXT 0x3D) when
    the disk supports FUA and has its write cache on.
  - Without FUA and with the cache on, each write is followed by FLUSH
    CACHE EXT (0xEA), or FLUSH CACHE (0xE7) on an LBA28-only disk.
- **`flush`** always issues FLUSH CACHE EXT (non-queued).

### 4.5 ATAPI (`atapi.c`) — `srN`

**Probe**
1. IDENTIFY PACKET DEVICE (0xA1), with the model string taken from it.
2. TEST UNIT READY, retried up to 3 times while the sense is UNIT
   ATTENTION (QEMU reports one after reset).
3. READ CAPACITY(10): the capacity is last LBA + 1, and the block
   length must be 512, 2048 or 4096. With NOT READY / 3A (no medium),
   the drive registers with 0 sectors.
4. Register as `srN` with `BLOCKDEV_RO | BLOCKDEV_NOPART` and
   `driver = "ahci"`.

Log line: `[ahci] port N: sr0 <blocks> blocks of <size> model "<model>"`.

**Read**
- PACKET (0xA0) with features bit 0 (DMA) and command header bit A,
  carrying READ(10) (0x28) as the CDB. Transfers are split at 240 KiB.
- On CHECK CONDITION (TFD.ERR), send REQUEST SENSE (0x03, 18 bytes) and
  map the result: NOT READY → `-ENOMEDIUM`, anything else → `-EIO`.
  The sense key and ASC/ASCQ are logged.

**Write and flush:** write returns `-EROFS` (the blockdev layer refuses
first anyway), and flush returns 0.

### 4.6 Port multipliers (`pmp.c`) — **UNVERIFIED**

This path is only entered when CAP.SPM is set.

**Detection**
1. Set PxCMD.PMA.
2. Soft reset aimed at PMP 15: an H2D FIS with SRST set (command header
   R and C bits), then one with SRST clear, with the signature read from
   the D2H FIS / PxSIG.
3. A PMP signature starts enumeration. Any other signature clears
   PxCMD.PMA and classifies the device as directly attached.

**Enumeration**
1. READ PORT MULTIPLIER (0xE4) on PMP 15 for GSCR[0] (IDs), GSCR[1]
   (revision) and GSCR[2] (fan-out port count, capped at 15). Features
   carries the register number and the device field the port; the value
   comes back in the D2H FIS count/LBA fields.
2. Write GSCR[32]'s error-enable bits to 0 (WRITE PORT MULTIPLIER,
   0xE8).
3. For each fan-out port:
   1. Hard reset through PSCR[2] (SControl): DET = 1, then DET = 0.
   2. Wait up to 1 s for PSCR[0] (SStatus) DET = 3.
   3. Clear PSCR[1] (SError).
   4. Soft reset to that PMP number and read the signature.
   5. Attach a disk or ATAPI device as in §4.4 / §4.5, on link
      (port, n). Every FIS carries the PMP number in byte 1 bits 3:0,
      and the command header carries it in bits 15:12.
4. Log line: `[ahci] port N: port multiplier, M ports`.

**Queueing:** command-based switching only (§4.3). NCQ is used within
one link at a time.

**Unverified:** there is no hardware or emulator to test against. The
code follows AHCI 1.3.1 §9 and SATA PM 1.2. The file carries a banner
saying so, and `docs/abi-compatibility.md` lists it as unverified.

### 4.7 Error recovery

This follows AHCI 1.3.1 §6.2.2 and libata's `ahci_error_handler` /
`ata_eh_analyze_ncq_error`. It runs under the port lock from
`port_poll()`.

1. **Snapshot state.** Read PxCI, PxSACT, PxCMD.CCS, PxTFD, PxSERR and
   PxIS, and log them.
2. **Stop the port.** Clear ST and wait up to 500 ms for CR to clear.
   If CR won't clear, COMRESET (§4.1 steps 4–7) and fail every
   outstanding request with `-EIO`.
3. **Clear errors.** Clear PxSERR and PxIS.
4. **Clear a hung device.** If TFD still shows BSY or DRQ: use CLO
   (PxCMD.CLO, wait for it to clear) when CAP.SCLO is set, otherwise
   COMRESET.
5. **Restart.** Set ST.
6. **Non-queued failure:** the failed slot is the one still set in the
   CI snapshot, per CCS. It completes with `-EIO` (ATAPI: sense
   mapping, §4.5). Nothing else can be outstanding.
7. **NCQ failure:** issue READ LOG EXT (0x2F) for page 10h, non-queued,
   with the NCQ Command Error log.
   - If NQ (byte 0 bit 7) is clear, byte 0 bits 4:0 is the failed tag.
     That request completes with `-EIO`. Every other request still set
     in the SACT snapshot is re-queued as an NCQ command in a fresh
     slot.
   - If the log can't be read, or NQ is set, every outstanding request
     is re-issued one at a time as a **non-queued** DMA EXT command
     ("single-stepping"). Each one that fails there gets `-EIO`.
   - Either way, innocent requests never fail because of a neighbour.
8. **No retries** for a failed command itself: a media error stays an
   error. Only innocent requests are re-issued.

Errors are logged as `[ahci] port N: ... error ...`. **Never with the
word FAILED** except for recovery that itself fails or a timeout, so
that the injected-error test (§7) does not trip `make test`'s FAILED
grep. Every such log line starts `[ahci]`.

## 5. Deferred storage-01 review fixes

**(a) The blkcache read-miss race.**

- `blkcache` gains a `uint64_t write_gen`. Every `blkcache_write_multi`
  (and `blkcache_invalidate`) increments it under `cache_lock`
  **before** writing the device, and again after.
- `blkcache_read`'s miss path records `write_gen` before its unlocked
  device read. It installs the bytes only if `write_gen` is unchanged
  when it relocks. Otherwise it returns the bytes it read (a read
  concurrent with a write may legitimately see either version) without
  caching them.
- The check is global, not per-LBA, so a write to any sector costs a
  racing reader its cache install. That is a missed optimisation, never
  wrong data.
- **Selftest:** `ramblk` gains `ramblk_set_read_hook(d, fn, arg)`,
  called inside the device read. The blkcache selftest's hook does a
  `blkcache_write` of new bytes to the same LBA (the cache lock is
  dropped at that point). The next `blkcache_read` must return the new
  bytes, where it previously returned the stale ones from the cache.

**(b) ATA probe on an empty or non-disk channel.**

`ata_identify_locked`:
1. Selects the drive and reads status. **0xFF** (floating bus, no
   devices) or 0 means `[ata] drive N not present`, with no command
   issued.
2. After IDENTIFY, if ERR is set, or LBA_MID/LBA_HIGH read 0x14/0xEB
   (ATAPI) or 0x69/0x96 (SATA bridge signature), it logs
   `[ata] drive N: not an ATA disk (sig XX/YY), skipped`.

Neither line contains "FAILED". A real timeout on a device that did
answer still says FAILED.

## 6. QEMU layout, test targets, os-builder

**Disks and devices** in every QEMU invocation:

```
-drive file=$(DISK2_IMG),format=raw                       # IDE primary master → sdb
-device ahci,id=sata,addr=0x7
-drive file=$(DISK_IMG),format=raw,if=none,id=sata0
-device ide-hd,drive=sata0,bus=sata.0                       # → sda (/)
-drive file=$(BUILD_DIR)/neoos.iso,format=raw,if=none,id=sata1,media=cdrom,readonly=on
-device ide-cd,drive=sata1,bus=sata.1                       # → sr0
```

- The AHCI function is pinned at `addr=0x7` and placed **after** the
  AC97 device on the command line. Auto-assignment must never be able
  to take slot 6 (AC97) or move virtio-net's slot, which its IRQ reroute
  depends on.
- `-cdrom` (the GRUB boot ISO, IDE secondary master) is unchanged.
- `sr0` is the same ISO read-only. Both opens take shared read locks,
  and SeaBIOS may boot from either CD; they are identical.

**Launchers that change together:**
- `QEMU_COMMON`, which `tools/boot_until.sh` receives verbatim.
- The two hand-written QEMU lines in the `Makefile` (`epolltcp`, and the
  os-builder parity target at ~1160).
- `tools/gauntlet.sh`, with per-run disk copies on the same buses.
- `tools/screenshot.sh`.
- `../neoos-os-builder/Makefile` and `scripts/qemu-run.sh.template`.
  os-builder attaches the disk on AHCI and no `sr0` (its images ship no
  ISO next to the run script).

**`make test` required markers** (added to `CORE_REQUIRED_MARKERS`):
- `[mmio] selftest passed`
- `[ahci] port 0: sda `
- `[ahci] port 1: sr0 `
- `[ahci] selftest passed`
- `[boot] root: sda via ahci`

**`[ahci] selftest`** (every boot, `selftest.c`):
- **FIS encoding:** NCQ read/write FIS fields, including the tag in
  count[7:3] and FUA, and a PMP-addressed FIS's byte 1.
- **PRDT builder:** a buffer spanning a page boundary gives two entries,
  or one when the pages are adjacent. An odd address gets `-EINVAL`.
  Byte counts are right.
- **Concurrent NCQ reads on `sda`:** 8 reads of 16 sectors at spread
  LBAs, all submitted before any is waited on. Their data must match 8
  sequential single reads. The maximum number observed in flight is
  logged, not required.
- **`sr0`:** block 16 begins with `\x01CD001`, the ISO 9660 primary
  volume descriptor, and a write returns `-EROFS`.
- The selftest writes nothing to `sda`. Writes through AHCI are
  exercised by the FAT write selftest and `blkdevtest`.

**`make ahcitest`** (a solo target, not part of `make test` or the
gauntlet, like `blkdevtest`):
- Adds a third SATA disk: a zeroed 8 MiB `build/ahci-scratch.img` with
  `serial=NEOOSSCRATCH`, attached through QEMU's `blkdebug` driver with
  `tools/ahci-blkdebug.conf`. That config injects EIO on `read_aio` at
  sector 4096.
- The kernel runs `ahci_scratch_selftest()` when it finds that serial:
  1. Write 64 KiB patterns at 8 LBAs concurrently (NCQ + FUA), flush,
     and read them back.
  2. Submit 4 concurrent reads, one covering sector 4096. **Exactly
     that one** fails with `-EIO`; the other three return correct data.
     This covers the whole §4.7 NCQ recovery path (log page or
     single-step fallback).
  3. A read after recovery succeeds.
- Pass line: `[ahci] scratch selftest passed`. The target stops on it
  or on a line starting `[ahci]` that contains FAILED.

**`blkdevtest`** (userland, `make blkdevtest`) gains:
- `sr0`: `S_IFBLK`, rdev 11:0, `BLKSSZGET` 2048, `BLKROGET` 1,
  `O_RDWR` → `EROFS`, and `pread` at 32 768 returns `\x01CD001`.
- `sda` `BLKROGET` 0.
- `/proc/partitions` contains `sr0`.

**Gauntlet bar:** 15/15 with zero retries, as for every milestone.

## 7. ABI and documentation

- **`docs/stdlib.md`, "Block devices" section:**
  - SATA/ATA share `sdX` in probe order (AHCI before legacy IDE).
  - `srN` (11:N), read-only, `EROFS` on write-open, `BLKROGET`, and no
    partitions.
  - **DIVERGENCE:** `sr` capacity is fixed at boot (no media-change
    detection), and CD-ROM ioctls/SG_IO return `ENOTTY`.
  - **DIVERGENCE:** writes are FUA/flushed per write (write-through),
    so `fsync` adds nothing (unchanged from storage-01).
- **`docs/abi-compatibility.md`:** a storage-02 refresh covering what
  is implemented, what is stubbed (hot-plug, FBS, media change, SG_IO),
  **port multipliers built but unverified**, and what a ported
  application would still hit.
- **os-builder:** `docs/BUILD_ORDER.md` needs no port change. The QEMU
  command in its README, if shown, is updated.

## 8. Risks

| risk | mitigation |
|---|---|
| QEMU's AHCI doesn't implement READ LOG EXT page 10h | the single-step fallback (§4.7) needs no log; `make ahcitest` exercises whichever path QEMU takes and logs which |
| QEMU's NCQ ignores FUA | harmless under QEMU (data reaches the image file regardless); FUA is spec-correct for real hardware |
| SeaBIOS leaves the HBA running with commands issued | the HBA reset in §4.1 step 3 |
| auto PCI slot assignment moves the NIC | `addr=0x7`, placed last |
| PM code rots unexercised | the FIS-encoding selftest covers its PMP field; the banner and docs mark it unverified |
