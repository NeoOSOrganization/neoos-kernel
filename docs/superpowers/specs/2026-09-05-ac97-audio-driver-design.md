# AC97 Audio Driver + ALSA-Shaped Userland Surface — Design

**Date:** 2026-09-05
**Status:** approved, ready for implementation
**Trigger:** the same need as
`docs/superpowers/specs/2026-09-05-intel-hda-audio-driver-design.md`
(Doom needs a sound path; NeoOS has no audio driver) -- AC97 is a much
smaller, simpler controller than Intel HDA (no CORB/RIRB verb
protocol, no codec node-graph discovery: two flat register blocks and
a plain DMA descriptor ring), so it ships FIRST to prove the userland
surface end-to-end. The HDA spec stays parked as a follow-up milestone
once this lands -- not superseded, not abandoned.

This spec reuses, unchanged, every userland-surface decision the HDA
spec already made (§3.4/3.5/4 there): ALSA-shaped `/dev/snd/*` nodes,
real Linux `SNDRV_*` ioctl numbers and struct layouts, one fixed
16-bit/stereo/48kHz format, playback only, QEMU-emulated hardware only
(no real-hardware claim), and a WAV-file-backed automated test. Only
the hardware bring-up section differs.

## 1. Problem

Same as the HDA spec's §1: no audio device of any kind exists in
NeoOS. QEMU's `-device AC97` emulates Intel's ICH 82801AA AC'97
controller, PCI vendor:device `8086:2415`.

## 2. Scope

**In scope:**
- PCI probe + AC97 controller bring-up: cold reset, codec-ready poll,
  NAM (Native Audio Mixer) volume/unmute setup, NABM (Native Audio Bus
  Master) PCM-OUT DMA engine setup with a Buffer Descriptor List (BDL),
  one IOAPIC-routed completion interrupt.
- The same fixed playback format as the HDA spec: 16-bit signed LE,
  stereo, 48000 Hz.
- The same `/dev/snd/controlC0` + `/dev/snd/pcmC0D0p` ALSA-shaped
  surface, same ioctl set, same `docs/stdlib.md` documentation
  approach -- see the HDA spec §3.4/3.5 for the exact structs/ioctls;
  this driver is a second implementation underneath the identical
  userland contract, not a different one.
- The same QEMU-`audiodev`-backed automated WAV test (HDA spec §4).

**Out of scope (deliberately, YAGNI):**
- Capture, real hardware, format negotiation/mixing -- identical
  reasoning to HDA spec §2.
- Any change to the userland surface's shape. If AC97 and HDA both
  exist later, they are two `fb_device`-style interchangeable backends
  behind the SAME `/dev/snd/*` nodes, selected by whichever probes
  successfully -- this spec does not design that selection layer yet
  (today there is exactly one audio driver; the abstraction point only
  matters once there are two, matching this project's existing
  `fb_device` precedent of not building an abstraction before a second
  real implementation exists).

## 3. Decisions

### 3.1 PCI probe

```c
// kernel/drivers/audio/ac97.c
#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415   // QEMU's -device AC97 (Intel 82801AA ICH AC'97)
```

Same shape as `virtio_net_init()`/the HDA spec's §3.1: `pci_find_id`;
absence is `"[ac97] no device (not a FAILED: a machine without one is
a valid machine)\n"`.

### 3.2 Register layout

Two I/O-space BARs (unlike HDA's single MMIO BAR):

- **BAR0 -- NAM (Native Audio Mixer)**, the codec's own registers,
  16-bit each, directly addressable at `BAR0 + offset` (no index/data
  indirection):
  - `0x00` Reset (any write resets the codec)
  - `0x02` Master Volume (bit15 mute; 0x0000 = loudest)
  - `0x18` PCM Out Volume (same encoding)
  - `0x2C` Extended Audio ID (read to confirm the codec responds)

- **BAR1 -- NABM (Native Audio Bus Master)**, the DMA engines and
  global control. Three 16-byte DMA engine blocks (PCM IN at `0x00`,
  **PCM OUT at `0x10`** -- the only one this driver uses, MIC IN at
  `0x20`), each block laid out identically:

  | Offset (within block) | Width | Register | Meaning |
  |---|---|---|---|
  | `0x00` | 4 | BDBAR | Buffer Descriptor Base Address (phys addr of the BDL) |
  | `0x04` | 1 | CIV | Current Index Value (read-only, which BDL entry is active) |
  | `0x05` | 1 | LVI | Last Valid Index (software sets: how many BDL entries are valid) |
  | `0x06` | 2 | SR | Status: bit0 DCH (DMA halted), bit2 LVBCI, bit3 BCIS (buffer-complete, write-1-to-clear), bit4 FIFOE |
  | `0x08` | 2 | PICB | Position In Current Buffer (read-only, counts down) |
  | `0x0A` | 1 | PIV | Prefetched Index Value |
  | `0x0B` | 1 | CR | Control: bit0 RPBM (Run/Pause Bus Master), bit1 RR (Reset Registers), bit2 LVBIE, bit4 IOCE (Interrupt On Completion Enable) |

  Global registers, fixed offsets within BAR1 regardless of engine:
  - `0x2C` GLOB_CNT (4 bytes) -- bit1 clear = codec held in cold
    reset, bit1 set = codec released from reset
  - `0x30` GLOB_STA (4 bytes) -- bit8 PCR (Primary Codec Ready);
    poll this after releasing reset
  - `0x34` CAS (1 byte) -- Codec Access Semaphore

### 3.3 BDL entry format

8 bytes per entry, matching the fixed 4-entry, 4096-frame-per-segment
layout the HDA spec's §3.3 already committed to (same buffer sizing,
different entry encoding):

