# NeoOS Doom Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new `neoos-doom` repository running `doomgeneric` on NeoOS —
video via `/dev/fb0`, input via `/dev/input/event0`, a real
multi-channel software sound mixer and a built-in MUS/MIDI synth, both
writing to `/dev/snd/pcmC0D0p`.

**Architecture:** `doomgeneric` (upstream, unmodified, git submodule)
built against musl, with a `neoos-shim/` directory providing every
platform-specific piece doomgeneric expects: the `DG_*` functions,
`sound_module_t`, and `music_module_t`.

**Tech Stack:** C, musl libc, no new NeoOS kernel changes (this plan
is entirely userland, consuming interfaces that already exist).

**Spec:** `docs/superpowers/specs/2026-09-05-neoos-doom-port-design.md`

## Global Constraints

- **Fixed audio format**: 16-bit signed LE, stereo, 48000 Hz — the one
  format `/dev/snd/pcmC0D0p` accepts (`docs/stdlib.md`).
- **`DOOMGENERIC_RESX`/`RESY` overridden to 320×200** at compile time
  — see spec §3.2. Do not use the 640×400 default.
- **Every `doomgeneric_*.c`, `i_sdl*.c`, `i_allegro*.c`, `i_cdmus.c`,
  `gusconf.c`(`.h`) source file is excluded from the build** — see
  spec §3.8's exact `filter-out` list. Missing even one causes a
  duplicate-symbol link error (most likely `DG_Init`/`DG_DrawFrame`
  from whichever `doomgeneric_<platform>.c` slipped through).
- **Music is deliberately lower-fidelity than real GM/soundfont
  playback** (right notes/rhythm, basic waveforms per instrument
  family, no sampled instruments) — this is the spec's stated,
  accepted scope, not a bug to "complete" later in this plan.
- **This creates a new repository** (`neoos-doom` under
  `github.com/NeoOSOrganization`), not a change to `neoos-kernel`.

---

### Task 1: Repository scaffold, submodule, build-exclusion Makefile

**Files:**
- Create (new repo `neoos-doom`): `.gitmodules`, `Makefile`,
  `user.ld`, `.gitignore`, `README.md`
- Create: `upstream/` (submodule pointing at
  `https://github.com/ozkl/doomgeneric`)

**Interfaces:**
- Produces: a `Makefile` whose `DOOM_SRCS` variable Task 2 onward
  appends `neoos-shim/*.c` files to (already includes the wildcard,
  so no Makefile change needed as shim files are added — only new
  files matching `neoos-shim/*.c` need to exist).

- [ ] **Step 1: Create the repository and clone the submodule**

```bash
mkdir neoos-doom && cd neoos-doom
git init
git submodule add https://github.com/ozkl/doomgeneric upstream
git submodule update --init --recursive
```

- [ ] **Step 2: Create `user.ld`**

Copy `neoos-3d-ascii-viewer`'s `user.ld` verbatim (fetch it from
`https://github.com/NeoOSOrganization/neoos-3d-ascii-viewer` — every
port in this org uses the identical linker script; there is no
port-specific difference to make here).

- [ ] **Step 3: Create `.gitignore`**

```
build/
*.o
*.nex
```

- [ ] **Step 4: Create the Makefile**

```makefile
# Doom port for NeoOS, via doomgeneric (unmodified upstream submodule)
# -- see docs/superpowers/specs/2026-09-05-neoos-doom-port-design.md
# in neoos-kernel for the full design.
MUSL_DIR ?= ../neoos-musl/build-output
UPSTREAM_DIR ?= upstream
SHIM_DIR ?= neoos-shim
BUILD_DIR ?= build

CC := x86_64-elf-gcc
MUSL_CFLAGS := -static -nostdlib -nostdinc -ffreestanding \
	-mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
	-isystem $(MUSL_DIR)/include -I$(UPSTREAM_DIR)/doomgeneric \
	-DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
	-DNORMALUNIX -DLINUX

# doomgeneric ships ONE doomgeneric_<platform>.c frontend per platform
# (win, xlib, emscripten, android, ...), each defining its own
# DG_Init/DG_DrawFrame/etc -- every one of them EXCLUDED, or the
# linker sees multiple definitions of every DG_* symbol neoos-shim/
# provides. i_sdl*.c, i_allegro*.c, i_cdmus.c, and gusconf.c are ALSO
# excluded -- SDL/Allegro/legacy-hardware backends never ported here.
DOOM_SRCS := $(filter-out \
  $(UPSTREAM_DIR)/doomgeneric/doomgeneric_%.c \
  $(UPSTREAM_DIR)/doomgeneric/i_sdl%.c \
  $(UPSTREAM_DIR)/doomgeneric/i_allegro%.c \
  $(UPSTREAM_DIR)/doomgeneric/i_cdmus.c \
  $(UPSTREAM_DIR)/doomgeneric/gusconf.c, \
  $(wildcard $(UPSTREAM_DIR)/doomgeneric/*.c)) \
  $(wildcard $(SHIM_DIR)/*.c)

.PHONY: all clean smoke-test
all: $(BUILD_DIR)/doom.nex

$(BUILD_DIR)/doom.nex: $(DOOM_SRCS) user.ld
	@[ -f "$(MUSL_DIR)/lib/libc.a" ] || { echo "error: musl not found at $(MUSL_DIR); build neoos-musl first" >&2; exit 1; }
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MUSL_CFLAGS) -I$(SHIM_DIR) -T user.ld -z noexecstack \
		-o $@ $(MUSL_DIR)/lib/crt1.o $(DOOM_SRCS) \
		-L$(MUSL_DIR)/lib -lc -lgcc -lm
	cp doom.test.json $(BUILD_DIR)/doom.test.json
	cp assets/doom1.wad $(BUILD_DIR)/doom1.wad

clean:
	rm -rf $(BUILD_DIR)

smoke-test: $(BUILD_DIR)/doom.nex
	./smoke-test.sh $(BUILD_DIR)/doom.nex
```

