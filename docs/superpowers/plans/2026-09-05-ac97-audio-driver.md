# AC97 Audio Driver Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give NeoOS its first audio driver -- AC97, targeting QEMU's
`-device AC97` emulation of Intel's ICH 82801AA controller -- with a
fixed 16-bit/stereo/48kHz playback path and an ALSA-shaped
`/dev/snd/*` userland surface, validated by a real automated
DMA-output-to-WAV-file check.

**Architecture:** A new `kernel/drivers/audio/ac97.c` follows this
codebase's existing PCI-driver shape (`virtio_net.c`): probe by
vendor/device ID, program the device directly, route one IOAPIC line,
expose a userland surface through two new devfs nodes using real Linux
`SNDRV_*` ioctl numbers and struct layouts. No new syscalls -- this
rides entirely on the existing `open`/`write`/`ioctl` path.

**Tech Stack:** C (freestanding kernel code), no new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-05-ac97-audio-driver-design.md`

## Global Constraints

- **Fixed format only**: 16-bit signed LE, stereo, 48000 Hz. Any
  `hw_params` request outside this returns `-EINVAL` -- this is a
  stated hardware-capability limitation, not a shape divergence.
- **Playback only**: no capture path, no `/dev/snd/pcmC0D0c`.
- **QEMU-emulated hardware only**: `8086:2415`. No claim of real-
  hardware support -- there is no physical machine to validate it in
  this environment.
- **Real Linux ABI shapes**: `struct snd_pcm_hw_params`,
  `struct snd_pcm_sw_params`, `struct snd_ctl_card_info`, and every
  `SNDRV_*` ioctl number used here are copied byte-for-byte from
  upstream Linux's `include/uapi/sound/asound.h` (verified against
  that source while writing this plan, not reconstructed from memory).
- **No gauntlet regression**: every existing `REQUIRED_MARKERS` entry
  must still print; this plan only adds new markers, never removes or
  reshapes existing ones.
- **This lands in `neoos-kernel` directly** -- same repo as every
  other in-tree driver (`ata.c`, `virtio_net.c`), not a separate repo.

---

### Task 1: PCI probe, controller reset, codec-ready poll, volume setup

**Files:**
- Create: `kernel/drivers/audio/ac97.h`
- Create: `kernel/drivers/audio/ac97.c`
- Modify: `kernel/kernel.c` (call `ac97_init()` during boot)
- Modify: `Makefile` (add `kernel/drivers/audio/ac97.c` to the source list)

**Interfaces:**
- Produces: `int ac97_init(void);` (0 = device found and brought up,
  -1 = no device -- Task 2 wires the IRQ using this result, same
  pattern as `virtio_net_init()`'s caller in `kernel.c`).
- Produces: `void ac97_selftest(void);` ("[ac97] selftest ...").
- Consumes: `pci_find_id`, `pci_config_read32`/`write32`,
  `pci_enable_bus_master` from `kernel/drivers/pci/pci.h` (already
  used by `virtio_net.c` this same way).

- [ ] **Step 1: Create `kernel/drivers/audio/ac97.h`**

```c
#ifndef NEOOS_AC97_H
#define NEOOS_AC97_H

// AC97 (Intel ICH 82801AA) playback driver. QEMU's -device AC97
// emulates exactly this controller -- see
// docs/superpowers/specs/2026-09-05-ac97-audio-driver-design.md.
//
// Returns 0 if a device was found and brought up (codec responded),
// -1 if no device is present (a machine without one is valid -- not
// a FAILED condition).
int ac97_init(void);

// "[ac97] selftest ..." -- structural checks only (register state,
// codec responded, DMA completed), never an audible check.
void ac97_selftest(void);

// The PCI IRQ line this device is on, valid only after ac97_init()
// returns 0. Same shape as virtio_net_irq_line() -- kernel.c reads
// this to program the IOAPIC redirection itself; the driver does not
// touch the IOAPIC.
uint8_t ac97_irq_line(void);

// Called from kernel/arch/isr.c's VECTOR_AC97 branch.
void ac97_irq(void);

#endif
```

- [ ] **Step 2: Create `kernel/drivers/audio/ac97.c` -- registers, probe, reset, volume**

```c
#include "drivers/audio/ac97.h"
#include "drivers/pci/pci.h"
#include "drivers/char/serial.h"
#include "arch/io.h"
#include <stdint.h>
#include <stddef.h>

// QEMU's -device AC97 emulates Intel's 82801AA ICH AC'97 controller.
#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415

// ---- NAM (Native Audio Mixer) registers -- BAR0, 16-bit each -------
#define NAM_RESET            0x00
#define NAM_MASTER_VOLUME    0x02
#define NAM_PCM_OUT_VOLUME   0x18
#define NAM_EXT_AUDIO_ID     0x2C

// ---- NABM (Native Audio Bus Master) registers -- BAR1 --------------
// Three 16-byte-wide DMA engine blocks; only PCM-OUT (offset 0x10) is
// used (this is a playback-only driver -- spec section 2).
#define NABM_PCM_OUT_BASE    0x10
#define NABM_BDBAR           0x00   // +engine base, 4 bytes
#define NABM_CIV             0x04   // +engine base, 1 byte, read-only
#define NABM_LVI             0x05   // +engine base, 1 byte
#define NABM_SR              0x06   // +engine base, 2 bytes
#define NABM_PICB            0x08   // +engine base, 2 bytes, read-only
#define NABM_PIV              0x0A  // +engine base, 1 byte, read-only
#define NABM_CR              0x0B   // +engine base, 1 byte

#define SR_DCH    0x01   // DMA controller halted
#define SR_CELV   0x02
#define SR_LVBCI  0x04   // last valid buffer completion interrupt
#define SR_BCIS   0x08   // buffer completion interrupt status (write-1-clear)
#define SR_FIFOE  0x10   // FIFO error

#define CR_RPBM   0x01   // run/pause bus master
#define CR_RR     0x02   // reset registers
#define CR_LVBIE  0x04   // last-valid-buffer interrupt enable
#define CR_FEIE   0x08   // FIFO error interrupt enable
#define CR_IOCE   0x10   // interrupt-on-completion enable

#define NABM_GLOB_CNT  0x2C   // 4 bytes
#define NABM_GLOB_STA  0x30   // 4 bytes
#define NABM_CAS       0x34   // 1 byte

#define GLOB_CNT_COLD_RESET  0x02   // set = codec released from reset
#define GLOB_STA_PCR         0x100  // primary codec ready

