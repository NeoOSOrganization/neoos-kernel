# Audio P1 — Sound Core + SB16 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** An OSS-style `/dev/dsp` + `/dev/mixer` sound core and an SB16
driver, so a userland program can `open("/dev/dsp")`, set format/rate/
channels via ioctl, and `write()` PCM samples that the hardware plays.

**Architecture:** A period-based ring buffer (`struct snd_pcm`) with a
driver `ops` table. `/dev/dsp` maps the OSS ioctl subset onto it;
`write()` fills the ring and blocks when full; the driver's period IRQ
advances the consumer pointer and wakes writers. SB16 drives the ring
via ISA DMA channel 5 (16-bit), auto-init mode.

**Tech Stack:** C (freestanding), NASM, x86-64, GNU Make, headless
QEMU (`-device sb16`), the parallel gauntlet (CONC=3). Audio is
inaudible headless — tests assert **the transport runs**, not sound.

**Spec:** `docs/superpowers/specs/2026-08-31-audio-stack-design.md`
(§1 sound core, §2 SB16).

**Prerequisites (P0 — do these first, as part of resuming Phase 14):**
- `struct file_ops` gains `ioctl` and `poll` (Phase 14 Task 8). Every
  existing initializer (`vnode_file_ops`, pipe, socket, tty) gets two
  NULL members; the dispatch layer returns `-ENOTTY` for a NULL
  `ioctl` and a sensible default for a NULL `poll`.
- devfs dynamic device registration with a per-open ctor (Phase 14
  Task 8: `struct devfs_dev` + open ctor). This plan's Task 4 assumes
  it exists.
- The ISA-DMA helper from the FDC milestone (`isadma_setup` + a low
  bounce/ring page). If FDC has not landed, this plan's Task 3 brings a
  minimal version.

## Global Constraints

- **No host unit tests.** Each task's test is a kernel selftest or
  `userland/dsptest.c`, verified by `make test` + the parallel gauntlet
  (`PGAUNTLET PASSED: 15/15`; ×3 after the SB16 IRQ path).
- **Lock ranks:** one new lock, `snd_pcm.lock` per stream, rank
  `LOCK_RANK_DRIVER`. Held across `waitq_sleep` for the writer-blocks-
  when-full path (hand it to `waitq_sleep` as `release`, like the mutex
  pattern) — so it must rank below WAITQ (it does: DRIVER=8 < WAITQ=14).
- **The ABI is not ours.** `struct audio_buf_info` / `struct
  count_info` layouts and the `SNDCTL_*` / `SOUND_MIXER_*` ioctl
  numbers match Linux `<sys/soundcard.h>` byte-for-byte. Recorded:
  NeoOS is OSS-native, not ALSA.
- **ISA DMA:** the SB16 ring is one fixed page < 16 MiB, 64 KiB-safe.
- **Work on `main`, one commit per task**, standard trailer.
- **No `make` while the gauntlet runs.**

## File Structure

| File | Change |
|---|---|
| `kernel/dev/snd/core.c` / `snd.h` (new) | `struct snd_pcm`, ring, `snd_pcm_write`, period-IRQ callback |
| `kernel/dev/snd/oss.c` (new) | `/dev/dsp` `file_ops` (read/write/ioctl/poll) |
| `kernel/dev/snd/mixer.c` (new) | `/dev/mixer` `file_ops` |
| `kernel/dev/snd/sb16.c` (new) | the driver + `snd_pcm_ops` impl |
| `lib/include/sys/soundcard.h` (new) | OSS constants + structs, Linux values |
| `kernel/kernel.c` | `sb16_init()` in device init |
| `Makefile` | `KERNEL_DIRS += kernel/dev/snd`; `-device sb16`; `[snd*]` markers; build `dsptest` |
| `userland/dsptest.c` (new) | the userland exercise |
| `docs/stdlib.md`, `docs/abi-compatibility.md` | the OSS ABI + divergence |

---

## Task 1: the ring buffer core (no hardware)

**Files:** `kernel/dev/snd/core.c`/`snd.h` (new), `Makefile`.