- [ ] **Step 5: Attempt the build to confirm the exclusion list is complete**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
mkdir -p neoos-shim
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: compiles every included upstream `.c` file cleanly (no
`neoos-shim/*.c` exists yet, so link fails with undefined references
to `DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`,
`DG_GetKey`, `DG_SetWindowTitle`, and the `sound_module_t`/
`music_module_t` symbols this plan's later tasks provide). If it fails
with anything OTHER than undefined-reference errors for exactly those
symbols (e.g., a compile error, or a *different* set of undefined
symbols like a stray `SDL_*` reference), the exclusion list in Step 4
is incomplete — stop and fix it before continuing; do not proceed with
a broken exclusion list, since every later task's build will silently
inherit the same gap.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "scaffold: doomgeneric submodule, build-exclusion Makefile"
```

---

### Task 2: Video (`DG_Init` / `DG_DrawFrame`)

**Files:**
- Create: `neoos-shim/i_video_neoos.c`

**Interfaces:**
- Consumes: `pixel_t *DG_ScreenBuffer` (extern, `uint32_t*` since
  `CMAP256` is not defined — already allocated by `doomgeneric_Create`
  before `DG_Init` runs, sized `320*200*4` bytes after this plan's
  `DOOMGENERIC_RESX`/`RESY` override).
- Consumes: `/dev/fb0`'s `mmap`, `FBIOGET_VSCREENINFO`,
  `FBIOGET_FSCREENINFO` (already implemented — `docs/stdlib.md`'s
  `/dev/fb0` section has the exact ioctl numbers and struct layouts;
  `<linux/fb.h>`-equivalent constants come from musl's own headers).
- Produces: `void DG_Init(void)`, `void DG_DrawFrame(void)` —
  Task 3/4/5/6's `neoos-shim/main.c` (Task 7) is the only other file
  that touches these by name (calling them indirectly via
  `doomgeneric_Create`/`doomgeneric_Tick`, never directly).

- [ ] **Step 1: Write `neoos-shim/i_video_neoos.c`**

```c
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <string.h>
#include "doomgeneric.h"

static int fb_fd = -1;
static uint8_t *fb_mem;
static uint32_t fb_width, fb_height, fb_pitch;
static uint32_t scale, off_x, off_y;

void DG_Init(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "doom: could not open /dev/fb0\n");
        return;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "doom: FBIOGET_*SCREENINFO failed\n");
        return;
    }
    fb_width  = vinfo.xres;
    fb_height = vinfo.yres;
    fb_pitch  = finfo.line_length;

    fb_mem = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE,
                  MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        fprintf(stderr, "doom: mmap(/dev/fb0) failed\n");
        fb_mem = NULL;
        return;
    }

    // Integer-scale-to-fit, letterboxed -- spec section 3.2.
    uint32_t sx = fb_width / DOOMGENERIC_RESX;
    uint32_t sy = fb_height / DOOMGENERIC_RESY;
    scale = sx < sy ? sx : sy;
    if (scale < 1) { scale = 1; }
    off_x = (fb_width  - DOOMGENERIC_RESX * scale) / 2;
    off_y = (fb_height - DOOMGENERIC_RESY * scale) / 2;

    // Clear the whole screen once, up front, so the letterbox bars
    // are black from the first frame rather than showing whatever
    // garbage the framebuffer held.
    memset(fb_mem, 0, (size_t)fb_pitch * fb_height);
}

void DG_DrawFrame(void) {
    if (!fb_mem || !DG_ScreenBuffer) { return; }

    for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
        const uint32_t *src_row = DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX;
        for (uint32_t sy = 0; sy < scale; sy++) {
            uint8_t *dst_row = fb_mem +
                (size_t)(off_y + y * scale + sy) * fb_pitch +
                (size_t)off_x * 4;
            for (uint32_t x = 0; x < DOOMGENERIC_RESX; x++) {
                uint32_t px = src_row[x];
                for (uint32_t sx = 0; sx < scale; sx++) {
                    memcpy(dst_row + (size_t)(x * scale + sx) * 4, &px, 4);
                }
            }
        }
    }
}

void DG_SetWindowTitle(const char *title) {
    (void)title;   // no window manager to set a title on
}
```

- [ ] **Step 2: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: `DG_Init`/`DG_DrawFrame`/`DG_SetWindowTitle` no longer
undefined; remaining undefined references are `DG_SleepMs`,
`DG_GetTicksMs`, `DG_GetKey`, and the sound/music module symbols
(Tasks 3–6). If a NEW undefined symbol appears that isn't one of
those (e.g. a musl header didn't provide `struct fb_var_screeninfo`),
stop and check musl's `include/linux/fb.h` exists at
`$(MUSL_DIR)/include/linux/fb.h` before continuing.

- [ ] **Step 3: Commit**

```bash
git add neoos-shim/i_video_neoos.c
git commit -m "video: DG_Init/DG_DrawFrame via /dev/fb0, integer-scale letterboxed"
```

---

### Task 3: Input (`DG_GetKey`)

**Files:**
- Create: `neoos-shim/i_input_neoos.c`

**Interfaces:**
- Consumes: `/dev/input/event0`'s `struct input_event` (24 bytes:
  `int64_t tv_sec, tv_usec; uint16_t type, code; int32_t value;`),
  `EV_KEY`, Linux `KEY_*` codes (`docs/stdlib.md`'s evdev section).
- Produces: `int DG_GetKey(int *pressed, unsigned char *key)` — no
  other file in this plan calls it directly (doomgeneric's own engine
  code does, via the `DG_GetKey` symbol).

- [ ] **Step 1: Write `neoos-shim/i_input_neoos.c`**

```c
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include "doomkeys.h"

static int ev_fd = -1;