static uint16_t nam_base, nabm_base;   // BAR0/BAR1, I/O port space
static uint8_t  ac97_present;
static uint8_t  ac97_irq_line_val;

static uint16_t nam_read16(uint16_t reg)  { return inw((uint16_t)(nam_base + reg)); }
static void     nam_write16(uint16_t reg, uint16_t v) { outw((uint16_t)(nam_base + reg), v); }

static uint32_t nabm_read32(uint16_t reg) { return inl((uint16_t)(nabm_base + reg)); }
static void     nabm_write32(uint16_t reg, uint32_t v) { outl((uint16_t)(nabm_base + reg), v); }
static uint16_t nabm_read16(uint16_t reg) { return inw((uint16_t)(nabm_base + reg)); }
static void     nabm_write16(uint16_t reg, uint16_t v) { outw((uint16_t)(nabm_base + reg), v); }
static uint8_t  nabm_read8(uint16_t reg)  { return inb((uint16_t)(nabm_base + reg)); }
static void     nabm_write8(uint16_t reg, uint8_t v) { outb((uint16_t)(nabm_base + reg), v); }

uint8_t ac97_irq_line(void) { return ac97_irq_line_val; }

int ac97_init(void) {
    ac97_present = 0;

    const struct pci_device *pci = pci_find_id(AC97_VENDOR, AC97_DEVICE);
    if (!pci) {
        serial_write_string("[ac97] no device (not a FAILED: a machine "
                            "without one is a valid machine)\n");
        return -1;
    }

    // BAR0 = NAM (mixer), BAR1 = NABM (bus master), both I/O space.
    // pci_find_id already decoded and sized every BAR (struct pci_bar
    // has the type bits already masked off) -- use that directly
    // rather than re-reading raw config space.
    if (!pci->bar[0].is_io || !pci->bar[1].is_io) {
        serial_write_string("[ac97] FAILED: expected I/O-space BARs\n");
        return -1;
    }
    nam_base  = (uint16_t)pci->bar[0].addr;
    nabm_base = (uint16_t)pci->bar[1].addr;
    ac97_irq_line_val = pci->irq_line;

    pci_enable_bus_master(pci);

    // Cold reset: release the codec from reset by setting GLOB_CNT bit1.
    uint32_t glob_cnt = nabm_read32(NABM_GLOB_CNT);
    nabm_write32(NABM_GLOB_CNT, glob_cnt | GLOB_CNT_COLD_RESET);

    // Poll GLOB_STA.PCR (primary codec ready) -- bounded, not infinite:
    // a codec that never becomes ready is a real failure, not a hang.
    int ready = 0;
    for (int i = 0; i < 100000; i++) {
        if (nabm_read32(NABM_GLOB_STA) & GLOB_STA_PCR) { ready = 1; break; }
    }
    if (!ready) {
        serial_write_string("[ac97] FAILED: codec never became ready\n");
        return -1;
    }

    uint16_t ext_id = nam_read16(NAM_EXT_AUDIO_ID);
    serial_write_string("[ac97] codec ready, ext_audio_id=");
    serial_write_hex64(ext_id);
    serial_write_string("\n");

    // Unmute + full volume (0x0000 = loudest, bit15 = mute).
    nam_write16(NAM_MASTER_VOLUME, 0x0000);
    nam_write16(NAM_PCM_OUT_VOLUME, 0x0000);

    ac97_present = 1;
    serial_write_string("[ac97] device found\n");
    return 0;
}

void ac97_selftest(void) {
    if (!ac97_present) {
        serial_write_string("[ac97] selftest skipped (no device)\n");
        return;
    }
    uint32_t sta = nabm_read32(NABM_GLOB_STA);
    if (!(sta & GLOB_STA_PCR)) {
        serial_write_string("[ac97] selftest FAILED: codec no longer ready\n");
        return;
    }
    serial_write_string("[ac97] selftest passed\n");
}

void ac97_irq(void) {
    // Populated in Task 2, once there is a stream to acknowledge.
}
```

- [ ] **Step 3: Wire `ac97_init()` into `kernel/kernel.c`**

Find the `virtio_net_init()` call block (the one that ends with
`serial_write_hex64(virtio_net_irq_line()); serial_write_string(" vector=0x22\n");`)
and add immediately after its closing `}`:

```c
    if (ac97_init() == 0) {
        // IRQ routing for the AC97 line is added in Task 2, once
        // there is an interrupt to acknowledge -- probing/reset
        // alone need no interrupt.
    }
```

Add the include near the other driver includes at the top of
`kernel/kernel.c`:

```c
#include "drivers/audio/ac97.h"
```

- [ ] **Step 4: Add `ac97_selftest()` to the boot selftest sequence**

Find where `fb_device_selftest()` (or another driver selftest, e.g.
`fbcon_selftest()`) is called during boot in `kernel/kernel.c`, and add
immediately after it:

```c
    ac97_selftest();
