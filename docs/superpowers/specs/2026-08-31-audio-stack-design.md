# Audio stack — SB16 / AC97 / Intel HDA — design

**Date:** 2026-08-31
**Status:** design (solo brainstorm — see roadmap). Largest of the
requested milestones; has real prerequisites.
**Context:** `kernel/fs/devfs.c` (static device table, no per-open
state, no ioctl/poll), `kernel/fs/file.h` (`struct file_ops` has no
`ioctl`/`poll`), **no PCI subsystem exists**, `kernel/dev/` (ISA
drivers: serial, pit, keyboard, ata).

## Problem

NeoOS has no audio and no device class that resembles one: no PCM
stream abstraction, no mixer, no DMA-ring plumbing, no way for a
userland program to open a sound device and `write()` samples. The
request is for three codecs spanning three eras of PC audio: SB16
(ISA DMA), AC97 (PCI bus-master), Intel HDA (PCI, codec graph).

## Prerequisites (not audio, but audio is blocked on them)

| Need | Why | Size | Notes |
|---|---|---|---|
| `file_ops` gains `ioctl` + `poll` | `/dev/dsp` is ioctl-configured and `poll`-able | small | This is Phase 14 Task 8's deliverable — do that, or bring it here. |
| devfs dynamic device registration + per-open private state | a sound device needs open/close ctors and per-fd stream state | small | Also Phase 14 Task 8 (`struct devfs_dev` + open ctor). |
| A PCI subsystem: enumerate bus 0, config-space read/write, BAR decode, MSI/legacy IRQ routing | AC97 and HDA are PCI devices | **medium, its own milestone** | SB16 does NOT need it. |

**Recommended split:** land the two small devfs/file_ops items (as
part of resuming Phase 14), then the sound core + SB16 (ISA only), then
a dedicated PCI milestone, then AC97 and HDA.

## Goals

1. **A sound core** (`kernel/dev/snd/`): a `struct snd_card` /
   `struct snd_pcm` device class, a period-based ring buffer, and an
   OSS-style `/dev/dsp` + `/dev/mixer` interface.
2. **SB16 driver** — playback via ISA DMA, 44.1 kHz/16-bit stereo.
3. **AC97 driver** — playback via PCI bus-master BDL, 48 kHz fixed.
4. **Intel HDA driver** — playback via a stream descriptor + BDL, codec
   graph walk to a line-out DAC.
5. Capture (recording) for at least one driver (HDA, via
   `qemu -device hda-duplex`), so `/dev/dsp` read works.

## Non-goals