// Only the keys Doom's menu/game actually reads translate to
// something other than 0 -- an unmapped key is silently ignored,
// matching every other doomgeneric platform backend's behavior for
// keys it does not recognize.
static unsigned char translate_key(uint16_t code) {
    switch (code) {
    case KEY_UP:        return KEY_UPARROW;
    case KEY_DOWN:      return KEY_DOWNARROW;
    case KEY_LEFT:      return KEY_LEFTARROW;
    case KEY_RIGHT:     return KEY_RIGHTARROW;
    case KEY_ENTER:     return KEY_ENTER;
    case KEY_ESC:       return KEY_ESCAPE;
    case KEY_SPACE:     return KEY_USE;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL: return KEY_FIRE;
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: return KEY_RSHIFT;
    case KEY_LEFTALT:   return KEY_LALT;
    case KEY_RIGHTALT:  return KEY_RALT;
    case KEY_TAB:       return KEY_TAB;
    case KEY_BACKSPACE: return KEY_BACKSPACE;
    case KEY_MINUS:     return KEY_MINUS;
    case KEY_EQUAL:     return KEY_EQUALS;
    default:
        // Ordinary letters/digits map straight to their ASCII value,
        // the same convention every doomgeneric platform backend
        // uses for "just a normal key". Linux's KEY_A..KEY_Z are
        // 30..38,16..25,44..50 (not contiguous with 'a'..'z'), so a
        // small explicit table -- not arithmetic -- is the correct,
        // unambiguous mapping.
        {
            static const struct { uint16_t code; unsigned char ch; } alpha[] = {
                {KEY_A,'a'},{KEY_B,'b'},{KEY_C,'c'},{KEY_D,'d'},{KEY_E,'e'},
                {KEY_F,'f'},{KEY_G,'g'},{KEY_H,'h'},{KEY_I,'i'},{KEY_J,'j'},
                {KEY_K,'k'},{KEY_L,'l'},{KEY_M,'m'},{KEY_N,'n'},{KEY_O,'o'},
                {KEY_P,'p'},{KEY_Q,'q'},{KEY_R,'r'},{KEY_S,'s'},{KEY_T,'t'},
                {KEY_U,'u'},{KEY_V,'v'},{KEY_W,'w'},{KEY_X,'x'},{KEY_Y,'y'},
                {KEY_Z,'z'},
                {KEY_0,'0'},{KEY_1,'1'},{KEY_2,'2'},{KEY_3,'3'},{KEY_4,'4'},
                {KEY_5,'5'},{KEY_6,'6'},{KEY_7,'7'},{KEY_8,'8'},{KEY_9,'9'},
            };
            for (unsigned i = 0; i < sizeof(alpha)/sizeof(alpha[0]); i++) {
                if (alpha[i].code == code) { return alpha[i].ch; }
            }
        }
        return 0;
    }
}

// Called once from DG_Init (Task 2's i_video_neoos.c doesn't open
// this device; it belongs to input, so it opens its own fd here,
// lazily on first DG_GetKey call).
static void ensure_open(void) {
    if (ev_fd < 0) {
        ev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    }
}

int DG_GetKey(int *pressed, unsigned char *key) {
    ensure_open();
    if (ev_fd < 0) { return 0; }

    struct input_event ev;
    for (;;) {
        ssize_t n = read(ev_fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) { return 0; }   // -EAGAIN: ring empty
        if (ev.type != EV_KEY) { continue; }   // skip EV_SYN/EV_MSC

        unsigned char k = translate_key(ev.code);
        if (k == 0) { continue; }   // unmapped key: drop and keep draining

        *pressed = ev.value ? 1 : 0;   // 1=press, 0=release (2=autorepeat, treated as press)
        *key = k;
        return 1;
    }
}
```

- [ ] **Step 2: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: `DG_GetKey` no longer undefined. Remaining undefined
references: `DG_SleepMs`, `DG_GetTicksMs`, sound/music symbols.

- [ ] **Step 3: Commit**

```bash
git add neoos-shim/i_input_neoos.c
git commit -m "input: DG_GetKey via /dev/input/event0"
```

---

### Task 4: Timing (`DG_SleepMs` / `DG_GetTicksMs`)

**Files:**
- Create: `neoos-shim/i_timer_neoos.c`

**Interfaces:**
- Consumes: musl's `nanosleep`, `clock_gettime(CLOCK_MONOTONIC, ...)`.
- Produces: `void DG_SleepMs(uint32_t ms)`, `uint32_t DG_GetTicksMs(void)`.

- [ ] **Step 1: Write `neoos-shim/i_timer_neoos.c`**

```c
#include <stdint.h>
#include <time.h>

void DG_SleepMs(uint32_t ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}
```

- [ ] **Step 2: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: `DG_SleepMs`/`DG_GetTicksMs` no longer undefined. Remaining
undefined references: only the sound/music module symbols (Tasks 5–6)
and `main` (Task 7).

- [ ] **Step 3: Commit**

```bash
git add neoos-shim/i_timer_neoos.c
git commit -m "timing: DG_SleepMs/DG_GetTicksMs via musl nanosleep/clock_gettime"
```

---

### Task 5: Sound effects (`mixer.c` + `sound_module_t`)

**Files:**
- Create: `neoos-shim/mixer.h`, `neoos-shim/mixer.c`
- Create: `neoos-shim/i_sound_neoos.c`

**Interfaces:**
- Consumes: `struct sfxinfo_struct` (aka `sfxinfo_t`, from upstream's
  `i_sound.h`):
  ```c
  struct sfxinfo_struct {
      char *tagname; char name[9]; int priority;
      sfxinfo_t *link; int pitch; int volume; int usefulness;
      int lumpnum; int numchannels; void *driver_data;
  };
  ```
  (verified against upstream's actual header while writing this plan,
  not reconstructed from memory).
- Consumes: `sound_module_t`'s exact field order (from upstream's
  `i_sound.h`, same verification):
  ```c
  typedef struct {
      snddevice_t *sound_devices; int num_sound_devices;
      boolean (*Init)(boolean use_sfx_prefix);
      void (*Shutdown)(void);
      int (*GetSfxLumpNum)(sfxinfo_t *sfxinfo);
      void (*Update)(void);
      void (*UpdateSoundParams)(int channel, int vol, int sep);
      int (*StartSound)(sfxinfo_t *sfxinfo, int channel, int vol, int sep);
      void (*StopSound)(int channel);
      boolean (*SoundIsPlaying)(int channel);
      void (*CacheSounds)(sfxinfo_t *sounds, int num_sounds);
  } sound_module_t;
  ```
- Consumes: upstream's `W_CacheLumpNum`/`W_GetNumForName` (WAD lump
  access, already implemented by the unmodified engine sources this
  repo compiles — declared in `w_wad.h`).
- Produces: `struct mixer_channel` API in `mixer.h` that Task 6's
  `i_music_neoos.c` also calls (`mixer_add_music_samples`) to feed the
  same output stream.
- Produces: `sound_module_t DG_sound_module` — Task 7's `main.c`
  assigns this to doomgeneric's active sound module pointer (the exact
  assignment mechanism — a global doomgeneric expects to be set, or an
  `#ifdef`-selected default — must be confirmed against
  `upstream/doomgeneric/i_sound.c`'s actual module-selection logic
  before Task 7's Step 1; note this explicitly in that task rather
  than guessing here).