**Interfaces — Produces:**
```c
struct snd_pcm_ops {
    int  (*set_params)(struct snd_pcm *, uint32_t rate, uint32_t fmt, uint32_t ch);
    int  (*prepare)(struct snd_pcm *);
    int  (*trigger)(struct snd_pcm *, int start);
    uint64_t (*pointer)(struct snd_pcm *);
    void (*close)(struct snd_pcm *);
};
struct snd_pcm { /* ops, hw, buf, buf_bytes, period_bytes, sw_ptr,
                   hw_ptr, writers (waitq), lock, running, capture,
                   rate, fmt, channels, xruns */ };

int64_t snd_pcm_write(struct snd_pcm *, const void *ubuf, uint64_t len);
int64_t snd_pcm_read(struct snd_pcm *, void *ubuf, uint64_t len);
void    snd_pcm_period_elapsed(struct snd_pcm *);  // called from the driver IRQ
uint32_t snd_pcm_avail(struct snd_pcm *);          // free bytes for GETOSPACE
int     snd_pcm_drain(struct snd_pcm *);           // for SNDCTL_DSP_SYNC
```

- [ ] **Step 1: failing test** — `snd_core_selftest`: make a fake
  `snd_pcm` with a stub `ops` (prepare/trigger/pointer are no-ops;
  `pointer` returns a value a driver-less test advances manually via
  `snd_pcm_period_elapsed`). Write 2×`period_bytes` of a ramp; assert
  `snd_pcm_avail` dropped; call `snd_pcm_period_elapsed` twice; assert
  `avail` recovered and a blocked writer would wake. `[snd] core
  selftest passed` in `REQUIRED_MARKERS`. Empty `core.c` → FAIL.

- [ ] **Step 2: implement** the ring: `snd_pcm_write` copies user
  bytes into `buf[sw_ptr % buf_bytes]` up to `hw_ptr`; on full,
  `waitq_sleep(&writers, &lock)`; on first byte, `ops->prepare()` +
  `ops->trigger(1)`. `snd_pcm_period_elapsed`: `hw_ptr += period_bytes`
  under `lock`; if `sw_ptr == hw_ptr` while running, `xruns++` (play
  silence — the ring already holds zeros past `sw_ptr` if we zero on
  drain); `waitq_wake_all(&writers)`. `snd_pcm_drain`: block until
  `hw_ptr >= sw_ptr`, then `ops->trigger(0)`.

- [ ] **Step 3** — `[snd] core selftest passed`; gauntlet 15/15.
  Commit `"snd: period-ring PCM core"`.

---

## Task 2: `/dev/dsp` + `/dev/mixer` file_ops and the OSS ioctls

**Files:** `kernel/dev/snd/oss.c`, `mixer.c` (new),
`lib/include/sys/soundcard.h` (new).

**Prereq check:** this task needs `file_ops.ioctl`/`.poll`. If the P0
items are not in, STOP and land them first.

- [ ] **Step 1: `soundcard.h`** — copy from Linux: `AFMT_S16_LE
  (0x10)`, `AFMT_U8 (0x08)`, `AFMT_QUERY (0)`; `SNDCTL_DSP_SPEED
  (0xC0045002)`, `_SETFMT (0xC0045005)`, `_CHANNELS (0xC0045006)`,
  `_GETOSPACE (0x8010500C)`, `_GETOPTR (0x800C5012)`, `_SETFRAGMENT
  (0xC004500A)`, `_SYNC (0x5001)`, `_RESET (0x5000)`, `_POST (0x5008)`;
  `struct audio_buf_info { int fragments, fragstotal, fragsize,
  bytes; }`; `struct count_info { int bytes, blocks; int ptr; }`;
  `SOUND_MIXER_READ_VOLUME` etc. Verify each number against a real
  `<sys/soundcard.h>`.