- ALSA. NeoOS implements the **OSS** (`/dev/dsp`) ABI: `write()`-driven,
  a handful of ioctls, no card/device/subdevice hierarchy, no
  mmap-mode by default. Rationale: OSS's ABI is ~10 ioctls and fully
  implementable from scratch; ALSA's is hundreds of ioctls plus a
  control/PCM/timer/sequencer split and is only ever *partially*
  implemented. Recorded divergence: Linux is ALSA-native with OSS via
  emulation; NeoOS is OSS-native. Programs using `libasound` will not
  work; programs writing `/dev/dsp` (aplay's oss mode, mpg123 `-o oss`,
  SDL's `dsp` driver, sox) will.
- Sample-rate conversion / mixing multiple streams in the kernel. One
  writer at a time per PCM device; the requested rate is passed to the
  codec and rejected (`-EINVAL` on `SNDCTL_DSP_SPEED`) if the hardware
  cannot do it. A userland `dmix`-equivalent is out of scope.
- MIDI, the OSS sequencer, `/dev/sndstat` beyond a stub.
- Power management / codec suspend.
- SB16 auto-init DMA for capture (playback only for SB16; capture only
  where the modern codec makes it cheap — HDA).
- MSI-X, multiple HDA streams, surround (>2 channels).

## Design

### 1. Sound core (`kernel/dev/snd/core.c`, `oss.c`, `mixer.c`)

**PCM device model:**

```c
struct snd_pcm_ops {
    int  (*open)(struct snd_pcm *, int capture);
    void (*close)(struct snd_pcm *);
    int  (*set_params)(struct snd_pcm *, uint32_t rate, uint32_t fmt, uint32_t ch);
    int  (*prepare)(struct snd_pcm *);          // program the DMA ring
    int  (*trigger)(struct snd_pcm *, int start);// start/stop transport
    uint64_t (*pointer)(struct snd_pcm *);       // hw byte position in the ring
};

struct snd_pcm {
    const struct snd_pcm_ops *ops;
    void   *hw;                 // driver private
    uint8_t *buf;               // the DMA ring, driver-allocated (low mem for ISA)
    uint32_t buf_bytes;         // total ring size
    uint32_t period_bytes;      // IRQ granularity
    uint32_t sw_ptr, hw_ptr;    // producer / consumer positions
    struct waitq  writers;      // block when the ring is full
    struct spinlock lock;       // LOCK_RANK_DRIVER
    int running, capture;
    uint32_t rate, fmt, channels;
};
```

- The driver allocates `buf` (SB16: a fixed page < 16 MB, 64 KB-aligned
  for ISA DMA; AC97/HDA: any physical pages, described by a BDL).
- `snd_pcm_write(pcm, ubuf, len)`: copy user samples into the ring at
  `sw_ptr` up to `hw_ptr` (mod `buf_bytes`); if full, `waitq_sleep
  (&pcm->writers, &pcm->lock)`; on first data, `prepare` + `trigger
  (start)`.
- **The period IRQ** (SB16 DMA-done / AC97 BDL-entry / HDA
  buffer-completion) advances `hw_ptr` by `period_bytes` and
  `waitq_wake_all(&pcm->writers)`. If the ring underruns (`sw_ptr ==
  hw_ptr` while running), the driver plays silence for that period and
  a counter increments (reported via `SNDCTL_DSP_GETODELAY` /
  logged) — no xrun stop, matching OSS's forgiving model.
- `poll`: writable when the ring has room.

**`/dev/dsp` (`oss.c`) — the ioctl subset:**

| ioctl | behaviour |
|---|---|
| `SNDCTL_DSP_SPEED` | set/return sample rate; `-EINVAL` if hw can't |
| `SNDCTL_DSP_SETFMT` | `AFMT_S16_LE` supported; `AFMT_U8` for SB16; query with `AFMT_QUERY` |
| `SNDCTL_DSP_CHANNELS` | 1 or 2 |
| `SNDCTL_DSP_GETOSPACE` | `audio_buf_info` = free bytes / fragments |
| `SNDCTL_DSP_GETOPTR` | `count_info` = bytes played |
| `SNDCTL_DSP_SETFRAGMENT` | fragment size hint -> `period_bytes` |
| `SNDCTL_DSP_SYNC` | drain: block until `hw_ptr == sw_ptr`, then stop |
| `SNDCTL_DSP_RESET` | stop + discard |
| `SNDCTL_DSP_POST` | no-op (we start on first write) |

Struct layouts (`audio_buf_info`, `count_info`) copied byte-for-byte
from Linux `<sys/soundcard.h>`. ioctl *numbers* are the Linux `_IOR/
_IOWR` encodings (they cross the ABI boundary — the shim cannot fix a
number an app compiled in).

**`/dev/mixer` (`mixer.c`):** `SOUND_MIXER_READ_VOLUME` /
`_WRITE_VOLUME` / `_READ_DEVMASK` (report PCM + MASTER). Maps 0-100 per
channel onto the codec's volume registers. Everything else returns the
current value or `-EINVAL`.

**devfs registration:** `snd_register(struct snd_pcm *)` adds
`/dev/dsp` and `/dev/mixer` via the new dynamic devfs API, with an
open ctor that allocates per-fd stream state.

### 2. SB16 (`kernel/dev/snd/sb16.c`)

- Probe: reset via port `0x226`, read version from the DSP (`0x22A`
  data, `0x22C` write, `0x22E` read-status). QEMU `-device sb16`
  presents a v4.05 DSP at base `0x220`, IRQ 5, DMA 1 (8-bit) / 5
  (16-bit).
- Playback: program the 8237 for DMA channel 5 (16-bit) at the ring
  page, auto-init mode so the transfer wraps the buffer; set sample
  rate via DSP command `0x41` (output rate, big-endian); start with
  `0xB6` (16-bit auto-init output) + block size.
- The period IRQ is the "half/full buffer" interrupt; ACK by reading
  DSP status port `0x22F` (16-bit) and the PIC.
- `set_params`: 44100 or 22050, `AFMT_S16_LE` / `AFMT_U8`, 1-2 ch.
  Other rates -> `-EINVAL`.

### 3. PCI subsystem (`kernel/dev/pci.c`) — prerequisite for F3/F4

- Config space via I/O ports `0xCF8` (address) / `0xCFC` (data).
- Enumerate bus 0, device 0-31, function 0-7: read vendor/device,
  class/subclass, header type; recurse into bridges (there is only the
  host bridge in QEMU's `pc` machine, so a flat bus-0 scan suffices
  initially — note the limitation).
- `pci_find(vendor, device)` / `pci_find_class(class, subclass)`.
- BAR decode: size a BAR by writing all-ones and reading back the
  mask; MMIO BARs get mapped into the kernel via `paging_map_into`
  (uncached — set `PAGE_CACHE_DISABLE`); I/O BARs kept as port bases.
- IRQ: read the interrupt line from config space (QEMU routes it
  through the PIIX3; the value in the register is the GSI). Wire it to
  the IOAPIC RTE like the existing ISA IRQ setup. MSI is a later
  enhancement.
- A `[pci] <bus:dev.fn> <vendor>:<device> class ...` boot log line per
  device.

### 4. AC97 (`kernel/dev/snd/ac97.c`)

- PCI class 0x04 subclass 0x01, or vendor/device `8086:2415` (QEMU
  `-device AC97`). Two BARs: NAM (mixer, I/O) and NABM (bus-master,
  I/O).
- Codec init: reset via NAM `0x00`, wait for codec-ready, set PCM
  out volume (NAM `0x18`) and master (NAM `0x02`), read the sample-rate
  support bit; QEMU's model is fixed 48 kHz — `set_params` accepts
  48000 only.
- Playback: a 32-entry **BDL** (buffer descriptor list) in low memory,
  each entry `{ buf_phys, samples, flags(IOC|BUP) }`. Point the PCM
  out box (NABM `0x10` BDBAR) at it, set LVI (`0x15`), set the control
  register (`0x1B`) run bit. IOC on every entry -> a period IRQ per
  entry that advances `hw_ptr`.
- IRQ: read NABM status (`0x16`), clear the BCIS bit, EOI.

### 5. Intel HDA (`kernel/dev/snd/hda.c`)

- PCI class 0x04 subclass 0x03, vendor/device `8086:2668` (QEMU
  `-device intel-hda`). One MMIO BAR.
- Controller reset (GCTL CRST), wait; allocate and program **CORB**
  (command output ring) and **RIRB** (response input ring) in memory,
  set their base/size/write-pointer registers, enable DMA.
- Codec enumeration: `GET_PARAMETER(VENDOR_ID)`, walk function groups
  (`SUBORDINATE_NODE_COUNT`), find the audio function group, walk its
  widgets; identify an **Audio Output (DAC)** widget and a **Pin
  Complex** with the line-out default-device, set the pin's output
  enable + EAPD, connect the DAC to the pin (connection-list select),
  set stream/channel on the DAC (`SET_CONVERTER_STREAM_CHANNEL`),
  unmute + set gain on DAC and pin.
- Stream: pick output stream descriptor 0 (SDO), program its BDL (same
  shape as AC97's), format register (48 kHz / 16-bit / 2 ch = 0x4011),
  stream number, cyclic buffer length, then set RUN.
- IRQ: global INTSTS -> stream STS, clear BCIS, EOI. `pointer()` reads
  the stream's LPIB.
- Capture (`hda-duplex`): a second stream descriptor (SDI), an Audio
  Input widget + a mic/line-in pin; `/dev/dsp` read drains it.

## Testing

- QEMU: `-device sb16 -device AC97 -device intel-hda -device hda-duplex`.
  Audio output goes nowhere audible in headless mode, so tests assert
  **the transport runs**, not sound:
  - `snd_selftest` per driver: open the PCM, `set_params`, write two
    buffers of a known ramp, poll `pointer()` and assert it advances
    past one period within a timeout, assert the period IRQ fired
    (a counter), `SNDCTL_DSP_SYNC` drains. `[snd:sb16] selftest
    passed` etc., in `REQUIRED_MARKERS`.
  - HDA capture: write silence, read back, assert non-blocking-once-
    running and that `read` returns `period_bytes` chunks.
- `userland/dsptest.c`: `open("/dev/dsp")`, `ioctl` the format/rate/
  channels, `write()` 1 second of a sine table, `SNDCTL_DSP_SYNC`,
  check every syscall's return; `poll` for writable. Assert
  `SNDCTL_DSP_SPEED` of an impossible rate returns `-EINVAL`.
  `/dev/mixer` volume round-trip.
- Gauntlet ×3 after each driver — the period IRQ + `waitq_wake` +
  writer-blocking is the concurrency surface.

## ABI / stdlib impact

- `docs/stdlib.md`: new `/dev/dsp`, `/dev/mixer`; the supported OSS
  ioctl set and their exact semantics; the "OSS-native, not ALSA"
  divergence; per-device single-writer; supported rates/formats per
  codec.
- `docs/abi-compatibility.md`: OSS `soundcard.h` struct layouts and
  ioctl numbers match Linux; ALSA is absent; `poll` on `/dev/dsp`
  matches Linux OSS behaviour.
- A `lib/` note or musl shim: `open`/`ioctl`/`write` already work; the
  `SNDCTL_*` / `SOUND_MIXER_*` constants ship in a NeoOS
  `<sys/soundcard.h>` mirroring Linux's values.

## Risks

1. **Prerequisite creep.** Three of the four sub-milestones need
   infrastructure that does not exist (PCI) or is half-built
   (devfs/ioctl). Do not start F3/F4 before the PCI milestone lands
   green. F1+F2 are safe once the devfs/ioctl items (Phase 14 Task 8)
   land.
2. **DMA coherency on QEMU vs metal.** QEMU does not need cache
   flushes; a real machine needs the MMIO BARs mapped uncached and
   memory barriers around ring-pointer updates. Build it right even
   though QEMU will not punish getting it wrong.
3. **ISA DMA 64 KB boundary / 16 MB limit** for SB16 — same fixed
   low-bounce-page discipline as the FDC milestone. Share the low-mem
   allocator if the FDC milestone lands first.
4. **HDA codec graph variance.** Real codecs differ wildly; QEMU's is
   simple and fixed. The walk should be defensive (bail with a clear
   log if the expected DAC/pin is not found) rather than assume
   QEMU's exact topology — but the *tested* path is QEMU's.
5. **The period IRQ rate.** A small `period_bytes` at 48 kHz stereo
   16-bit is an IRQ every few ms per active stream. Fine for one
   stream; keep `period_bytes` >= ~10 ms of audio and cap active
   streams at 1.
6. **`poll`/`ioctl` on `file_ops`** must be added carefully — every
   existing `struct file_ops` initializer (`vnode_file_ops`,
   `pipe_ops`, `sock_ops`, tty) gains two NULL members; a NULL `ioctl`
   must return `-ENOTTY` and NULL `poll` a sane default from the
   dispatch layer.

## Plan sketch

Each of these is its own `writing-plans` document:

- **P0 (fold into Phase 14 resumption):** `file_ops` + `ioctl`/`poll`;
  devfs dynamic registration + per-open state.
- **P1 audio core + SB16:** sound core, `/dev/dsp` + `/dev/mixer`,
  SB16 driver, `dsptest`, selftests. Gauntlet ×3.
- **P2 PCI subsystem:** `pci.c` enumerate/config/BAR/IRQ, boot log,
  a selftest that finds the QEMU devices. Gauntlet.
- **P3 AC97:** driver on top of P1's core + P2's PCI. Gauntlet ×3.
- **P4 Intel HDA:** driver + codec walk + capture. Gauntlet ×3.
- **P5 docs:** stdlib.md, abi-compatibility.md, `<sys/soundcard.h>`.