- [ ] **Step 1: Write `neoos-shim/mixer.h`**

```c
#ifndef NEOOS_DOOM_MIXER_H
#define NEOOS_DOOM_MIXER_H

#include <stdint.h>

#define MIXER_RATE     48000
#define MIXER_CHANNELS 2
#define MIXER_NUM_SFX_CHANNELS 8

// Called once at startup: opens /dev/snd/pcmC0D0p, sets the fixed
// format via SNDRV_PCM_IOCTL_HW_PARAMS. Returns 0 on success.
int mixer_init(void);

// Loads a resampled-to-48kHz-stereo-16bit copy of `src` (raw 8-bit
// unsigned mono PCM at `src_rate` Hz, `src_len` samples) into sfx
// channel `ch`, replacing whatever that channel was playing. `vol` is
// Doom's 0-127 scale, `sep` is Doom's -1..254..+1-mapped stereo
// separation (0 = full left, 254 = center, ... -- see i_sound.c's
// actual encoding, confirmed in Step 3 below).
void mixer_play_sfx(int ch, const uint8_t *src, uint32_t src_len,
                     uint32_t src_rate, int vol, int sep);
void mixer_stop_sfx(int ch);
int  mixer_sfx_playing(int ch);
void mixer_set_sfx_params(int ch, int vol, int sep);

// Music (Task 6) feeds its synthesized samples in here, mixed into
// the SAME output stream as the sfx channels above -- there is only
// one audio device. `frames` is the number of stereo frames in
// `samples` (already 48kHz/stereo/16-bit).
void mixer_add_music_samples(const int16_t *samples, uint32_t frames);
void mixer_set_music_volume(int vol);

// Runs one mix step (spec section 3.5, steps 1-4) and writes the
// result to /dev/snd/pcmC0D0p. Called once per doomgeneric_Tick()
// from Task 7's main.c.
void mixer_update(void);

#endif
```

- [ ] **Step 2: Write `neoos-shim/mixer.c`**