```

- [ ] **Step 5: Add the new file to the build**

In `Makefile`, find the line listing `kernel/drivers/video/vesafb.c`
(or another `kernel/drivers/*/*.c` entry) in the kernel source list and
add `kernel/drivers/audio/ac97.c` alongside it, following this
Makefile's existing pattern for how `kernel/drivers/*/*.c` files are
discovered/listed (check whether it globs `kernel/drivers/*/*.c`
automatically -- if so, no Makefile change is needed at all; only add
an explicit line if sources are listed by hand).

- [ ] **Step 6: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output
```

Expected: clean build.

- [ ] **Step 7: Boot with AC97 emulation and check the markers**

The gauntlet's `QEMU_COMMON` flags need `-device AC97` added (find that
variable in `Makefile` and add `-device AC97` to it, matching how other
emulated devices already there, e.g. `-device virtio-net`, are listed).

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
grep "\[ac97\]" build/serial.log
```

Expected: `[ac97] codec ready, ext_audio_id=...`, `[ac97] device
found`, `[ac97] selftest passed`.

- [ ] **Step 8: Commit**

```bash
git add kernel/drivers/audio/ac97.h kernel/drivers/audio/ac97.c \
        kernel/kernel.c Makefile
git commit -m "audio: AC97 controller probe, reset, codec-ready poll, volume setup"
```

---

### Task 2: BDL, DMA playback, IRQ, and the automated WAV-file test

**Files:**
- Modify: `kernel/drivers/audio/ac97.h`
- Modify: `kernel/drivers/audio/ac97.c`
- Modify: `kernel/kernel.c` (IOAPIC routing for the AC97 line)
- Modify: `kernel/arch/isr.c` (dispatch `VECTOR_AC97` to `ac97_irq()`)
- Modify: `Makefile` (QEMU `-audiodev wav` backend + gauntlet marker)

**Interfaces:**
- Consumes: `uint64_t pmm_alloc(unsigned order)` /
  `void *phys_to_virt(uint64_t phys)` from `kernel/mm/pmm.h` /
  `kernel/mm/paging.h` (same pattern `virtio_net.c` uses for its RX
  pool -- `rx_pool_phys = pmm_alloc(rx_pool_order);` then
  `phys_to_virt(...)` to touch it from the CPU).
- Consumes: `void ioapic_set_redirection(uint8_t pin, uint8_t vector,
  uint8_t polarity, uint8_t trigger, uint8_t dest_apic_id);` from
  `kernel/drivers/irq/ioapic.h`, and `acpi.ioapic_gsi_base` /
  `lapic_get_id()` -- same exact pattern as `kernel.c`'s existing
  virtio-net IRQ routing block.
- Produces: extends `ac97_selftest()` to also drive one DMA burst.

- [ ] **Step 1: Add the BDL entry type and buffer sizing to `ac97.h`**

```c
#include <stdint.h>

// One BDL, 4 fixed-size segments, 4096 frames each (16-bit stereo:
// 4096 frames * 2 channels * 2 bytes = 16384 bytes/segment, ~85ms;
// ~341ms total buffer -- see spec section 3.3).
#define AC97_BDL_ENTRIES     4
#define AC97_FRAMES_PER_SEG  4096
#define AC97_BYTES_PER_SEG   (AC97_FRAMES_PER_SEG * 2 /*channels*/ * 2 /*bytes/sample*/)

struct ac97_bdl_entry {
    uint32_t buf_phys;
    uint16_t length;   // SAMPLES (16-bit words): frames * channels
    uint16_t flags;    // bit15 IOC (0x8000), bit14 BUP (0x4000)
} __attribute__((packed));
_Static_assert(sizeof(struct ac97_bdl_entry) == 8, "AC97 BDL ABI");
```

- [ ] **Step 2: Add the BDL/DMA setup and IRQ handler to `ac97.c`**

Add near the top, with the other includes:

```c
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sync/waitq.h"
#include "sync/lock.h"
```

Add after the existing static state (`nam_base`, etc.):

```c
static struct ac97_bdl_entry *bdl;     // 4 entries, one page, never freed
static uint64_t bdl_phys;
static uint8_t *seg_virt[AC97_BDL_ENTRIES];
static uint64_t seg_phys[AC97_BDL_ENTRIES];
static volatile uint32_t next_write_seg;   // next segment software may fill
static volatile uint32_t irq_count;        // observed by the selftest
static struct waitq  space_waitq;
static struct spinlock ac97_lock;

// Programs the PCM-OUT DMA engine with the BDL and starts it.
static void ac97_start_dma(void) {
    nabm_write32(NABM_PCM_OUT_BASE + NABM_BDBAR, (uint32_t)bdl_phys);
    nabm_write8(NABM_PCM_OUT_BASE + NABM_LVI, AC97_BDL_ENTRIES - 1);
    nabm_write8(NABM_PCM_OUT_BASE + NABM_CR, CR_RPBM | CR_IOCE);
}

// Fills segment `seg` with silence (used to arm buffers before the
// first real write, so DMA has valid data to play immediately).
static void ac97_fill_silence(uint32_t seg) {
    uint8_t *p = seg_virt[seg];
    for (uint32_t i = 0; i < AC97_BYTES_PER_SEG; i++) { p[i] = 0; }
}

static void ac97_setup_bdl(void) {
    bdl_phys = pmm_alloc(0);      // one 4KiB page: 4 * 8-byte entries fits easily
    bdl = (struct ac97_bdl_entry *)phys_to_virt(bdl_phys);

    // One contiguous pool for all 4 segments, never freed -- same
    // style as virtio_net's rx_pool_phys.
    uint64_t need = (uint64_t)AC97_BDL_ENTRIES * AC97_BYTES_PER_SEG;
    unsigned order = 0;
    while (((uint64_t)4096 << order) < need) { order++; }
    uint64_t pool_phys = pmm_alloc(order);
    uint8_t *pool_virt = (uint8_t *)phys_to_virt(pool_phys);

    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        seg_phys[i] = pool_phys + (uint64_t)i * AC97_BYTES_PER_SEG;
        seg_virt[i] = pool_virt + (uint64_t)i * AC97_BYTES_PER_SEG;
        bdl[i].buf_phys = (uint32_t)seg_phys[i];
        bdl[i].length   = AC97_FRAMES_PER_SEG * 2;   // samples, not bytes
        bdl[i].flags    = 0x8000;                    // IOC on every segment
        ac97_fill_silence((uint32_t)i);
    }

    waitq_init(&space_waitq);
    spin_init(&ac97_lock, LOCK_RANK_AC97, "ac97");
    next_write_seg = 0;
    irq_count = 0;

    ac97_start_dma();
}
```

Replace the placeholder `ac97_irq()` body with:

```c
void ac97_irq(void) {
    uint16_t sr = nabm_read16(NABM_PCM_OUT_BASE + NABM_SR);
    if (sr & SR_BCIS) {
        nabm_write16(NABM_PCM_OUT_BASE + NABM_SR, SR_BCIS);   // write-1-to-clear
        irq_count++;
        waitq_wake_all(&space_waitq);
    }
    if (sr & SR_FIFOE) {
        nabm_write16(NABM_PCM_OUT_BASE + NABM_SR, SR_FIFOE);
    }
}
```

Call `ac97_setup_bdl()` from the end of `ac97_init()`, right before
`ac97_present = 1;`:

```c
    ac97_setup_bdl();
```

Add to `kernel/sync/lock.h`, immediately after `#define
LOCK_RANK_LOOPBACK 27` (the last of that file's low-numbered
single-driver-lock cluster -- `ac97_lock` is taken alone, never nested
under or within another lock, the same profile as `LOCK_RANK_INPUT`/
`LOCK_RANK_VIRTIO_TX`/`LOCK_RANK_LOOPBACK` right above it):

```c
#define LOCK_RANK_AC97 28
```

Do not renumber any existing rank -- this only adds the next unused
number in that cluster.

- [ ] **Step 3: Route the IRQ in `kernel/kernel.c`**

Replace the Task 1 placeholder block:

```c
    if (ac97_init() == 0) {
        // IRQ routing for the AC97 line is added in Task 2, once
        // there is an interrupt to acknowledge -- probing/reset
        // alone need no interrupt.
    }