- [ ] **Step 2: `oss.c`** — a `file_ops` with:
  - `open` ctor: grab the single `snd_pcm` (return `-EBUSY` if already
    open — single writer), default 8000 Hz / `AFMT_U8` / mono (OSS
    defaults).
  - `write` → `snd_pcm_write`; `read` → `snd_pcm_read` (returns
    `-EINVAL` on a playback-only device until capture exists).
  - `poll` → `POLLOUT` when `snd_pcm_avail() > 0`.
  - `ioctl` → the table above, each mapping onto the core / `ops->
    set_params`. Unknown ioctl → `-EINVAL` (not `-ENOTTY` — OSS
    convention).
  - `close` → `snd_pcm_drain` + `ops->close` + release the busy flag.

- [ ] **Step 3: `mixer.c`** — `SOUND_MIXER_READ_DEVMASK` returns
  `(1<<SOUND_MIXER_PCM)|(1<<SOUND_MIXER_VOLUME)`; `READ_VOLUME` /
  `WRITE_VOLUME` store a 0-100 per-channel value and call a driver
  `set_volume` hook (stub for now). Others: current value or `-EINVAL`.

- [ ] **Step 4: register** `/dev/dsp` and `/dev/mixer` via the dynamic
  devfs API. No driver yet, so a stub `snd_pcm` with `ops` that
  `-ENODEV` on `prepare`. `dsptest` skeleton (Task 5) can already open
  `/dev/dsp` and probe ioctls that do not touch hardware
  (`SETFMT`/`SPEED` validation).

- [ ] **Step 5** — `[snd] dsp/mixer registered`; a small selftest that
  opens `/dev/dsp`, does `SNDCTL_DSP_SETFMT` with `AFMT_QUERY` and
  `AFMT_S16_LE`, `SNDCTL_DSP_SPEED` with an absurd rate (expect
  `-EINVAL`), `close`. Gauntlet 15/15. Commit
  `"snd: /dev/dsp + /dev/mixer, the OSS ioctl subset"`.

---

## Task 3: SB16 driver — probe + DMA plumbing

**Files:** `kernel/dev/snd/sb16.c` (new), ISA-DMA helper, IRQ routing,
`kernel/kernel.c`, `Makefile` (`-device sb16`).

- [ ] **Step 1: probe** — reset the DSP (write 1 then 0 to `0x226`,
  read `0x22A` for `0xAA`); `GET VERSION` (cmd `0xE1`) → expect 4.x.
  Read the mixer IRQ/DMA config (mixer register `0x80`/`0x81`) or use
  the QEMU defaults (base `0x220`, IRQ 5, DMA16 5). Log
  `[snd:sb16] DSP v%d.%d at 0x220`.

- [ ] **Step 2: the ring page + ISA DMA** — allocate one page < 16 MiB
  for the ring (reuse the FDC low allocator or add `pmm_alloc_low`).
  `isadma_setup(5, ring_phys, ring_bytes, /*to_memory=*/0)` in
  auto-init mode (mode byte `0x58` for ch5: single... actually
  **auto-init** = `0x58 | 0x10` → block/auto; use `0x48|0x10`? — use
  the datasheet: auto-init read-from-memory on ch5 = `0x58`). The
  transfer wraps the whole ring; the DSP's block-size command
  (`0x48 lo hi`) sets the half-buffer IRQ granularity =
  `period_bytes`.

- [ ] **Step 3: `snd_pcm_ops`** — `set_params`: 44100 or 22050,
  `AFMT_S16_LE`/`AFMT_U8`, 1-2 ch; else `-EINVAL`. `prepare`: program
  DMA + DSP sample rate (cmd `0x41` hi lo for output rate). `trigger
  (1)`: DSP cmd `0xB6` (16-bit auto-init output), mode byte
  (`0x10`|`0x20` stereo/mono, `0x00`|`0x10` signed), block size.
  `trigger(0)`: `0xD5` (pause 16-bit) + `0xD9` (exit auto-init).
  `pointer`: derive from the DMA channel's current count register.

- [ ] **Step 4: IRQ** — SB16 IRQ (5): ACK by reading `0x22F` (16-bit
  ack) + PIC EOI; call `snd_pcm_period_elapsed(pcm)`.