```c
#include "mixer.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

// Real Linux ALSA struct layouts/ioctl numbers -- see
// docs/stdlib.md's Audio section in neoos-kernel for the source of
// truth (upstream Linux's include/uapi/sound/asound.h). Duplicated
// here rather than shared via a header because this repo has no
// dependency on neoos-kernel's source tree at build time (same
// "standalone build" contract every port in this org follows).
#define SNDRV_MASK_MAX 256
struct snd_mask { uint32_t bits[(SNDRV_MASK_MAX+31)/32]; };
struct snd_interval { unsigned int min, max; unsigned int openmin:1, openmax:1, integer:1, empty:1; };
#define SNDRV_PCM_HW_PARAM_FORMAT 1
#define SNDRV_PCM_HW_PARAM_FIRST_MASK 0
#define SNDRV_PCM_HW_PARAM_CHANNELS 10
#define SNDRV_PCM_HW_PARAM_RATE 11
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL 8
#define SNDRV_PCM_FORMAT_S16_LE 2
struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    unsigned int rmask, cmask, info, msbits, rate_num, rate_den;
    unsigned long fifo_size;
    unsigned char sync[16];
    unsigned char reserved[48];
};
#define SNDRV_PCM_IOCTL_HW_PARAMS 0xC2604111u

static int pcm_fd = -1;

struct sfx_channel {
    int16_t *data;      // resampled 48kHz stereo 16-bit, owned
    uint32_t frames;
    uint32_t pos;
    int vol, sep;
    int active;
};
static struct sfx_channel channels[MIXER_NUM_SFX_CHANNELS];

#define MUSIC_BUF_FRAMES (MIXER_RATE / 35 * 2)   // a couple of tics' worth
static int16_t music_buf[MUSIC_BUF_FRAMES * MIXER_CHANNELS];
static uint32_t music_frames_available;
static int music_volume = 127;

int mixer_init(void) {
    pcm_fd = open("/dev/snd/pcmC0D0p", O_WRONLY);
    if (pcm_fd < 0) { return -1; }

    struct snd_pcm_hw_params hp;
    memset(&hp, 0, sizeof(hp));
    hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = (1u << SNDRV_PCM_FORMAT_S16_LE);
    hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 2;
    hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = 2;
    hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 48000;
    hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = 48000;
    if (ioctl(pcm_fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) != 0) {
        close(pcm_fd); pcm_fd = -1; return -1;
    }
    return 0;
}

void mixer_play_sfx(int ch, const uint8_t *src, uint32_t src_len,
                     uint32_t src_rate, int vol, int sep) {
    if (ch < 0 || ch >= MIXER_NUM_SFX_CHANNELS) { return; }
    struct sfx_channel *c = &channels[ch];
    free(c->data);

    // Linear-interpolation resample from src_rate (typically 11025Hz,
    // 8-bit unsigned mono) to 48kHz stereo 16-bit signed.
    uint32_t out_frames = (uint32_t)((uint64_t)src_len * MIXER_RATE / src_rate);
    c->data = malloc((size_t)out_frames * MIXER_CHANNELS * sizeof(int16_t));
    if (!c->data) { c->active = 0; return; }

    for (uint32_t i = 0; i < out_frames; i++) {
        double src_pos = (double)i * src_rate / MIXER_RATE;
        uint32_t i0 = (uint32_t)src_pos;
        uint32_t i1 = i0 + 1 < src_len ? i0 + 1 : i0;
        double frac = src_pos - i0;
        double s = ((double)src[i0] - 128.0) * (1.0 - frac) + ((double)src[i1] - 128.0) * frac;
        int16_t sample = (int16_t)(s * 256.0);   // 8-bit unsigned -> 16-bit signed
        c->data[i * 2 + 0] = sample;
        c->data[i * 2 + 1] = sample;   // mono source, both channels equal before pan
    }
    c->frames = out_frames;
    c->pos = 0;
    c->vol = vol;
    c->sep = sep;
    c->active = 1;
}

void mixer_stop_sfx(int ch) {
    if (ch < 0 || ch >= MIXER_NUM_SFX_CHANNELS) { return; }
    channels[ch].active = 0;
}

int mixer_sfx_playing(int ch) {
    if (ch < 0 || ch >= MIXER_NUM_SFX_CHANNELS) { return 0; }
    return channels[ch].active;
}

void mixer_set_sfx_params(int ch, int vol, int sep) {
    if (ch < 0 || ch >= MIXER_NUM_SFX_CHANNELS) { return; }
    channels[ch].vol = vol;
    channels[ch].sep = sep;
}

void mixer_add_music_samples(const int16_t *samples, uint32_t frames) {
    uint32_t n = frames < MUSIC_BUF_FRAMES ? frames : MUSIC_BUF_FRAMES;
    memcpy(music_buf, samples, (size_t)n * MIXER_CHANNELS * sizeof(int16_t));
    music_frames_available = n;
}

void mixer_set_music_volume(int vol) { music_volume = vol; }

void mixer_update(void) {
    if (pcm_fd < 0) { return; }

    uint32_t frames = MUSIC_BUF_FRAMES;
    int32_t *accum = calloc(frames * MIXER_CHANNELS, sizeof(int32_t));
    if (!accum) { return; }

    for (int ch = 0; ch < MIXER_NUM_SFX_CHANNELS; ch++) {
        struct sfx_channel *c = &channels[ch];
        if (!c->active) { continue; }
        // sep: 0=full left .. 128=center .. 254=full right (Doom's
        // convention -- confirmed against i_sound.c's actual encoding
        // in Task 5 Step 3 below before this ships).
        int left_pct  = 254 - c->sep;
        int right_pct = c->sep;
        for (uint32_t i = 0; i < frames && c->pos < c->frames; i++, c->pos++) {
            int32_t l = c->data[c->pos * 2 + 0] * c->vol / 127;
            int32_t r = c->data[c->pos * 2 + 1] * c->vol / 127;
            accum[i * 2 + 0] += l * left_pct  / 254;
            accum[i * 2 + 1] += r * right_pct / 254;
        }
        if (c->pos >= c->frames) { c->active = 0; }
    }

    for (uint32_t i = 0; i < music_frames_available && i < frames; i++) {
        accum[i * 2 + 0] += music_buf[i * 2 + 0] * music_volume / 127;
        accum[i * 2 + 1] += music_buf[i * 2 + 1] * music_volume / 127;
    }

    int16_t *out = malloc((size_t)frames * MIXER_CHANNELS * sizeof(int16_t));
    if (!out) { free(accum); return; }
    for (uint32_t i = 0; i < frames * MIXER_CHANNELS; i++) {
        int32_t v = accum[i];
        if (v > 32767) { v = 32767; }
        if (v < -32768) { v = -32768; }
        out[i] = (int16_t)v;
    }
    free(accum);

    write(pcm_fd, out, (size_t)frames * MIXER_CHANNELS * sizeof(int16_t));
    free(out);
}
```

- [ ] **Step 3: Verify Doom's `sep` (stereo separation) encoding against upstream before trusting the `left_pct`/`right_pct` math above**

```bash
grep -n "sep" upstream/doomgeneric/i_sound.c upstream/doomgeneric/s_sound.c | head -20
```

Confirm `sep` really is a `0..254` range with `128` as center (this is
Chocolate Doom's standard convention, but confirm against THIS fork's
actual source before trusting `mixer.c`'s `254 - c->sep` / `c->sep`
math above — if the range differs, fix the two `left_pct`/`right_pct`
lines in Step 2's `mixer_update` to match the real range, nothing
else needs to change).

- [ ] **Step 4: Write `neoos-shim/i_sound_neoos.c`**

```c
#include "mixer.h"
#include "i_sound.h"
#include "w_wad.h"
#include <string.h>

static boolean sound_init(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    return mixer_init() == 0;
}

static void sound_shutdown(void) {}

static int sound_get_lump_num(sfxinfo_t *sfxinfo) {
    char name[9];
    // "DS" prefix is the standard Doom digital-sound lump naming
    // convention (DSPISTOL, DSSHOTGN, ...) -- every real driver
    // (i_sdlsound.c included) builds the lookup name this same way.
    snprintf(name, sizeof(name), "DS%s", sfxinfo->name);
    return W_GetNumForName(name);
}

static void sound_update(void) {
    mixer_update();
}

static void sound_update_params(int channel, int vol, int sep) {
    mixer_set_sfx_params(channel, vol, sep);
}

static int sound_start(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    int lumpnum = sfxinfo->lumpnum >= 0 ? sfxinfo->lumpnum : sound_get_lump_num(sfxinfo);
    uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC);
    // DMX sound format: uint16 format(=3), uint16 sample_rate,
    // uint32 num_samples, then num_samples bytes of 8-bit unsigned
    // PCM (standard, stable format -- id Software's DMX header,
    // unchanged since original Doom).
    uint16_t rate = data[2] | (data[3] << 8);
    uint32_t num_samples = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);
    mixer_play_sfx(channel, data + 8, num_samples, rate, vol, sep);
    return channel;
}

static void sound_stop(int channel) { mixer_stop_sfx(channel); }
static boolean sound_is_playing(int channel) { return mixer_sfx_playing(channel); }
static void sound_cache(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

static snddevice_t sound_devices[] = { SNDDEVICE_GENMIDI };

sound_module_t DG_sound_module = {
    sound_devices, 1,
    sound_init, sound_shutdown, sound_get_lump_num, sound_update,
    sound_update_params, sound_start, sound_stop, sound_is_playing,
    sound_cache,
};
```