```

with:

```c
    if (ac97_init() == 0) {
        uint8_t ac97_pin = (uint8_t)(ac97_irq_line() - acpi.ioapic_gsi_base);
        ioapic_set_redirection(ac97_pin, VECTOR_AC97,
                               1 /* active-low */, 1 /* level */,
                               (uint8_t)lapic_get_id());
        serial_write_string("[ioapic] ac97 routed: gsi=");
        serial_write_hex64(ac97_irq_line());
        serial_write_string(" vector=0x23\n");
    }
```

- [ ] **Step 4: Add `VECTOR_AC97` and dispatch it in `isr.c`**

In `kernel/drivers/audio/ac97.h`, add:

```c
#define VECTOR_AC97 0x23
```

In `kernel/arch/isr.c`, add immediately after the `VECTOR_VIRTIO_NET`
branch (before `unhandled_interrupt`):

```c
    if (regs->vector_number == VECTOR_AC97) {
        ac97_irq();
        lapic_send_eoi();
        return;
    }
```

Add `#include "drivers/audio/ac97.h"` to `isr.c`'s includes.

- [ ] **Step 5: Extend the selftest to drive one real DMA burst**

Replace `ac97_selftest()`'s body (from Task 1) with:

```c
void ac97_selftest(void) {
    if (!ac97_present) {
        serial_write_string("[ac97] selftest skipped (no device)\n");
        return;
    }
    uint32_t sta = nabm_read32(NABM_GLOB_STA);
    if (!(sta & GLOB_STA_PCR)) {
        serial_write_string("[ac97] selftest FAILED: codec no longer ready\n");
        return;
    }

    // Write one segment's worth of a simple 480 Hz square wave (100
    // samples high, 100 low, at 48kHz -- integer-only, no FPU: this
    // kernel is built -mno-sse) and wait for its completion IRQ. This
    // is the structural proof DMA actually moves data, matching
    // fb_device_selftest's "prove the driver's real path works, not
    // just that a function pointer is non-NULL" standard.
    uint32_t before = irq_count;
    uint16_t *samples = (uint16_t *)seg_virt[0];
    for (uint32_t i = 0; i < AC97_FRAMES_PER_SEG; i++) {
        int16_t v = ((i / 100) % 2) ? 8000 : -8000;
        samples[i * 2 + 0] = (uint16_t)v;   // left
        samples[i * 2 + 1] = (uint16_t)v;   // right
    }

    uint64_t f = spin_lock_irqsave(&ac97_lock);
    while (irq_count == before) {
        if (waitq_sleep(&space_waitq, &ac97_lock) < 0) { break; }
    }
    spin_unlock_irqrestore(&ac97_lock, f);

    if (irq_count == before) {
        serial_write_string("[ac97] selftest FAILED: no completion interrupt\n");
        return;
    }
    serial_write_string("[ac97] selftest passed\n");
}
```

- [ ] **Step 6: Wire QEMU's WAV-file audio backend + gauntlet marker**

In `Makefile`, find `QEMU_COMMON` and add an `-audiodev` backend plus
`-device AC97` (if not already added in Task 1 Step 7) wired to it:

```
QEMU_COMMON += -audiodev wav,id=ac97wav,path=$(BUILD_DIR)/ac97-test.wav \
               -device AC97,audiodev=ac97wav
```

Add `[ac97] selftest passed` to `REQUIRED_MARKERS` in the `Makefile`
(find that variable -- it is a space-separated list `make test`/the
gauntlet already checks every entry of).

Add a WAV-file assertion to the `test` target, immediately after its
existing marker-checking loop (`@for m in $(REQUIRED_MARKERS); do ...`)
and before the final `@echo "PASS: ..."` line:

```makefile
	@if [ ! -s $(BUILD_DIR)/ac97-test.wav ]; then \
		echo "AC97 WAV FILE MISSING OR EMPTY: no audio was captured"; exit 1; fi
	@wav_bytes=$$(stat -c%s $(BUILD_DIR)/ac97-test.wav 2>/dev/null || stat -f%z $(BUILD_DIR)/ac97-test.wav); \
	if [ "$$wav_bytes" -lt 1000 ]; then \
		echo "AC97 WAV FILE TOO SMALL ($$wav_bytes bytes): DMA likely did not run"; exit 1; fi
```

(A precise expected-byte-count match is deliberately not required here
-- QEMU's `wav` audiodev backend's exact framing/header overhead isn't
worth pinning exactly; "meaningfully non-empty" is what proves DMA
actually produced audio, which is the property this test exists to
check.)

- [ ] **Step 7: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output
```

Expected: clean build.

- [ ] **Step 8: Boot and check the WAV file + markers**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
grep "\[ac97\]\|\[ioapic\] ac97" build/serial.log
ls -la build/ac97-test.wav
```

Expected: `[ac97] device found`, `[ioapic] ac97 routed: ...`, `[ac97]
selftest passed`, and `build/ac97-test.wav` present and non-trivially
sized (the `test` target's own check already asserts this, but confirm
by eye too).

- [ ] **Step 9: Commit**

```bash
git add kernel/drivers/audio/ac97.h kernel/drivers/audio/ac97.c \
        kernel/kernel.c kernel/arch/isr.c kernel/sync/lock.h Makefile
git commit -m "audio: AC97 BDL/DMA playback, IRQ handling, automated WAV-output test"
```

---

### Task 3: `/dev/snd/controlC0` + `/dev/snd/pcmC0D0p` ALSA-shaped surface

**Files:**
- Create: `kernel/drivers/audio/snd_abi.h` (the verbatim upstream
  Linux structs/ioctl numbers, isolated in their own header so their
  origin and exactness are easy to audit separately from driver logic)
- Modify: `kernel/drivers/audio/ac97.h`
- Modify: `kernel/drivers/audio/ac97.c` (the two new `file_ops`
  implementations + a write path that feeds the BDL from Task 2)
- Modify: `kernel/fs/devfs.c` (new nodes)

**Interfaces:**
- Consumes: `struct ac97_bdl_entry`, `seg_virt`/`seg_phys`,
  `next_write_seg`, `space_waitq`, `ac97_lock` from Task 2 (same
  translation unit -- these stay file-static in `ac97.c`, no new
  cross-file interface needed for them).