- [ ] **Step 5** — `sb16_init` registers the real `snd_pcm` with
  `/dev/dsp` (replacing the Task 2 stub). `[snd:sb16] ready`. Gauntlet
  15/15. Commit `"snd: SB16 driver -- probe, ISA DMA, PCM ops"`.

---

## Task 4: transport selftest + `dsptest`

**Files:** `kernel/dev/snd/sb16.c` (selftest hook), `userland/dsptest.c`
(new), `Makefile`.

- [ ] **Step 1: `snd_sb16_selftest`** (kernel) — open the PCM,
  `set_params(44100, S16_LE, 2)`, `prepare`, write two periods of a
  512-sample sine ramp into the ring, `trigger(1)`, then poll
  `ops->pointer()` and a period-IRQ counter for up to ~200 ms; assert
  the pointer advanced past one period **and** the IRQ fired at least
  once; `trigger(0)`. `[snd:sb16] transport selftest passed` in
  `REQUIRED_MARKERS`.

- [ ] **Step 2: `dsptest.c`** (userland) — `open("/dev/dsp")`;
  `ioctl(SNDCTL_DSP_SETFMT, &fmt=AFMT_S16_LE)`;
  `ioctl(SNDCTL_DSP_CHANNELS, &ch=2)`;
  `ioctl(SNDCTL_DSP_SPEED, &rate=44100)`; build a 1-second sine table;
  `write()` it in `period`-sized chunks, checking every return;
  `poll()` for `POLLOUT` between chunks; `ioctl(SNDCTL_DSP_SYNC)`.
  Assert an impossible rate (`96000` if hw can't) → `-EINVAL`.
  `/dev/mixer`: `SOUND_MIXER_WRITE_VOLUME` then `READ` round-trips.
  `[dsptest] ALL PASSED` in `REQUIRED_MARKERS`; spawn line in the
  Makefile.

- [ ] **Step 3** — both markers present; gauntlet **×3** (period IRQ +
  `waitq_wake` + writer-blocking is the concurrency surface). Commit
  `"snd: SB16 transport selftest + userland dsptest"`.

---

## Task 5: docs

- [ ] `docs/stdlib.md`: `/dev/dsp` + `/dev/mixer`; the supported OSS
  ioctls and exact semantics; single-writer; SB16 supported rates/
  formats; the "OSS-native, not ALSA" divergence.
- [ ] `docs/abi-compatibility.md`: `soundcard.h` struct layouts + ioctl
  numbers match Linux; ALSA absent; `poll` on `/dev/dsp` matches Linux
  OSS.
- [ ] `docs/optimization-summary.md` (or an audio section): the sound
  core + SB16.
- [ ] Commit `"snd: docs for the OSS ABI and SB16"`.

## Self-Review

**Spec coverage:** PCM model + ring (§1) → Task 1; `/dev/dsp` ioctls +
`/dev/mixer` (§1) → Task 2; SB16 probe/DMA/ops/IRQ (§2) → Task 3;
transport tests (§ testing) → Task 4; ABI (§ ABI) → Task 5. ✓
Prerequisites (§ prerequisites) are called out at the top and re-checked
in Task 2.

**Placeholder scan:** SB16 DSP command / DMA mode bytes are given as
concrete hex with a "use the datasheet" note where the exact auto-init
bit is ambiguous — a "verify against the SB16 programmer's guide" step,
not a hidden TODO. ioctl numbers are given with a "verify against a
real `<sys/soundcard.h>`" step. No bare "write tests".

**Type consistency:** `struct snd_pcm` / `snd_pcm_ops` / `snd_pcm_write`
/ `snd_pcm_period_elapsed` / `snd_pcm_avail` / `snd_pcm_drain`
consistent across tasks. `AFMT_*` / `SNDCTL_*` names match the header
Task 2 Step 1 defines.

**Downstream:** AC97 (P3) and HDA (P4) reuse `struct snd_pcm` and
`snd_pcm_ops` unchanged; they need the PCI milestone (P2) first. Those
are separate plans.