```c
struct ac97_bdl_entry {
    uint32_t buf_phys;   // physical address of this segment
    uint16_t length;     // SAMPLES (16-bit words), not bytes or frames:
                          // stereo 16-bit = 2 samples/frame, so
                          // length = frames_per_segment * 2 = 8192
    uint16_t flags;      // bit15 (0x8000) = IOC, interrupt when this
                          // entry completes; bit14 (0x4000) = BUP,
                          // repeat last sample on underrun
};
_Static_assert(sizeof(struct ac97_bdl_entry) == 8, "AC97 BDL ABI");
```

### 3.4 Bring-up sequence

1. Cold reset: read `GLOB_CNT`, set bit1 (release codec from reset),
   write back.
2. Poll `GLOB_STA` bit8 (PCR) until set (codec ready) or a bounded
   retry count is exhausted -- treat exhaustion as `[ac97] selftest
   FAILED: codec never became ready`, not an infinite loop.
3. Confirm the codec responds: read NAM `0x2C` (Extended Audio ID) and
   log it.
4. Unmute + set full volume: write `0x0000` to NAM `0x02` (Master
   Volume) and NAM `0x18` (PCM Out Volume).
5. Allocate the 4-entry BDL + its 4x4096-frame buffer pool (identical
   sizing/allocation style to the HDA spec's §3.3 -- one contiguous
   DMA block, never freed).
6. Program the PCM-OUT engine (NABM `0x10` block): `BDBAR` = BDL
   physical address, `LVI` = 3 (4 entries, zero-indexed last-valid),
   `CR` = `RPBM | IOCE` (start the bus master with interrupt-on-
   completion).
7. Route the PCI interrupt line exactly like the HDA spec's §3.2 step
   6 (same `ioapic_set_redirection` pattern as `virtio_net`, a new
   `VECTOR_AC97` next to `VECTOR_HDA`/`VECTOR_VIRTIO_NET` in
   `kernel/arch/isr.c`'s vector if-chain).
8. IRQ handler: read PCM-OUT `SR`, check `BCIS` (bit3); if set, write
   it back to clear (write-1-to-clear), advance the write cursor,
   refill the just-completed segment, wake any writer blocked on
   buffer space (same `waitq` pattern as the HDA spec's §3.3 /
   `pipe.c`'s ring buffer).

### 3.5 Userland surface, docs

Unchanged from the HDA spec's §3.4/3.5 -- same device nodes, same
ioctl numbers/struct layouts (`SNDRV_PCM_IOCTL_HW_PARAMS` etc., byte-
verified against the real upstream Linux
`include/uapi/sound/asound.h`), same fixed-format constraint reasoning,
same `docs/stdlib.md`/`docs/abi-compatibility.md` update approach.
`write()` on `/dev/snd/pcmC0D0p` copies PCM frames into the next free
BDL segment, blocking on the `waitq` above if none is free.

## 4. Testing

- `[ac97] device found` / `[ac97] no device` -- probe result.
- `[ac97] selftest passed` -- cold reset + codec-ready poll + BDL/DMA
  engine programming succeeded (structural check, not an audible one
  -- same reasoning as `fb_device_selftest`/the HDA spec's §4).
- The same QEMU `-audiodev wav,path=...` automated WAV-file check as
  the HDA spec's §4: a known test tone written to `/dev/snd/pcmC0D0p`,
  the gauntlet asserts the resulting `.wav` file's byte count is within
  tolerance of the expected `duration * rate * channels *
  bytes_per_sample`.

## 5. Migration ordering

1. PCI probe + cold reset + codec-ready poll (`[ac97] selftest passed`
   reachable with no DMA yet).
2. NAM volume/unmute setup.
3. BDL allocation + NABM PCM-OUT engine programming + IRQ wiring.
4. `/dev/snd/controlC0` + `/dev/snd/pcmC0D0p` devfs nodes and ioctl
   surface (this task can mostly reuse code shape from the parked HDA
   spec's equivalent task, since the userland contract is identical).
5. `docs/stdlib.md` / `docs/abi-compatibility.md` updates.
6. The WAV-file automated test wired into the gauntlet.

This lands in `neoos-kernel` directly, same as every other in-tree
driver.