- Consumes: `struct file_ops` from `kernel/fs/file.h` (fields: `name`,
  `read`, `write`, `lseek`, `getdents`, `ioctl`, `poll`, `poll_head`,
  `mmap`, `dup`, `close` -- exact shape already used by `fb_file_ops`
  in `vesafb.c`).
- Produces: `extern const struct file_ops ac97_control_file_ops;`,
  `extern const struct file_ops ac97_pcm_file_ops;`,
  `int ac97_control_open(struct file_descriptor *f);`,
  `int ac97_pcm_open(struct file_descriptor *f);` -- `devfs.c`'s new
  table entries reference these by name, exactly like `fb.h`'s
  `fb_file_ops`/`fb_open` are referenced from `devfs.c` today.

- [ ] **Step 1: Create `kernel/drivers/audio/snd_abi.h`**

Every struct and ioctl number below is copied verbatim from upstream
Linux's `include/uapi/sound/asound.h` (fetched and checked against that
source while writing this plan -- not reconstructed from memory, per
this project's Linux-ABI-shape rule).

```c
#ifndef NEOOS_SND_ABI_H
#define NEOOS_SND_ABI_H

#include <stdint.h>

typedef unsigned long snd_pcm_uframes_t;
typedef signed long   snd_pcm_sframes_t;

#define SNDRV_MASK_MAX 256
struct snd_mask {
    uint32_t bits[(SNDRV_MASK_MAX + 31) / 32];   // 8 x uint32_t = 32 bytes
};
_Static_assert(sizeof(struct snd_mask) == 32, "snd_mask ABI");

struct snd_interval {
    unsigned int min, max;
    unsigned int openmin:1, openmax:1, integer:1, empty:1;
};
_Static_assert(sizeof(struct snd_interval) == 12, "snd_interval ABI");

#define SNDRV_PCM_HW_PARAM_ACCESS        0
#define SNDRV_PCM_HW_PARAM_FORMAT        1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT     2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK    SNDRV_PCM_HW_PARAM_ACCESS
#define SNDRV_PCM_HW_PARAM_LAST_MASK     SNDRV_PCM_HW_PARAM_SUBFORMAT

#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS   8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS    9
#define SNDRV_PCM_HW_PARAM_CHANNELS      10
#define SNDRV_PCM_HW_PARAM_RATE          11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME   12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE   13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES  14
#define SNDRV_PCM_HW_PARAM_PERIODS       15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME   16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE   17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES  18
#define SNDRV_PCM_HW_PARAM_TICK_TIME     19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL SNDRV_PCM_HW_PARAM_SAMPLE_BITS
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL  SNDRV_PCM_HW_PARAM_TICK_TIME

#define SNDRV_PCM_FORMAT_S16_LE  2

struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1];
    struct snd_mask mres[5];
    struct snd_interval intervals[SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1];
    struct snd_interval ires[9];
    unsigned int rmask;
    unsigned int cmask;
    unsigned int info;
    unsigned int msbits;
    unsigned int rate_num;
    unsigned int rate_den;
    snd_pcm_uframes_t fifo_size;
    unsigned char sync[16];
    unsigned char reserved[48];
};
_Static_assert(sizeof(struct snd_pcm_hw_params) == 608, "snd_pcm_hw_params ABI");

struct snd_pcm_sw_params {
    int tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    snd_pcm_uframes_t avail_min;
    snd_pcm_uframes_t xfer_align;
    snd_pcm_uframes_t start_threshold;
    snd_pcm_uframes_t stop_threshold;
    snd_pcm_uframes_t silence_threshold;
    snd_pcm_uframes_t silence_size;
    snd_pcm_uframes_t boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
};
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136, "snd_pcm_sw_params ABI");

struct snd_ctl_card_info {
    int card;
    int pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved_[16];
    unsigned char mixername[80];
    unsigned char components[128];
};
_Static_assert(sizeof(struct snd_ctl_card_info) == 376, "snd_ctl_card_info ABI");

// _IOC-encoded ioctl numbers, precomputed (dir<<30 | size<<16 | type<<8
// | nr) from upstream's _IOR/_IOW/_IOWR/_IO macro invocations -- same
// "precomputed hex, comment names the source macro" style fb.h already
// uses for FBIOGET_VSCREENINFO etc.
#define SNDRV_CTL_IOCTL_PVERSION   0x80045500u   // _IOR('U',0x00,int)
#define SNDRV_CTL_IOCTL_CARD_INFO  0x81785501u   // _IOR('U',0x01,struct snd_ctl_card_info)

#define SNDRV_PCM_IOCTL_PVERSION   0x80044100u   // _IOR('A',0x00,int)
#define SNDRV_PCM_IOCTL_HW_PARAMS  0xC2604111u   // _IOWR('A',0x11,struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_SW_PARAMS  0xC0884113u   // _IOWR('A',0x13,struct snd_pcm_sw_params)
#define SNDRV_PCM_IOCTL_PREPARE    0x4140u        // _IO('A',0x40)
#define SNDRV_PCM_IOCTL_START      0x4142u        // _IO('A',0x42)
#define SNDRV_PCM_IOCTL_DROP       0x4143u        // _IO('A',0x43)

#endif
```

- [ ] **Step 2: Run the `_Static_assert`s to catch a struct-layout mistake immediately**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
x86_64-elf-gcc -ffreestanding -c -Ikernel -Ishared \
    -x c -o /tmp/snd_abi_check.o - <<'EOF'
#include "drivers/audio/snd_abi.h"
int dummy;
EOF
```

Expected: compiles with no `_Static_assert` failure. If any assertion
fires, the struct above has a packing mistake -- stop and recheck
field order/types against this step's source header before continuing
(the numbers embedded in the ioctl defines above depend on these exact
sizes being right).

- [ ] **Step 3: Add the two `file_ops` implementations to `ac97.c`**

```c
#include "drivers/audio/snd_abi.h"
#include "fs/file.h"
#include "errno.h"

// ---- /dev/snd/controlC0 ---------------------------------------------
// Opens cleanly, answers PVERSION/CARD_INFO with real values, -ENOTTY
// otherwise -- there is no mixer tree to expose yet (documented
// limitation, see docs/stdlib.md).