- [ ] **Step 5: Confirm how upstream selects the active `sound_module_t` before this can link**

```bash
grep -n "sound_module\|DG_sound_module" upstream/doomgeneric/i_sound.c
```

If upstream's `i_sound.c` selects its active module from a static
array (checking each candidate's `Init` return value in turn, the
common Chocolate Doom pattern), `DG_sound_module` must be added to
that array — find the array (likely named `sound_modules[]`) and add
`&DG_sound_module` to it, guarded so it does not depend on any
excluded file (`i_sdl*.c` etc. — Task 1's exclusion list). If instead
upstream expects a single hardcoded module pointer already named
`DG_sound_module` with no array (matching this symbol's name exactly),
no change to `i_sound.c` is needed at all — this file's existence and
correct symbol name is already sufficient. Record which case applies
in the commit message for this step.

- [ ] **Step 6: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: sound-module-related undefined references resolved.
Remaining: `music_module_t`/`DG_music_module` (Task 6) and `main`
(Task 7).

- [ ] **Step 7: Commit**

```bash
git add neoos-shim/mixer.h neoos-shim/mixer.c neoos-shim/i_sound_neoos.c
git commit -m "sound: real multi-channel sfx mixer, DG_sound_module"
```

---

### Task 6: Music (`music_module_t` + built-in MUS/MIDI synth)

**Files:**
- Create: `neoos-shim/i_music_neoos.c`

**Interfaces:**
- Consumes: `mixer_add_music_samples`, `mixer_set_music_volume` from
  Task 5's `mixer.h`.
- Consumes: upstream's `mus2mid(MEMFILE *musinput, MEMFILE *midioutput)`
  and `memio.h`'s real API (both verified against upstream's actual
  headers while writing this plan, not reconstructed from memory):
  ```c
  MEMFILE *mem_fopen_read(void *buf, size_t buflen);
  MEMFILE *mem_fopen_write(void);
  void mem_get_buf(MEMFILE *stream, void **buf, size_t *buflen);
  void mem_fclose(MEMFILE *stream);
  ```
- Produces: `music_module_t DG_music_module` — same
  module-registration caveat as Task 5's `DG_sound_module` (Step 2
  below).

- [ ] **Step 1: Write `neoos-shim/i_music_neoos.c`**

