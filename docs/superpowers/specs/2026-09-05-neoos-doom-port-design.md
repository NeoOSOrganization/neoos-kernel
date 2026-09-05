# NeoOS Doom Port — Design

**Date:** 2026-09-05
**Status:** approved, ready for implementation
**Trigger:** the framebuffer blit abstraction
(`docs/superpowers/specs/2026-09-05-fb-device-blit-abstraction-design.md`)
and the AC97 audio driver
(`docs/superpowers/specs/2026-09-05-ac97-audio-driver-design.md`) both
exist now specifically so a real, demanding userland application can
prove them under load. Doom is that application.

## 1. Problem

NeoOS has no ported game or graphical application heavier than the
kernel's own framebuffer terminal and the 3D ASCII viewer (which draws
to a terminal, not a real framebuffer). Nothing in the org has
exercised `/dev/fb0`, `/dev/input/event0`, and `/dev/snd/pcmC0D0p`
together, under sustained real-time load (Doom targets 35 logic ticks/
second with continuous video+audio output) rather than as isolated
correctness checks.

## 2. Scope

**Repository:** a new `neoos-doom`, following the exact contract
`neoos-busybox`/`neoos-3d-ascii-viewer` already establish: an
unmodified upstream source as a git submodule, built against musl
(`MUSL_DIR`), producing one static `.nex` binary + a `.test.json`
manifest, with `make`/`make smoke-test`/`make clean` as the required
targets.

**Upstream:** `doomgeneric` (`github.com/ozkl/doomgeneric`) — a fork of
Chocolate Doom's engine with all platform I/O funneled through a
6-function contract (`DG_Init`, `DG_DrawFrame`, `DG_SleepMs`,
`DG_GetTicksMs`, `DG_GetKey`, `DG_SetWindowTitle`) plus Chocolate
Doom's inherited `sound_module_t`/`music_module_t` interfaces
(`i_sound.h`) for audio, unmodified.

**In scope:**
- The `DG_*` platform contract: video via `/dev/fb0`, input via
  `/dev/input/event0`, timing via musl.