static int64_t ctl_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f;
    if (request == SNDRV_CTL_IOCTL_PVERSION) {
        *(int *)arg = 0x02000108;   // ALSA protocol 2.0.8, matches real cards' typical value
        return 0;
    }
    if (request == SNDRV_CTL_IOCTL_CARD_INFO) {
        struct snd_ctl_card_info *ci = (struct snd_ctl_card_info *)arg;
        for (uint64_t i = 0; i < sizeof(*ci); i++) { ((uint8_t *)ci)[i] = 0; }
        ci->card = 0;
        const char id[] = "AC97";
        const char driver[] = "neoos-ac97";
        const char name[] = "AC97 (NeoOS)";
        for (unsigned i = 0; i < sizeof(id); i++) { ci->id[i] = (unsigned char)id[i]; }
        for (unsigned i = 0; i < sizeof(driver); i++) { ci->driver[i] = (unsigned char)driver[i]; }
        for (unsigned i = 0; i < sizeof(name); i++) { ci->name[i] = (unsigned char)name[i]; }
        return 0;
    }
    return -ENOTTY;
}

static int64_t ctl_read(struct file_descriptor *f, void *b, uint64_t n) { (void)f;(void)b;(void)n; return 0; }
static int64_t ctl_write(struct file_descriptor *f, const void *b, uint64_t n) { (void)f;(void)b; return (int64_t)n; }
static int64_t ctl_lseek(struct file_descriptor *f, int64_t o, int w) { (void)f;(void)o;(void)w; return -ESPIPE; }
static int64_t ctl_getdents(struct file_descriptor *f, void *b, int n) { (void)f;(void)b;(void)n; return -ENOTDIR; }
static int     ctl_poll(struct file_descriptor *f, int e) { (void)f; return e; }
static void    ctl_dup(struct file_descriptor *f) { (void)f; }
static void    ctl_close(struct file_descriptor *f) { (void)f; }

const struct file_ops ac97_control_file_ops = {
    .name = "ac97-control", .read = ctl_read, .write = ctl_write,
    .lseek = ctl_lseek, .getdents = ctl_getdents, .ioctl = ctl_ioctl,
    .poll = ctl_poll, .dup = ctl_dup, .close = ctl_close,
};

int ac97_control_open(struct file_descriptor *f) {
    if (!ac97_present) { return -ENODEV; }
    f->ops = &ac97_control_file_ops;
    f->priv = 0;
    return 0;
}

// ---- /dev/snd/pcmC0D0p ----------------------------------------------
// Fixed format only: S16_LE, 2 channels, 48000 Hz. hw_params for
// anything else is -EINVAL -- see Global Constraints.

static int pcm_started;

static int64_t pcm_hw_params(struct snd_pcm_hw_params *hp) {
    struct snd_mask *fmt_mask = &hp->masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    if (!(fmt_mask->bits[0] & (1u << SNDRV_PCM_FORMAT_S16_LE))) { return -EINVAL; }

    struct snd_interval *ch = &hp->intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    if (ch->min > 2 || ch->max < 2) { return -EINVAL; }

    struct snd_interval *rate = &hp->intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    if (rate->min > 48000 || rate->max < 48000) { return -EINVAL; }

    // Pin every negotiated value to the one fixed format -- no
    // interval-refinement algorithm, just "this is the only answer".
    fmt_mask->bits[0] = (1u << SNDRV_PCM_FORMAT_S16_LE);
    ch->min = ch->max = 2; ch->integer = 1;
    rate->min = rate->max = 48000; rate->integer = 1;
    hp->fifo_size = AC97_FRAMES_PER_SEG;
    return 0;
}

static int64_t pcm_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f;
    if (request == SNDRV_PCM_IOCTL_PVERSION) { *(int *)arg = 0x02000108; return 0; }
    if (request == SNDRV_PCM_IOCTL_HW_PARAMS) { return pcm_hw_params((struct snd_pcm_hw_params *)arg); }
    if (request == SNDRV_PCM_IOCTL_SW_PARAMS) { return 0; }   // fixed buffer/period sizes, nothing to negotiate
    if (request == SNDRV_PCM_IOCTL_PREPARE) { next_write_seg = 0; return 0; }
    if (request == SNDRV_PCM_IOCTL_START) { pcm_started = 1; return 0; }
    if (request == SNDRV_PCM_IOCTL_DROP) { pcm_started = 0; return 0; }
    return -ENOTTY;
}

static int64_t pcm_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    if (!ac97_present) { return -ENODEV; }
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t written = 0;

    uint64_t fl = spin_lock_irqsave(&ac97_lock);
    while (written < len) {
        uint64_t chunk = len - written;
        if (chunk > AC97_BYTES_PER_SEG) { chunk = AC97_BYTES_PER_SEG; }

        // Block until the segment we are about to overwrite is not the
        // one DMA is currently reading from -- CIV is read-only
        // hardware state, so "safe to write" means "not CIV".
        while (next_write_seg == nabm_read8(NABM_PCM_OUT_BASE + NABM_CIV)) {
            if (waitq_sleep(&space_waitq, &ac97_lock) < 0) {
                spin_unlock_irqrestore(&ac97_lock, fl);
                return written ? (int64_t)written : -EINTR;
            }
        }

        for (uint64_t i = 0; i < chunk; i++) { seg_virt[next_write_seg][i] = src[written + i]; }
        next_write_seg = (next_write_seg + 1) % AC97_BDL_ENTRIES;
        written += chunk;
    }
    spin_unlock_irqrestore(&ac97_lock, fl);
    return (int64_t)written;
}

static int64_t pcm_read(struct file_descriptor *f, void *b, uint64_t n) { (void)f;(void)b;(void)n; return -EINVAL; }
static int64_t pcm_lseek(struct file_descriptor *f, int64_t o, int w) { (void)f;(void)o;(void)w; return -ESPIPE; }
static int64_t pcm_getdents(struct file_descriptor *f, void *b, int n) { (void)f;(void)b;(void)n; return -ENOTDIR; }
static int     pcm_poll(struct file_descriptor *f, int e) { (void)f; return e & 0x4 /* POLLOUT */; }
static void    pcm_dup(struct file_descriptor *f) { (void)f; }
static void    pcm_close(struct file_descriptor *f) { (void)f; pcm_started = 0; }

const struct file_ops ac97_pcm_file_ops = {
    .name = "ac97-pcm", .read = pcm_read, .write = pcm_write,
    .lseek = pcm_lseek, .getdents = pcm_getdents, .ioctl = pcm_ioctl,
    .poll = pcm_poll, .dup = pcm_dup, .close = pcm_close,
};

