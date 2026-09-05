# Intel HD Audio Driver + ALSA-Shaped Userland Surface — Design

**Date:** 2026-09-05
**Status:** approved, ready for implementation
**Trigger:** the Doom port (`neoos-doom`, planned separately) needs a
sound path; NeoOS has no audio driver at all. This spec covers the
audio subsystem itself, independent of Doom -- Doom is simply the
first real consumer.

## 1. Problem

NeoOS has PCI enumeration (`kernel/drivers/pci/pci.c`), a virtio-net
driver, and an AHCI/ATA block driver, all following the same shape:
probe by vendor/device ID, program the device directly, route one
IOAPIC line, expose a userland surface through devfs. There is no audio
device of any kind, and no `/dev/snd/*` (or any other audio) node.

QEMU's `-device intel-hda` (with a `hda-output` or `hda-duplex` codec
child device) emulates a real Intel ICH6 HD Audio controller,
vendor:device `8086:2668` -- the same class of "real hardware, QEMU
emulated, validated headlessly" target as every other NeoOS driver.

## 2. Scope

**In scope:**
- PCI probe + HDA controller bring-up: CORB/RIRB command rings, codec
  enumeration over those rings, one output stream descriptor with a
  Buffer Descriptor List (BDL) for DMA, one IOAPIC-routed completion
  interrupt.
- A fixed-format playback path: 16-bit signed LE, stereo, 48000 Hz --
  QEMU's HDA emulation's native/default format. No resampling, no
  format negotiation beyond accept-or-reject.
- `/dev/snd/controlC0` and `/dev/snd/pcmC0D0p`, using real Linux
  `SNDRV_*` ioctl numbers and `struct snd_pcm_hw_params`/`sw_params`
  layouts, so a real (unmodified) statically-linked ALSA-shaped client
  binary's ioctl traffic is byte-for-byte what a real Linux kernel
  would accept -- this project's stated Linux-ABI-shape goal, applied
  to audio.
- `docs/stdlib.md` + `docs/abi-compatibility.md` updates recording the
  fixed-format constraint as a documented hardware-capability
  limitation (real hardware also reports a restricted format set via
  hw constraints -- this is not a shape divergence).
- A QEMU-audiodev-backed automated test: `-audiodev wav,path=...`
  captures the controller's actual DMA output to a `.wav` file; the
  gauntlet asserts that file is non-empty and roughly the expected byte
  count for a known test tone. This is a REAL automated correctness
  check, not a human listening test.

**Out of scope (deliberately, YAGNI):**
- Capture (microphone input). Nothing in scope needs it; one output
  stream only.
- Real hardware. Only QEMU's emulated ICH6 HDA is targeted or
  validated -- there is no physical machine to test real silicon
  against in this environment, and claiming real-hardware support
  without ever running it there would be a false claim, not a design
  choice. The driver is written cleanly enough that real hardware
  would plausibly work, but that claim stays unverified and
  unadvertised.
- Format negotiation / resampling / mixing. One fixed format, one
  stream, one client at a time.
- A full ALSA control surface (mixer element tree, jack detection,
  hwdep). `controlC0` exists because ALSA-shaped clients expect the
  device node to exist and open cleanly; it does the minimum to not
  fail that open, nothing more.
- Doom's actual sound wiring. That is `neoos-doom`'s job, on top of
  this driver, once this lands.

## 3. Decisions

### 3.1 PCI probe

```c
// kernel/drivers/audio/hda.c
#define HDA_VENDOR 0x8086
#define HDA_DEVICE 0x2668   // QEMU's -device intel-hda (ICH6 HDA)
```

Same shape as `virtio_net_init()`: `pci_find_id(HDA_VENDOR,
HDA_DEVICE)`; absence is `"[hda] no device (not a FAILED: a machine
without one is a valid machine)\n"`, not an error.

### 3.2 Controller bring-up

BAR0 is the HDA MMIO register set (32-bit, memory-mapped, mapped the
same way `vesafb`/`virtio` map their BARs -- physmap when reachable,
else an explicit mapping). Bring-up sequence, each step a real,
checkable register write/readback:

1. Reset: clear then set `GCTL.CRST`, poll until the controller
   acknowledges (spec-mandated reset handshake).
2. Size and allocate the CORB (Command Output Ring Buffer) and RIRB
   (Response Input Ring Buffer) -- each a small DMA'd ring the
   controller reads commands from / writes responses to. Program their
   base addresses (`CORBLBASE`/`CORBUBASE`, `RIRBLBASE`/`RIRBUBASE`),
   sizes, and start both engines (`CORBCTL.RUN`, `RIRBCTL.RUN`).
3. Codec discovery: `STATESTS` reports which of the 15 possible codec
   addresses responded to the reset. For each set bit, send a
   `Get Parameter` verb (Vendor/Device ID, node count) over the CORB
   and read the RIRB response -- this is how the codec's node graph
   (AFG -> pin complex -> DAC) gets walked.