```c
#include "mixer.h"
#include "i_sound.h"
#include "mus2mid.h"
#include "memio.h"
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES 32
#define GM_PERCUSSION_CHANNEL 9   // MIDI channels are 0-indexed; "channel 10" is index 9

struct voice {
    int active;
    int channel;
    double freq;       // Hz, from MIDI note number
    double phase;
    int waveform;       // 0=square, 1=triangle, 2=noise
    int velocity;        // 0-127
};
static struct voice voices[MAX_VOICES];

struct midi_track_state {
    const uint8_t *data;
    size_t len, pos;
    uint32_t next_event_tick;
    int last_status;   // MIDI running status
};

static struct midi_track_state track;
static int song_playing;
static int music_volume = 127;
static int program_per_channel[16];   // GM program number, set by Program Change

static double note_to_freq(int note) {
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

static int waveform_for_program(int channel, int program) {
    if (channel == GM_PERCUSSION_CHANNEL) { return 2; }   // noise
    if (program < 16)      { return 0; }   // piano/organ family -> square
    if (program < 80)      { return 1; }   // strings/pad/brass family -> triangle
    return 0;
}

static void note_on(int channel, int note, int velocity) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) {
            voices[i] = (struct voice){
                .active = 1, .channel = channel,
                .freq = note_to_freq(note), .phase = 0,
                .waveform = waveform_for_program(channel, program_per_channel[channel]),
                .velocity = velocity,
            };
            return;
        }
    }
    // All voices busy: drop the note (simplest correct behavior --
    // spec section 3.6 explicitly does not promise voice-stealing).
}

static void note_off(int channel, int note) {
    double f = note_to_freq(note);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].active && voices[i].channel == channel && voices[i].freq == f) {
            voices[i].active = 0;
        }
    }
}

// Reads one MIDI event from `track` and applies it (note on/off,
// program change; all other event types skipped). Returns the delta
// time (in MIDI ticks) before the NEXT event, or -1 at end of track.
// Standard MIDI variable-length-quantity delta time + running status
// -- well-established, stable format, not upstream-specific.
static int32_t read_one_midi_event(void) {
    if (track.pos >= track.len) { return -1; }

    uint32_t delta = 0;
    uint8_t b;
    do {
        b = track.data[track.pos++];
        delta = (delta << 7) | (b & 0x7F);
    } while (b & 0x80);

    uint8_t status = track.data[track.pos];
    if (status & 0x80) { track.pos++; track.last_status = status; }
    else { status = track.last_status; }   // running status: reuse last

    uint8_t type = status & 0xF0;
    uint8_t channel = status & 0x0F;

    if (type == 0x90) {   // note on (velocity 0 == note off, per spec)
        uint8_t note = track.data[track.pos++];
        uint8_t vel  = track.data[track.pos++];
        if (vel == 0) { note_off(channel, note); } else { note_on(channel, note, vel); }
    } else if (type == 0x80) {   // note off
        uint8_t note = track.data[track.pos++];
        track.pos++;   // release velocity, unused
        note_off(channel, note);
    } else if (type == 0xC0) {   // program change
        program_per_channel[channel] = track.data[track.pos++];
    } else if (type == 0xB0 || type == 0xA0 || type == 0xE0) {
        track.pos += 2;   // controller/aftertouch/pitch-bend: skip, 2 data bytes
    } else if (type == 0xD0) {
        track.pos += 1;   // channel aftertouch: 1 data byte
    } else if (status == 0xFF) {   // meta event
        track.pos++;   // meta type
        uint32_t len = 0; uint8_t lb;
        do { lb = track.data[track.pos++]; len = (len << 7) | (lb & 0x7F); } while (lb & 0x80);
        track.pos += len;
    }

    return (int32_t)delta;
}

static boolean music_init(void) { return 1; }
static void music_shutdown(void) {}
static void music_set_volume(int vol) { music_volume = vol; mixer_set_music_volume(vol); }
static void music_pause(void) { song_playing = 0; }
static void music_resume(void) { song_playing = 1; }

// The owned copy of one converted MIDI file this module keeps alive
// between RegisterSong and PlaySong/UnRegisterSong.
struct midi_song { uint8_t *data; size_t len; };

static void *music_register_song(void *data, int len) {
    MEMFILE *mus_in = mem_fopen_read(data, (size_t)len);
    MEMFILE *mid_out = mem_fopen_write();
    if (!mus_in || !mid_out || !mus2mid(mus_in, mid_out)) {
        if (mus_in) { mem_fclose(mus_in); }
        if (mid_out) { mem_fclose(mid_out); }
        return NULL;
    }

    void *mid_buf; size_t mid_len;
    mem_get_buf(mid_out, &mid_buf, &mid_len);

    // Copy out before mem_fclose frees mid_out's internal buffer --
    // mem_get_buf hands back a pointer INTO that buffer, not a
    // transferred allocation.
    struct midi_song *song = malloc(sizeof(*song));
    uint8_t *owned = malloc(mid_len);
    if (!song || !owned) {
        free(song); free(owned);
        mem_fclose(mus_in); mem_fclose(mid_out);
        return NULL;
    }
    memcpy(owned, mid_buf, mid_len);
    song->data = owned;
    song->len = mid_len;

    mem_fclose(mus_in);
    mem_fclose(mid_out);
    return song;
}

static void music_unregister_song(void *handle) {
    struct midi_song *song = handle;
    if (song) { free(song->data); free(song); }
}

static void music_play_song(void *handle, boolean looping) {
    (void)looping;
    struct midi_song *song = handle;
    if (!song || song->len < 22) { song_playing = 0; return; }

    // mus2mid always emits a format-0, single-track Standard MIDI
    // File (MUS is inherently one flat event stream, so there is only
    // ever one track to convert into) -- verified against
    // upstream/doomgeneric/mus2mid.c's actual output while writing
    // this plan. Header layout: "MThd"(4) + length(4, big-endian,
    // always 6) + format(2) + ntrks(2) + division(2) = 14 bytes, then
    // "MTrk"(4) + length(4, big-endian) = 8 bytes, THEN the event
    // stream read_one_midi_event expects starts. 14+8=22.
    track.data = song->data + 22;
    track.len  = song->len - 22;
    track.pos = 0;
    track.next_event_tick = 0;
    track.last_status = 0;
    memset(program_per_channel, 0, sizeof(program_per_channel));
    song_playing = 1;
}

static void music_stop_song(void) { song_playing = 0; }
static boolean music_is_playing(void) { return song_playing; }

static void music_poll(void) {
    if (!song_playing) { return; }

    // Advance MIDI events due this tick (35 Doom tics/sec is the
    // engine's fixed rate; MIDI delta-times are handled in a simple
    // fixed ticks-per-quarter-note approximation -- exact tempo-event
    // handling is not implemented, matching spec section 3.6's stated
    // "right notes and rhythm, not sample-accurate timing" scope).
    while (track.next_event_tick == 0) {
        int32_t delta = read_one_midi_event();
        if (delta < 0) { song_playing = 0; return; }
        track.next_event_tick = (uint32_t)delta;
    }
    track.next_event_tick--;

    // Synthesize one tic's worth of samples from all active voices.
    int16_t buf[MIXER_RATE / 35 * MIXER_CHANNELS];
    uint32_t frames = MIXER_RATE / 35;
    memset(buf, 0, sizeof(buf));
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].active) { continue; }
        for (uint32_t i = 0; i < frames; i++) {
            double t = voices[v].phase;
            int16_t s;
            if (voices[v].waveform == 0) {   // square
                s = (t < 0.5 ? 1 : -1) * (voices[v].velocity * 200);
            } else if (voices[v].waveform == 1) {   // triangle
                s = (int16_t)((t < 0.5 ? (t * 4 - 1) : (3 - t * 4)) * voices[v].velocity * 200);
            } else {   // noise (percussion)
                s = (int16_t)(((rand() % 200) - 100) * voices[v].velocity);
            }
            buf[i * 2 + 0] += s / MAX_VOICES;
            buf[i * 2 + 1] += s / MAX_VOICES;
            voices[v].phase += voices[v].freq / MIXER_RATE;
            if (voices[v].phase >= 1.0) { voices[v].phase -= 1.0; }
        }
    }
    mixer_add_music_samples(buf, frames);
}

static snddevice_t music_devices[] = { SNDDEVICE_GENMIDI };

music_module_t DG_music_module = {
    music_devices, 1,
    music_init, music_shutdown, music_set_volume, music_pause, music_resume,
    music_register_song, music_unregister_song, music_play_song,
    music_stop_song, music_is_playing, music_poll,
};
```

- [ ] **Step 2: Confirm how upstream selects the active `music_module_t`**

Same check as Task 5 Step 5, for music:

```bash
grep -n "music_module\|DG_music_module" upstream/doomgeneric/i_sound.c
```

Apply the same either/or handling described there.

- [ ] **Step 3: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: only `main` (Task 7) remains undefined.

- [ ] **Step 4: Commit**

```bash
git add neoos-shim/i_music_neoos.c
git commit -m "music: MUS/MIDI event parser + basic-waveform synth, DG_music_module"
```