int ac97_pcm_open(struct file_descriptor *f) {
    if (!ac97_present) { return -ENODEV; }
    f->ops = &ac97_pcm_file_ops;
    f->priv = 0;
    return 0;
}
```

Add to `ac97.h`:

```c
struct file_descriptor;
extern const struct file_ops ac97_control_file_ops;
extern const struct file_ops ac97_pcm_file_ops;
int ac97_control_open(struct file_descriptor *f);
int ac97_pcm_open(struct file_descriptor *f);
```

- [ ] **Step 4: Add `/dev/snd/*` devfs nodes**

Before making this change, confirm the current entry count and the
`pts` invariant have not shifted since this plan was written:

```bash
grep -c '{ "' kernel/fs/devfs.c
```

Expected: 19 (if this differs, the numeric inode IDs referenced below
need recomputing against the actual current array before continuing --
do not proceed on a stale count).

In `kernel/fs/devfs.c`, add `#include "drivers/audio/ac97.h"` to the
top includes, and insert these three lines into the `devices[]` array
**immediately before** the `{ "pts", VNODE_DIR, NULL, NULL },` line
(must stay last -- `DEVFS_PTS_INODE == DEVFS_COUNT`):

```c
    { "snd",              VNODE_DIR,    NULL,                    NULL },
    { "snd/controlC0",    VNODE_DEVICE, &ac97_control_file_ops,  ac97_control_open },
    { "snd/pcmC0D0p",     VNODE_DEVICE, &ac97_pcm_file_ops,      ac97_pcm_open },
```

With the array at 19 entries before this change, `"snd"` becomes inode
20, `"snd/controlC0"` inode 21, `"snd/pcmC0D0p"` inode 22 (array
position + 1) -- matching how `"input"`=5/`"input/event0"`=6 are
already hardcoded.

Find `devfs_lookup`'s existing `if (dir && dir->inode_id == 5)` branch
(the "we're in the input directory" check) and add immediately after
its closing `}`:

```c
    if (dir && dir->inode_id == 20) {   // "snd" directory
        if (name_eq(name, "controlC0")) { *out_inode_id = 21; return 0; }
        if (name_eq(name, "pcmC0D0p"))  { *out_inode_id = 22; return 0; }
        return -ENOENT;
    }
```

Find `devfs_lookup`'s hierarchical-entry root-level branch (the `if
(has_slash) { if (name_eq(name, "input")) ... }` block) and add an
equivalent check for `"snd"`:

```c
            if (name_eq(name, "snd")) {
                *out_inode_id = i + 1;
                return 0;
            }
```

right next to the existing `if (name_eq(name, "input"))` check (same
`if (has_slash)` block -- this loop already matches any hierarchical
entry's parent directory name generically by checking each `/`-
containing entry, so both `"snd/controlC0"` and `"snd/pcmC0D0p"`
resolve through the same one added line since both share the `"snd"`
prefix).

Find `devfs_readdir`'s existing `if (dir && dir->inode_id == 5)` branch
(input directory listing) and add immediately after its closing `}`:

```c
    if (dir && dir->inode_id == 20) {   // "snd" directory
        if (index == 0) {
            memcpy_local(out->name, "controlC0", 10);
            out->type = DT_CHR; out->ino = 21;
            return 0;
        }
        if (index == 1) {
            memcpy_local(out->name, "pcmC0D0p", 9);
            out->type = DT_CHR; out->ino = 22;
            return 0;
        }
        return -ENOENT;
    }
```

- [ ] **Step 5: Add an in-kernel `[ac97] /dev/snd selftest passed` check**

This exercises the file_ops surface directly (open/hw_params/write),
proving the ioctl parsing and write path are correct without needing a
real userland client -- full end-to-end userland validation with an
actual ALSA-shaped binary is out of scope for this repo (see the
plan's closing note).

Add to `ac97.c`:

```c
void ac97_snd_selftest(void) {
    if (!ac97_present) {
        serial_write_string("[ac97] /dev/snd selftest skipped (no device)\n");
        return;
    }

    struct snd_pcm_hw_params hp;
    for (uint64_t i = 0; i < sizeof(hp); i++) { ((uint8_t *)&hp)[i] = 0; }
    hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = (1u << SNDRV_PCM_FORMAT_S16_LE);
    hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 2;
    hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = 2;
    hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 48000;
    hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = 48000;

    if (pcm_hw_params(&hp) != 0) {
        serial_write_string("[ac97] /dev/snd selftest FAILED: hw_params rejected the fixed format\n");
        return;
    }

    // Wrong format must be rejected -- proves this isn't a rubber-stamp accept.
    struct snd_pcm_hw_params bad = hp;
    bad.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = (1u << 0 /* S8 */);
    if (pcm_hw_params(&bad) == 0) {
        serial_write_string("[ac97] /dev/snd selftest FAILED: hw_params accepted an unsupported format\n");
        return;
    }

    uint16_t tone[AC97_FRAMES_PER_SEG * 2];
    for (uint32_t i = 0; i < AC97_FRAMES_PER_SEG; i++) {
        int16_t v = ((i / 100) % 2) ? 8000 : -8000;
        tone[i * 2 + 0] = (uint16_t)v; tone[i * 2 + 1] = (uint16_t)v;
    }
    int64_t rc = pcm_write(0, tone, sizeof(tone));
    if (rc != (int64_t)sizeof(tone)) {
        serial_write_string("[ac97] /dev/snd selftest FAILED: write() short or errored\n");
        return;
    }

    serial_write_string("[ac97] /dev/snd selftest passed\n");
}
```

Add `void ac97_snd_selftest(void);` to `ac97.h`, and call it from
`kernel/kernel.c` right after the existing `ac97_selftest();` call
(Task 1 Step 4).

- [ ] **Step 6: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output
```

Expected: clean build.

- [ ] **Step 7: Add `[ac97] /dev/snd selftest passed` to `REQUIRED_MARKERS` and boot**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
grep "\[ac97\]" build/serial.log
```

Expected: all four markers --
`[ac97] device found`, `[ac97] selftest passed`,
`[ac97] /dev/snd selftest passed`, and (from Task 2) the WAV file
check passing.

- [ ] **Step 8: Commit**

```bash
git add kernel/drivers/audio/snd_abi.h kernel/drivers/audio/ac97.h \
        kernel/drivers/audio/ac97.c kernel/fs/devfs.c kernel/kernel.c \
        Makefile