4. Configure the discovered output DAC + pin complex for the fixed
   16-bit/stereo/48kHz format (a single `Set Converter Format` verb
   with the format's HDA-encoded value) and unmute/enable the pin.
5. Program one output stream descriptor (`SDnCTL`/`SDnFMT`/`SDnBDPL`/
   `SDnBDPU`/`SDnCBL`/`SDnLVI`) with a BDL pointing at a DMA'd audio
   buffer, matching the same fixed format.
6. Route the controller's PCI interrupt line exactly like
   `virtio_net`'s in `kernel/kernel.c`: read `pci->irq_line`, subtract
   `acpi.ioapic_gsi_base` for the IOAPIC pin, `ioapic_set_redirection`
   with a new `VECTOR_HDA`, active-low + level-triggered (same as
   every other PCI line in this kernel). Dispatch added to
   `kernel/arch/isr.c`'s existing vector if-chain (`VECTOR_HDA` next to
   `VECTOR_VIRTIO_NET`), not a general registration table -- matches
   this file's existing style.

### 3.3 Buffer model

One BDL of 4 fixed-size segments, each 4096 frames (16384 bytes at
16-bit stereo -- ~85ms per segment, ~341ms total buffer), allocated as
one contiguous DMA block, never freed (mirrors `virtio_net`'s "one
contiguous block, never freed" pool style, not virtio's descriptor-ring
machinery -- HDA has its own, simpler, hardware-defined ring shape). A
stream-completion interrupt fires when the controller finishes a
segment; the handler advances a write cursor and wakes any writer
blocked on buffer space (a `waitq`, same pattern `pipe.c` uses for its
ring buffer).

### 3.4 `/dev/snd/*` device nodes

```
/dev/snd/controlC0   -- opens cleanly; SNDRV_CTL_IOCTL_PVERSION and
                         SNDRV_CTL_IOCTL_CARD_INFO answered with real
                         values, everything else -ENOTTY (no mixer
                         tree to expose yet -- documented limitation).
/dev/snd/pcmC0D0p    -- playback PCM device.
```

`pcmC0D0p` ioctls implemented, matching real `SNDRV_PCM_IOCTL_*`
numbers and struct layouts:
- `HW_PARAMS` / `HW_PARAMS_OLD`: validates the request against the one
  fixed format (`SNDRV_PCM_FORMAT_S16_LE`, 2 channels, 48000 Hz);
  anything else returns `-EINVAL`, exactly as real hardware reporting
  a narrow capability mask would.
- `SW_PARAMS`: accepted with the driver's fixed buffer/period sizes
  (no negotiation -- one buffer size, one period size, both fixed by
  the BDL layout in §3.3).
- `PREPARE`: resets the DMA write cursor.
- `START` / `DROP`: enables/disables the stream descriptor's `RUN` bit.
- `write()` on the fd: the actual audio path. Copies PCM frames into
  the next free BDL segment (blocking, via the `waitq` in §3.3, if
  none is free -- exactly the backpressure shape `pipe_write` already
  uses in this kernel).

New devfs nodes only -- no new syscall numbers. `open`/`write`/`ioctl`
already exist; this is a translation-only extension exactly like every
other devfs node, per this project's adaptor philosophy.

### 3.5 `docs/stdlib.md` / `docs/abi-compatibility.md`

Record: which `SNDRV_PCM_IOCTL_*`/`SNDRV_CTL_IOCTL_*` numbers are
implemented vs. `-ENOTTY`; the fixed-format constraint and why (one
DAC, one format, no resampling -- a real hardware limitation shape, not
an ABI divergence); that capture is entirely absent (`/dev/snd/pcmC0D0c`
does not exist -- opening it is `-ENOENT`, matching "the device isn't
there" rather than inventing a fake divergence for a node that was
never promised).

## 4. Testing

- `[hda] device found` / `[hda] no device` -- probe result, mirroring
  `virtio-net`'s existing style.
- `[hda] selftest passed` -- controller reset + CORB/RIRB bring-up +
  codec discovery succeeded, using the same "structural, not audible"
  self-check style `fb_device_selftest` uses for video: verifies
  register state and the codec responded, not that sound is audible.
- **The real automated check**: the gauntlet boots with
  `-audiodev wav,id=hda0,path=build/hda-test.wav` wired to the HDA
  codec, a selftest program writes a known short test tone (e.g. a
  fixed-frequency sine, a fixed duration) to `/dev/snd/pcmC0D0p`, and
  `make test`/gauntlet asserts the resulting WAV file exists and its
  byte count is within tolerance of `duration * sample_rate *
  channels * bytes_per_sample` -- an actual pass/fail on real captured
  DMA output, not a human listening test. This is the audio
  equivalent of `make run`'s human visual spot-check, except this one
  is fully automatable and should be, since QEMU's `wav` audiodev
  backend makes it possible.

## 5. Migration ordering

1. PCI probe + controller reset + CORB/RIRB bring-up (`[hda] selftest
   passed` reachable with no codec output yet).
2. Codec discovery + output path configuration.
3. Stream descriptor + BDL + IRQ wiring.
4. `/dev/snd/controlC0` + `/dev/snd/pcmC0D0p` devfs nodes and their
   ioctl surface.
5. `docs/stdlib.md` / `docs/abi-compatibility.md` updates.
6. The WAV-file automated test wired into the gauntlet.

This lands in `neoos-kernel` directly (matches the org-restructuring
state: boot-critical kernel drivers live in `neoos-kernel`, not a
separate repo -- there is no "neoos-audio" repo, the same as there is
no separate repo for `ata.c` or `virtio_net.c`).