- `sound_module_t`: a real multi-channel software mixer for sound
  effects (Doom's overlapping gunshots/pain sounds/pickups), writing
  to `/dev/snd/pcmC0D0p`.
- `music_module_t`: a self-contained MUS/MIDI event synthesizer (basic
  waveforms per General MIDI instrument family, not sampled
  instruments), mixed into the SAME output stream as sound effects —
  there is only one audio device.
- Bundled shareware `doom1.wad` (id Software has permitted free
  redistribution since 1995).
- `smoke-test.sh` validating ELF format/static linking only, matching
  the established port contract.

**Out of scope (deliberately, YAGNI):**
- Real GM/soundfont-quality music. That needs a ported audio library
  (SDL_mixer or equivalent) — a separate undertaking with no clear
  payoff for a bare-metal kernel port. The built-in synth plays the
  right notes and rhythm; it does not sound like authentic
  instruments.
- Any change to `/dev/fb0`, `/dev/input/event0`, or
  `/dev/snd/pcmC0D0p`'s existing kernel-side behavior. All three
  already work correctly; this port is a consumer, not a co-designer.
- A full interactive in-QEMU playthrough test. Not headlessly
  testable in a way that proves "the game is fun/correct" — this
  repo's own test stays at the established ELF/static-link smoke
  check; a later `neoos-os-builder` integration milestone (or
  `neoos-kernel-tests-common`) is where real end-to-end validation
  belongs, matching the precedent already set for `/dev/snd`'s
  in-kernel-only selftest.
- Multiplayer (Doom's networking). Nothing in scope needs it.

## 3. Decisions

### 3.1 Repository layout

```
neoos-doom/
├── .gitmodules              # upstream = doomgeneric
├── upstream/                 # doomgeneric, UNMODIFIED
├── neoos-shim/
│   ├── i_video_neoos.c       # DG_Init, DG_DrawFrame
│   ├── i_input_neoos.c       # DG_GetKey, keycode translation table
│   ├── i_timer_neoos.c       # DG_SleepMs, DG_GetTicksMs
│   ├── i_sound_neoos.c       # sound_module_t: mixer + PCM output
│   ├── i_music_neoos.c       # music_module_t: MUS/MIDI synth
│   └── mixer.c / mixer.h     # shared ring buffer + mix loop both
│                              # i_sound_neoos.c and i_music_neoos.c
│                              # write into
├── assets/doom1.wad          # bundled shareware IWAD
├── Makefile
├── user.ld
├── doom.test.json
└── smoke-test.sh
```

### 3.2 Video (`DG_Init` / `DG_DrawFrame`)

`DG_Init` opens and `mmap`s `/dev/fb0`, reads geometry via
`FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` (already implemented,
`docs/stdlib.md`'s `/dev/fb0` section). `DOOMGENERIC_RESX`/
`DOOMGENERIC_RESY` default to 640×400 in `doomgeneric.h` (doomgeneric's
own 2x upscale of Doom's true 320×200 render) — overridden to `320`/
`200` via `-D` compile flags (the header's `#ifndef` guard supports
this) so `DG_ScreenBuffer` is the genuinely native buffer and this
port's own integer-scale-to-fit (below) is the only scaling that
happens, not doomgeneric's baked-in 2x stacked with a second one.

`DG_DrawFrame` computes `scale = max(1, min(fb_width/320,
fb_height/200))`, then blits `DG_ScreenBuffer` into the mmapped
framebuffer replicated `scale`×`scale` per pixel, centered with black
letterbox bars on any remainder. `pixel_t` is `uint32_t` (not
`CMAP256`) so `DG_ScreenBuffer`'s format is already the framebuffer's
canonical XRGB8888 — no palette lookup needed at blit time.

### 3.3 Input (`DG_GetKey`)

`DG_Init` opens `/dev/input/event0` non-blocking. `DG_GetKey` drains
one `struct input_event` per call (already Linux-shaped per
`docs/stdlib.md`'s evdev section — `EV_KEY`, standard `KEY_*` codes),
translates the Linux keycode to doomgeneric's key enum via a static
lookup table (the same approach doomgeneric's other platform backends
use), and reports press/release through the `int* pressed, unsigned
char* key` out-parameters. Returns 0 when the ring is empty (matching
`/dev/input/event0`'s documented `-EAGAIN`-on-empty behavior — polled,
never blocks the game loop).

### 3.4 Timing

`DG_SleepMs` → musl's `nanosleep`. `DG_GetTicksMs` → musl's
`clock_gettime(CLOCK_MONOTONIC, ...)` scaled to milliseconds. Both
already Linux-shaped syscalls per this project's stdlib convention —
no new kernel primitive.

### 3.5 The shared mixer (`mixer.c`)

Both sound effects and music write into one shared ring buffer at a
fixed 48kHz/stereo/16-bit-signed rate (matching `/dev/snd/pcmC0D0p`'s
one supported format exactly — no resampling needed at the `write()`
boundary). A single mix step, called once per `doomgeneric_Tick()`:

1. For each active sfx channel (`NUM_CHANNELS`, Doom's usual 8): read
   the next block of that channel's already-loaded PCM samples
   (converted once at `StartSound` time from the sfx lump's native
   DMX format — an 8-byte header plus 8-bit unsigned mono PCM,
   typically 11025Hz — to 16-bit signed stereo at 48kHz via simple
   linear interpolation resampling), scale by that channel's volume
   (`UpdateSoundParams`'s `vol`, 0–127) and pan (`sep`, stereo
   separation), and add into an accumulator.
2. Add the music synth's next block (§3.6) into the same accumulator,
   scaled by the music volume (`SetMusicVolume`).
3. Clip the accumulator to `int16_t` range (hard clip — no dynamic
   compression; simplest correct behavior, matches how most simple
   software mixers of this class handle overflow).
4. `write()` the resulting block to `/dev/snd/pcmC0D0p`.

`sound_module_t`'s `Init`/`Shutdown`/`GetSfxLumpNum`/`CacheSounds`/
`StartSound`/`StopSound`/`SoundIsPlaying`/`UpdateSoundParams` all
operate on this shared channel array; `Update()` is where the mix step
above actually runs.

### 3.6 Music (`music_module_t`)

`RegisterSong` receives a MUS-format lump (Doom's music format;
`mus2mid.c`, already bundled unmodified in `doomgeneric/upstream`,
converts it to standard MIDI events — used as-is, not reimplemented).
The shim parses those MIDI events itself (note-on/off, program change,
channel volume) and drives a small built-in synth:

- One active oscillator per currently-sounding MIDI note (basic
  polyphony, no voice-stealing limit beyond a fixed max — 32 voices is
  plenty for Doom's MUS tracks).
- Waveform chosen by coarse General MIDI program-number family, not
  per-instrument: square wave for organ/brass-family programs,
  triangle wave for string/pad-family programs, a short noise burst
  for the percussion channel (MIDI channel 10). This is explicitly
  "the right notes and rhythm," not "the right timbre" — the spec's
  stated, accepted limitation (§2).
- Simple linear ADSR-free envelope: full volume on note-on, hard stop
  on note-off (no release tail) — bounded scope, not a real synth
  engine.

`PlaySong`/`StopSong`/`PauseMusic`/`ResumeMusic` control which
registered song is currently feeding the synth; `Poll()` (called every
tick, per doomgeneric's existing calling convention) advances the MIDI
event clock and starts/stops oscillators as events come due.

### 3.7 WAD asset

`assets/doom1.wad` committed directly into the repo (shareware,
freely redistributable). Where it ends up on a bootable disk image is
`neoos-os-builder`'s job (matching the existing contract: a port
produces a `.nex` + manifest, not a full image) — out of scope here,
same reasoning as the original brainstorm.

### 3.8 Build

```makefile
MUSL_DIR ?= ../neoos-musl/build-output
UPSTREAM_DIR ?= upstream
SHIM_DIR ?= neoos-shim

# doomgeneric ships ONE doomgeneric_<platform>.c frontend per platform
# (win, xlib, emscripten, android, ...), each defining its own
# DG_Init/DG_DrawFrame/etc -- every one of them EXCLUDED, or the
# linker sees multiple definitions of every DG_* symbol this repo's
# own neoos-shim/ provides. i_sdl*.c, i_allegro*.c, i_cdmus.c, and
# gusconf.c(.h) are ALSO excluded -- SDL/Allegro/legacy-hardware
# backends this project never ports. Everything else (the
# platform-agnostic engine: d_main.c, g_game.c, i_sound.c/i_sound.h's
# shared struct definitions, mus2mid.c, ...) is compiled as-is.
DOOM_SRCS := $(filter-out \
  $(UPSTREAM_DIR)/doomgeneric/doomgeneric_%.c \
  $(UPSTREAM_DIR)/doomgeneric/i_sdl%.c \
  $(UPSTREAM_DIR)/doomgeneric/i_allegro%.c \
  $(UPSTREAM_DIR)/doomgeneric/i_cdmus.c \
  $(UPSTREAM_DIR)/doomgeneric/gusconf.c, \
  $(wildcard $(UPSTREAM_DIR)/doomgeneric/*.c)) \
  $(wildcard $(SHIM_DIR)/*.c)
```

Same `MUSL_CFLAGS`/link pattern as `neoos-3d-ascii-viewer`'s Makefile
(`-mcmodel=large -fno-pic -mno-red-zone -static -nostdlib`, linked
against `$(MUSL_DIR)/lib/crt1.o` + `-lc -lgcc`), plus
`-DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200` (see §3.2).

## 4. Testing

- `smoke-test.sh`: ELF format + static linking only, matching the
  busybox/3d-viewer contract exactly. No headless gameplay assertion —
  see §2's out-of-scope note.
- Manual verification (not automated, same status as the framebuffer
  blit migration's `make run` visual spot-check): boot with the WAD
  present, confirm the title screen renders, confirm a keypress
  advances state, confirm a test-fire sound effect and a few seconds
  of title music are audible.

## 5. Migration ordering

1. Repository scaffold + submodule + Makefile + excluded-source
   filtering (produces a build that FAILS at link time on the missing
   `DG_*`/sound/music symbols — proves the exclusion list is right
   before any shim code exists).
2. `DG_Init`/`DG_DrawFrame` (video) — playable-but-silent-and-keyboard-
   dead Doom, visually confirmed via `make run`-equivalent.
3. `DG_GetKey` (input) — playable, silent.
4. `DG_SleepMs`/`DG_GetTicksMs` (timing) — correct game speed (likely
   already incidentally correct from step 2, but confirmed explicitly
   here).
5. `mixer.c` + `sound_module_t` (sound effects) — audible sfx.
6. `music_module_t` (music synth) — audible music.
7. `doom1.wad` bundled, `smoke-test.sh`, manual verification, first
   commit tagged as the completed port.

This lands in a new `neoos-doom` repository under
`github.com/NeoOSOrganization`, not `neoos-kernel`.