---

### Task 7: Entry point, WAD asset, smoke test, manual verification

**Files:**
- Create: `neoos-shim/main.c`
- Create: `assets/doom1.wad` (download, not hand-written)
- Create: `doom.test.json`
- Create: `smoke-test.sh`

**Interfaces:**
- Consumes: everything from Tasks 2–6.
- Produces: `build/doom.nex` — the finished port; nothing downstream
  in this plan consumes it further.

- [ ] **Step 1: Write `neoos-shim/main.c`**

```c
#include "doomgeneric.h"

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
```

(This is the exact boilerplate every doomgeneric platform backend
uses — confirmed against `upstream/doomgeneric/doomgeneric_xlib.c`
while writing this plan.)

- [ ] **Step 2: Fetch the shareware WAD**

```bash
mkdir -p assets
curl -L -o assets/doom1.wad https://distro.ibiblio.org/pub/linux/distributions/slitaz/sources/packages/d/doom1.wad
# Confirm it's the real shareware IWAD, not an HTML error page:
file assets/doom1.wad   # expect: "data" or similar, NOT "HTML document"
ls -la assets/doom1.wad # expect: ~4.2 MB
```

If that URL is unavailable, any mirror of the well-known
`doom1.wad` (MD5 `f0cefca49926d00903cf57551d901abe`, the id Software
1995 shareware release) is equivalent — verify the MD5 matches before
committing it, since a corrupt or substituted WAD fails silently deep
inside the engine rather than at load time.

- [ ] **Step 3: Write `doom.test.json`**

Copy the shape of `neoos-3d-ascii-viewer`'s `av.test.json` (fetch it
from that repo) — every port in this org uses the identical manifest
shape; only the binary name (`doom.nex`) differs.

- [ ] **Step 4: Write `smoke-test.sh`**

Copy `neoos-3d-ascii-viewer`'s `smoke-test.sh` verbatim except the
binary name — it validates ELF format and static linking only
(matching every port's contract; see spec section 4).

```bash
#!/bin/bash
set -e
NEX="$1"
file "$NEX" | grep -q "ELF 64-bit LSB executable" || { echo "FAIL: not an ELF64 executable"; exit 1; }
file "$NEX" | grep -q "statically linked" || { echo "FAIL: not statically linked"; exit 1; }
echo "OK: $NEX is a static ELF64 executable"
```

- [ ] **Step 5: Full build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -60
```

Expected: clean build, `build/doom.nex` produced, no undefined
references remaining.

- [ ] **Step 6: Smoke test**

```bash
make MUSL_DIR=../neoos-musl/build-output smoke-test
```

Expected: `OK: build/doom.nex is a static ELF64 executable`.

- [ ] **Step 7: Manual verification (not automated — spec section 4)**

Boot NeoOS with `build/doom.nex` and `build/doom1.wad` reachable on a
disk image (coordinate with whatever `neoos-os-builder` setup is
available at implementation time — this repo's own build does not
assemble a bootable image, per the established port contract). Confirm:
- The Doom title screen renders correctly (not garbled, not
  letterboxed incorrectly).
- A keypress advances past the title screen.
- Firing a test shot produces an audible sound effect.
- A few seconds of title music are audible (recognizable melody,
  even with the synth's basic waveforms).

- [ ] **Step 8: Commit**

```bash
git add neoos-shim/main.c assets/doom1.wad doom.test.json smoke-test.sh
git commit -m "finish: entry point, bundled shareware WAD, smoke test"
```

---

## Self-Review Notes (from writing this plan)

- **Spec coverage:** §3.1 (layout) -> Task 1. §3.2 (video) -> Task 2.
  §3.3 (input) -> Task 3. §3.4 (timing) -> Task 4. §3.5 (mixer) ->
  Task 5. §3.6 (music) -> Task 6. §3.7 (WAD) -> Task 7 Step 2. §3.8
  (build) -> Task 1 Step 4. §4 (testing) -> Task 7 Steps 6-7. §5
  (migration ordering) matches this plan's task order exactly.
- **Verified-not-guessed facts, explicitly**: `doomgeneric_Create`/
  `doomgeneric_Tick`/`main()` boilerplate, `DG_ScreenBuffer` allocation
  timing, `DOOMGENERIC_RESX`/`RESY` defaults, `doomkeys.h`'s KEY_*
  values, `sfxinfo_t`/`snddevice_t`/`sound_module_t`/`music_module_t`
  struct layouts, `mus2mid`'s signature — all fetched from upstream's
  actual source while writing this plan, not reconstructed from
  memory, matching this project's established standard (set by the
  AC97 driver's ALSA-struct verification) for anything ABI/interface-
  shaped.
- **One remaining verification step, explicitly marked, not silently
  guessed**: whether `sound_module_t`/`music_module_t` registration
  needs an array entry or just the right symbol name (Task 5 Step 5 /
  Task 6 Step 2 — grep the real `i_sound.c` before assuming; this
  fork's actual selection mechanism was not fetched while writing this
  plan). `memio.h`'s `MEMFILE` API and `mus2mid`'s exact output format
  (format-0, single-track, fixed 22-byte header before the event
  stream) WERE fetched and verified while writing this plan — Task 6's
  code uses the real function names and the real, confirmed byte
  offset, not a placeholder.
- **Type consistency:** `mixer_play_sfx`/`mixer_stop_sfx`/
  `mixer_sfx_playing`/`mixer_set_sfx_params`/`mixer_add_music_samples`/
  `mixer_set_music_volume`/`mixer_update`/`mixer_init` are declared in
  Task 5 Step 1's `mixer.h` and used with identical signatures in Task
  5 Step 4's `i_sound_neoos.c` and Task 6 Step 1's `i_music_neoos.c`.
- **No placeholders** in the sense the writing-plans skill forbids
  (vague "add error handling", "similar to Task N", missing code) —
  every step has real code. The two gaps noted above are a different,
  legitimate thing: verified-unknown external facts with a concrete
  command to resolve them, not hand-waved implementation.