git commit -m "audio: /dev/snd/controlC0 + /dev/snd/pcmC0D0p ALSA-shaped surface"
```

---

### Task 4: Documentation

**Files:**
- Modify: `docs/stdlib.md`
- Modify: `docs/abi-compatibility.md` (create if it does not exist yet
  -- check first; this project's convention is that whichever
  milestone first needs it creates it)

**Interfaces:** none (docs only).

- [ ] **Step 1: Check whether `docs/abi-compatibility.md` exists**

```bash
ls docs/abi-compatibility.md 2>&1
```

- [ ] **Step 2: Add the AC97 entry to `docs/stdlib.md`**

Add a new section (matching this file's existing per-subsystem section
style -- look at how `/dev/fb0` or `/dev/input/event0` are documented
there and match that format):

```markdown
## Audio: /dev/snd/controlC0, /dev/snd/pcmC0D0p (AC97)

NeoOS's one audio backend is an AC97 controller (QEMU's `-device
AC97`, Intel 82801AA emulation). The userland surface is ALSA-shaped:
real Linux `SNDRV_*` ioctl numbers and `struct snd_pcm_hw_params`/
`snd_pcm_sw_params`/`snd_ctl_card_info` layouts (verified against
upstream Linux's `include/uapi/sound/asound.h`), reached through the
existing `open`/`write`/`ioctl` syscalls -- no new syscall numbers.

**Divergence from a real Linux ALSA driver (documented, not a shape
bug):**
- **Fixed format only.** `SNDRV_PCM_IOCTL_HW_PARAMS` accepts exactly
  one configuration -- 16-bit signed LE, stereo, 48000 Hz -- and
  returns `-EINVAL` for anything else. A real driver reports a richer
  (but still hardware-limited) capability set; NeoOS's AC97 driver
  reports the narrowest possible one. An application that queries
  capabilities before asking for this exact format will work
  correctly; one that assumes a wider format set will get `-EINVAL`
  where Linux might have accepted its request.
- **Playback only.** `/dev/snd/pcmC0D0c` (capture) does not exist --
  opening it is `-ENOENT`, the same as any other absent device, not a
  special-cased divergence.
- **No mixer tree.** `/dev/snd/controlC0` answers `PVERSION` and
  `CARD_INFO` only; every other control ioctl is `-ENOTTY`. A real
  ALSA mixer (volume controls, jack detection, etc.) does not exist.
- **QEMU-emulated hardware only.** This driver has never been run
  against real AC97 silicon -- only QEMU's `-device AC97` emulation,
  the same as every other NeoOS driver's validation story.
```

- [ ] **Step 3: Add/append the AC97 section to `docs/abi-compatibility.md`**

If the file does not exist (per Step 1), create it with a short header
matching this project's convention ("what of the Linux ABI is
implemented, what is stubbed, what diverges and why, and what a real
ported application would still hit" -- copy that framing from
`CLAUDE.md`'s own description of this file's purpose) plus the AC97
section below. If it already exists, append this as a new section:

```markdown
## Audio (AC97)

**Implemented:** `open`/`ioctl`/`write` on `/dev/snd/controlC0` and
`/dev/snd/pcmC0D0p`, with real `SNDRV_*` ioctl numbers and struct
layouts for `PVERSION`, `CARD_INFO`, `HW_PARAMS`, `SW_PARAMS`,
`PREPARE`, `START`, `DROP`.

**Stubbed/absent:** capture (`/dev/snd/pcmC0D0c`), any mixer control
beyond `PVERSION`/`CARD_INFO`, format negotiation beyond one fixed
16-bit/stereo/48kHz configuration, `mmap`-based playback (real ALSA
clients commonly prefer mmap; this driver only supports the `write()`
path).

**What a real ported application would hit:** any app that calls
`snd_pcm_hw_params_set_format_first`/similar to auto-negotiate the
"best" format rather than asking for S16_LE/2ch/48kHz directly may see
`-EINVAL` where real hardware would have offered a different format
that also happened to work. An app hard-coded to CD-quality stereo
PCM (extremely common for game sound effects, which is this driver's
actual motivating use case) works unmodified.
```

- [ ] **Step 4: Commit**

```bash
git add docs/stdlib.md docs/abi-compatibility.md
git commit -m "docs: AC97 audio driver stdlib + ABI-compatibility entries"
```

---

## Self-Review Notes (from writing this plan)

- **Spec coverage:** §3.1 (PCI probe) -> Task 1. §3.2 (register
  layout) -> Task 1 (NAM/reset/volume) + Task 2 (NABM/BDL/IRQ). §3.3
  (BDL format) -> Task 2 Step 1. §3.4 (bring-up sequence) -> Task 1
  (steps 1-4) + Task 2 (steps 2-5, DMA/IRQ). §3.5 (userland surface,
  docs) -> Task 3 (surface) + Task 4 (docs). §4 (testing) -> Task 2
  Step 6 (WAV automated check) + Task 1/2/3's selftest markers.
- **Struct/ioctl exactness:** every struct and ioctl number in Task 3
  Step 1 was fetched from upstream Linux's actual
  `include/uapi/sound/asound.h` while writing this plan (not
  reconstructed from memory) and cross-checked with `_Static_assert`s
  in the same header plus a standalone compile check (Task 3 Step 2)
  -- this is the plan's strongest correctness guarantee, since a wrong
  byte offset here would silently defeat the entire ABI-fidelity goal.
- **Type consistency:** `struct ac97_bdl_entry` (Task 2 Step 1),
  `seg_virt`/`seg_phys`/`next_write_seg`/`space_waitq`/`ac97_lock`
  (Task 2 Step 2) are referenced with identical names/types in Task 3's
  `pcm_write`/`ac97_snd_selftest` -- all in the same translation unit
  (`ac97.c`), so no cross-file signature drift is possible.
- **No placeholders:** every step has real, complete code -- register
  offsets and bit values come from the spec (itself checked against
  the AC97 standard's well-documented, stable register model); ioctl
  numbers/struct layouts come from a live fetch of upstream Linux
  headers, not invented.
- **Known gap, explicitly out of scope:** Task 3 Step 5's selftest
  proves the file_ops surface works when called in-kernel; it does
  not prove a real statically-linked ALSA-shaped userland binary can
  drive this surface end-to-end through actual syscalls. That
  validation belongs either to the Doom port (once it exists, as the
  first real userland consumer) or to `neoos-kernel-tests-common` (a
  separate repo per the org-restructuring split, not cloned in this
  workspace) -- not invented here as a fake in-repo test.
