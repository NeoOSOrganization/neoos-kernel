# M1a — Console plumbing (framebuffer, poll/select, PTY) — design

**Date:** 2026-08-31
**Status:** design (brainstormed with user, 2026-08-31). First of three
milestones toward a userspace terminal and PID 1:

- **M1a (this spec):** kernel plumbing — `/dev/fb0`, an in-kernel
  framebuffer boot console, `poll`/`select`, and a PTY subsystem.
- **M2:** `init` as PID 1 (ring 3), an `/etc/inittab` manifest, orphan
  reaping, `reboot(2)`, the test suite run through init.
- **M1b:** the userspace `terminal` process — a broad (xterm-ish) VT
  emulator with scrollback, rendering the NokiaPure font (rasterised to
  PSF2 at build time), reading `/dev/input/event0`, driving a pty.

**Execution order:** M1a → M2 → M1b. NX/W^X (in progress) lands first.

**Context:**
- `boot/boot.asm` — Multiboot2 header (no framebuffer tag today), long
  mode bring-up, `kmain` entry.
- `kernel/dev/vga.c` — the only display driver: writes VGA text cells at
  physical `0xB8000`. Used by the panic path and (mirrored) by the tty.
- `kernel/dev/tty.c` — one global `console_tty`: line discipline
  (canonical mode, echo, edit/ready buffers), `TCGETS`/`TCSETS`/
  `TIOCGWINSZ`, `SIGINT`/`SIGQUIT`, foreground-pgrp tracking.
  `tty_file_ops` (Phase 15) backs `/dev/CONSOLE` and `/dev/TTY`.
- `kernel/dev/input.c` + `kernel/dev/evdev.c` — the input core and
  `/dev/input/event0` (Phase 15). `evdev_client_read` never blocks
  (documented divergence) — it needs `poll`.
- `kernel/fs/file.h` — `struct file_ops` already carries `.poll` (a
  level-triggered readiness check, no blocking) on every fd type.
- `kernel/fs/devfs.c` — a compile-time device table with hierarchical
  lookup for `input/event0` (Phase 15). No runtime registration.
- `kernel/sync/waitq.{c,h}` — `waitq_sleep`, `waitq_sleep_timeout`
  (used by `nanosleep`, `futex`), `waitq_wake_all`.
- `kernel/mm/vma.c`, `kernel/syscall/sys_mem.c` — `mmap` handles
  anonymous and (via demand paging) file regions; no device path.

## Problem

The kernel is the display: `vga.c` pokes `0xB8000` directly, the tty
mirrors writes there, and the panic handler prints there. Userland
reaches the screen only by `write(1, …)` travelling through the tty
into that same poke. There is no framebuffer, no scrollback, no way for
a userland process to own the console.

The target architecture (user's words) is: the kernel boots, starts an
`init` process, and *init* brings up the console — the kernel out of the
printing business except for early boot and panics. A userspace
terminal renders glyphs into a framebuffer and speaks a pty to whatever
runs "inside" it.

That userspace terminal needs three things the kernel does not provide:

1. **A framebuffer it can `mmap`.** No Multiboot2 framebuffer is even
   requested; GRUB leaves us in VGA text mode.
2. **`poll`/`select`.** The terminal waits on two fds at once — the pty
   master and `/dev/input/event0` — and evdev reads never block.
   Neither syscall exists.
3. **A PTY.** There is one hard-coded global tty. A terminal needs to
   allocate a master/slave pair, run programs against the slave, and
   pump bytes through the master.

M1a builds exactly those three, plus the in-kernel framebuffer console
that replaces the VGA-text path for boot and panic output.

## Goals

- **`/dev/fb0`:** request a linear framebuffer at boot; expose it as a
  device that a userland process can `mmap`, with Linux-shaped
  `FBIOGET_VSCREENINFO` / `FBIOGET_FSCREENINFO`.
- **In-kernel framebuffer console:** a dumb glyph renderer (one
  embedded 8×16 font, wrap, `memmove` scroll — no escapes, no
  scrollback) that the panic path and kernel selftests use in place of
  VGA text. Serial mirroring unchanged.
- **`poll` / `select`:** real blocking multiplexors over the existing
  `file_ops.poll`, with a timeout, waking on the polled objects' own
  wait queues.
- **PTY subsystem:** `struct tty` becomes allocatable; `/dev/ptmx`
  allocates a pair and returns the master; `/dev/pts/N` is the slave;
  `devfs_register` makes the `pts` entries dynamic.
- Every piece independently testable headless and gated in
  `REQUIRED_MARKERS`. `make test` behaviour otherwise unchanged.
- All userland-visible shapes (`struct fb_var_screeninfo`, `struct
  pollfd`, `fd_set`, `struct winsize`, the pty ioctls) match Linux
  x86-64. Every deliberate divergence recorded in `docs/stdlib.md`.

## Non-goals

- **The VT/ANSI emulator, scrollback, alternate screen** — M1b, in the
  userspace terminal. M1a's in-kernel console is deliberately dumb.
- **The NokiaPure → PSF pipeline** — M1b.
- **`init` / PID 1 / `/etc/inittab` / `reboot(2)`** — M2. M1a does not
  touch `kmain`'s spawn list.
- **`SIGWINCH` delivery, `TIOCSWINSZ` semantics.** The framebuffer
  terminal is a fixed size; the slave `winsize` is set once at
  creation. `TIOCSWINSZ` stores the value; no signal is sent
  (documented divergence).
- **Multiple virtual terminals** (Alt+F1..F6). One console tty.
- **`epoll`, `ppoll`/`pselect` sigmask, `POLLPRI`, `POLLRDHUP`.** The
  first `poll` supports `POLLIN`/`POLLOUT`/`POLLERR`/`POLLHUP`/
  `POLLNVAL`.
- **A `devpts` filesystem.** `/dev/pts/N` are dynamic devfs entries.
- **Framebuffer acceleration, double buffering, vsync, mode setting
  after boot.** One mode, set by GRUB, mapped write-through/-combining.
- **Removing `vga.c`.** It stays as dead-but-compiled code until M1b
  proves the framebuffer console across a full milestone; then it is
  deleted in an M1b cleanup commit.

## Design

### 1. Framebuffer: request, parse, map

**1.1 Boot header (`boot/boot.asm`).** Add a Multiboot2 framebuffer
tag (type 5) to the header, between the existing tags and the end tag:

```asm
    ; framebuffer request: 1280x800x32, GRUB may substitute
    align 8
    dw 5        ; type = framebuffer
    dw 0        ; flags (0 = required; GRUB still substitutes a mode)
    dd 20       ; size
    dd 1280     ; width  (0 = no preference)
    dd 800      ; height
    dd 32       ; depth  (0 = no preference / text)
```

GRUB then selects a VBE/GOP linear RGB mode and passes a framebuffer
info tag in the MBI. If the request cannot be honoured GRUB falls back
to text mode and omits the tag — see 1.4.

**1.2 MBI parse (`kernel/dev/fb.c`, new; called from `kmain` first,
before serial even).** Walk the MBI tag list for tag type 8
(framebuffer info):

```c
struct fb_info {
    uint64_t phys;        // framebuffer physical base
    uint64_t size;        // pitch * height, page-rounded
    uint32_t pitch;       // bytes per scanline
    uint32_t width, height;
    uint8_t  bpp;         // 32 (only 32bpp packed RGB supported in M1a)
    uint8_t  type;        // MB2: 1 = direct RGB
    struct { uint8_t pos, size; } r, g, b;   // channel bitfields
    void    *virt;        // kernel VA once mapped
    int      present;     // 0 => text-mode fallback
};
extern struct fb_info fb;
```

**1.3 Map (`fb.c`, after paging + physmap are up).** Map
`[fb.phys, fb.phys + fb.size)` into the kernel higher half at a fixed
window (a new `FB_VIRT_BASE` in `paging.h`, above the physmap). Use
write-combining (PAT entry) if the CPU advertises PAT; otherwise
uncached. Set `fb.virt`.

Between 1.2 and 1.3 there is no framebuffer mapping — an *extremely*
early panic (bad MBI, no long mode) can only use serial. That window is
a few hundred instructions and already serial-only today.

**1.4 Text-mode fallback.** If tag 8 is absent, `fb.present = 0` and
`vga.c` remains the console backend exactly as today. The framebuffer
console, `/dev/fb0`, and `fbtest` all degrade: `/dev/fb0` `open`
returns `-ENODEV`, `fbtest` prints `SKIPPED` and exits 0. This keeps a
GRUB that refuses the mode from bricking the boot. (QEMU with `-cdrom`
always honours it; the fallback is for real hardware / odd GRUBs.)

### 2. In-kernel framebuffer console (`kernel/dev/fbcon.c`, new)

A dumb character grid. No escape parsing, no scrollback, no colour
beyond a fixed pair. This is what `serial`-mirrored kernel output and
the **panic** path render through when `fb.present`.

```c
void fbcon_init(void);            // compute cols/rows from fb + font
void fbcon_putc(char c);          // '\n', '\r', '\b', '\t', printable
void fbcon_write(const char *, uint64_t);
void fbcon_clear(void);
```

- **Font:** one embedded 8×16 bitmap, `kernel/dev/font8x16.c` — a
  public-domain VGA font as a `static const uint8_t[256][16]`. ~4 KiB.
  This is the *kernel's* font; M1b's NokiaPure PSF is a userland file
  and unrelated.
- **Glyph blit:** for a 32bpp framebuffer, write `fg`/`bg` packed
  pixels per set/clear font bit. `pitch`-aware.
- **Scroll:** at the bottom row, `memmove` the framebuffer up one cell
  height and clear the last row. O(fb size) per scroll — fine for boot
  logging; M1b's terminal does damage-tracked rendering.
- **Cursor:** a filled cell at the write position, XOR-drawn so it can
  be cheaply erased. Optional for M1a (boot logging doesn't need a
  visible caret); include it, it's five lines.

**Wiring:** `vga.c`'s callers (`kernel/dev/serial.c` mirror,
`kernel/arch/isr.c` panic dump, `tty.c` output sink) call a new
`console_putc` / `console_write` indirection that dispatches to
`fbcon_*` when `fb.present`, else `vga_*`. `vga.h`'s API is untouched;
one new header `kernel/dev/console.h`.

### 3. `/dev/fb0` device

**3.1 devfs entry.** A new compile-time entry `fb0` →
`fb_file_ops` / `fb_open`. `fb_open` returns `-ENODEV` when
`!fb.present`.

**3.2 `file_ops` gains `.mmap`.**

```c
// in struct file_ops
int64_t (*mmap)(struct file_descriptor *f, struct mmap_req *req);
```

`struct mmap_req` carries `{ uint64_t addr_hint, len, prot, flags,
off; uint64_t *out_addr; }`. The default (`vnode_file_ops.mmap`)
returns `-ENODEV` — only `fb0` implements it in M1a. `sys_mmap`
(`kernel/syscall/sys_mem.c`) gains a branch: if `fd >= 0` and the fd's
`file_ops.mmap` is non-null, call it instead of the anonymous path.

**3.3 `fb_mmap`.** Validates `off + len <= fb.size`, `prot` has no
`PROT_EXEC` (W^X — a framebuffer is data), then inserts a VMA over the
requested range whose backing is the *framebuffer physical frames*
(not anonymous, not COW) and maps the PTEs immediately (write-combining,
`PAGE_USER | PAGE_WRITABLE | PAGE_NO_EXECUTE`). A new VMA kind
`VMA_PHYS` records `phys_base` so `munmap` / address-space teardown
does **not** `pmm_free` framebuffer frames.

**3.4 ioctls (`fb_ioctl`).** Linux structs, byte-exact:

```c
struct fb_fix_screeninfo {   // FBIOGET_FSCREENINFO = 0x4602
    char     id[16];         // "neoosfb"
    uint64_t smem_start;     // fb.phys
    uint32_t smem_len;       // fb.size
    uint32_t type;           // 0 = PACKED_PIXELS
    uint32_t type_aux, visual; // 0, 2 = TRUECOLOR
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;    // fb.pitch
    uint64_t mmio_start; uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities, reserved[2];
};

struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_var_screeninfo {   // FBIOGET_VSCREENINFO = 0x4600
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    // ... trailing timing fields, all zero in M1a
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin,
             lower_margin, hsync_len, vsync_len, sync, vmode,
             rotate, colorspace, reserved[4];
};
```

`FBIOPUT_VSCREENINFO` returns `-EINVAL` unless the requested mode
exactly equals the current one (no mode setting — non-goal).

**3.5 read/write.** `fb_read`/`fb_write` do bounded `memcpy` to/from
`fb.virt + f->position`; `fb_lseek` is a plain offset clamp. A fallback
for code that does not `mmap`.

### 4. `poll` / `select`

**4.1 Syscalls.** New NeoOS numbers `SYS_POLL`, `SYS_SELECT`
(`kernel/syscall/syscall_nr.h`, bump `SYS_MAX`). Handlers in
`kernel/syscall/sys_poll.c` (new).

```c
struct pollfd { int fd; short events; short revents; };
int64_t sys_poll(struct syscall_args *);   // (fds, nfds, timeout_ms)
int64_t sys_select(struct syscall_args *); // (nfds, rd, wr, ex, tv)
```

`sys_select` translates the three `fd_set` bitmaps into an internal
pollfd array and calls the same core; on return it writes the bitmaps
back. `fd_set` is Linux's 1024-bit layout; `nfds > FD_SETSIZE` →
`-EINVAL`.

**4.2 Core (`poll_core`).**

```
poll_core(pollfd[] pfd, n, deadline_ticks):
  register a per-thread poll_waiter on every pfd's object waitq   // once
  loop:
    ready = 0
    for i in 0..n:
        m = file_poll(fd_get(pfd[i].fd), pfd[i].events | ERR|HUP)
        pfd[i].revents = m & (pfd[i].events | ERR|HUP|NVAL)
        if pfd[i].revents: ready++
    if ready or now >= deadline: break
    waitq_sleep_timeout(&self->poll_waiter.q, NULL, deadline)   // -EINTR aware
  unregister the poll_waiter from every waitq
  return ready
```

**4.3 The wake hook.** `file_ops` gains:

```c
void (*poll_wait)(struct file_descriptor *f, struct poll_waiter *w);
```

Each pollable driver's `poll_wait` calls `waitq_link(&obj->readers, w)`
(and `&obj->writers` where write-readiness matters) — one or two lines
each for pipe, socket, tty/pts, evdev. `waitq_link` / `waitq_unlink`
(new, in `waitq.c`) attach a *foreign* waiter node to a waitq so that
any `waitq_wake_all` on that queue also wakes `w->q`. The waiter node
is a small embedded struct; `poll_core` holds an array of them on its
kernel stack (bounded by `nfds`, itself capped).

**4.4 Timeout & signals.** `waitq_sleep_timeout` already returns
`-ETIMEDOUT` / `-EINTR`. `poll` with `timeout_ms < 0` blocks
indefinitely; `0` is a non-blocking scan. A signal makes `poll` return
`-EINTR` (no `SA_RESTART` for `poll`, matching Linux).

**4.5 evdev.** `evdev_client_poll` already reports `POLLIN` when the
ring is non-empty; add `evdev`'s `poll_wait` linking `&c->readers`.
The "read never blocks" divergence note in `docs/stdlib.md` gains: "use
`poll`/`select` to wait; they are correct."

**4.6 musl.** `poll`, `select`, `pselect6` in musl map onto
`SYS_POLL` / `SYS_SELECT` through the shim (`pselect6` drops the
sigmask arg — documented). `ppoll` similarly.

### 5. PTY subsystem

**5.1 `struct tty` becomes allocatable (`kernel/dev/tty.c`).**
The line discipline state currently living in the file-scope
`console_tty` moves into:

```c
struct tty {
    struct spinlock lock;          // LOCK_RANK_TTY (new)
    // line discipline
    struct termios termios;
    struct winsize winsize;
    char     edit[TTY_BUF];  uint32_t edit_len;   // canonical assembly
    char     ready[TTY_BUF]; uint32_t ready_head, ready_len;  // to reader
    struct waitq readers;    // blocked in slave read()
    struct waitq poll_readers;
    // session / job control
    int pgrp, sid;
    // backend
    const struct tty_backend *backend;
    void *backend_priv;
    // output sink
    char     outq[TTY_BUF]; uint32_t out_head, out_len;   // pty only
    struct waitq out_readers;   // master read() / poll
};

struct tty_backend {
    // slave wrote bytes (post output-processing) -> where do they go?
    void (*output)(struct tty *, const char *, uint32_t);
};
```

Two backends:

- **`console_backend`** — `output` → `console_write` (fbcon + serial).
  Input arrives via `tty_input_char` from the kernel input core, as
  today. `console_tty` is a single static `struct tty` with this
  backend; `/dev/CONSOLE` and `/dev/TTY` keep pointing at it. **No
  behaviour change** for the existing suite.
- **`pty_backend`** — `output` → append to `tty->outq`, wake
  `out_readers`. Input arrives from master `write()`.

**5.2 `/dev/ptmx` (`kernel/dev/pty.c`, new).**

- Fixed pool: `static struct pty { struct tty slave; int master_open,
  slave_open; ... } ptys[PTY_MAX];  // PTY_MAX = 16`
- `ptmx_open`: find a free slot, init `slave` with `pty_backend`,
  default `termios` (canonical, echo, `ONLCR`, `ISIG`), `winsize`
  `{0,0,0,0}`. `f->ops = ptm_file_ops`, `f->priv = &ptys[i]`.
  `devfs_register("pts/<i>", &pts_file_ops, &ptys[i])`.
- **master ops** (`ptm_file_ops`):
  - `read` → drains `slave.outq` (what the program printed, ONLCR
    applied), blocks on `out_readers` unless `O_NONBLOCK`.
  - `write` → feeds bytes into the slave line discipline exactly as
    `tty_input_char` does (echo, canonical assembly, `^C`→`SIGINT` to
    `slave.pgrp`).
  - `poll` → `POLLIN` when `outq` non-empty, `POLLOUT` always.
  - `ioctl(TIOCGPTN)` → slot index. `ioctl(TIOCSWINSZ)` → store into
    `slave.winsize` (no `SIGWINCH` — non-goal). `ioctl(TIOCGWINSZ)` →
    return it.
  - `close` → `master_open = 0`; if slave still open, its reads get
    EOF and its fg pgrp gets `SIGHUP`. When both closed,
    `devfs_unregister("pts/<i>")`, slot freed.
- `grantpt`/`unlockpt` (musl) → `ioctl`s that are no-ops returning 0
  (documented: NeoOS has no pts permission model).

**5.3 `/dev/pts/N` (`pts_file_ops`).** Same read/write/poll/ioctl as
`tty_file_ops` today, but bound to `pty->slave` instead of
`console_tty`. `open` requires the pty slot to exist (else `-ENOENT`
via devfs) and marks `slave_open`. `TCGETS`/`TCSETS`/`TIOCGWINSZ`
operate on the slave. `close` → `slave_open = 0`; master `read` gets
EOF.

**5.4 `devfs_register` (`kernel/fs/devfs.c`).**

```c
int  devfs_register(const char *path, const struct file_ops *ops, void *priv);
void devfs_unregister(const char *path);
```

A small dynamic table (`DEVFS_DYN_MAX = 32`) checked after the static
table in `devfs_lookup` / `devfs_readdir`. `path` may contain one `/`
(`"pts/3"`); the `pts` directory node itself is a static entry so an
empty `/dev/pts` still lists. Guarded by a new `LOCK_RANK_DEVFS`.

**5.5 Controlling terminal.** Minimal: when a session leader `open`s a
pts (or `/dev/CONSOLE`) without `O_NOCTTY` and has no controlling tty,
it becomes the controlling tty and its `sid`/`pgrp` are recorded on the
`struct tty`. `TIOCSCTTY` forces it. Enough for `^C` to reach the right
process group; full POSIX session teardown is M2/M3 territory.

### 6. Lock ranks

New `kernel/sync/lock.h` slots, each below `LOCK_RANK_INPUT` is *not*
required — order by acquisition:

- `LOCK_RANK_TTY` — a `struct tty`'s `lock`. Taken from the input core
  (`tty_input_char`) and from syscalls; never held across
  `console_write`/`waitq_*` (same discipline as the input lock).
- `LOCK_RANK_PTY` — the `ptys[]` pool allocation lock. Taken briefly
  around slot alloc/free only; ranks above `TTY` (alloc takes the pool
  lock, then inits a slave's tty lock).
- `LOCK_RANK_DEVFS` — the dynamic devfs table. Leaf-ish; taken during
  `open`/`register`, ranks with the filesystem locks.
- `poll_waiter.q` reuses `LOCK_RANK_WAITQ`.

Every rank gets a one-paragraph rationale in `lock.h`, per project
convention; an out-of-order acquisition panics the boot.

### 7. New syscalls / ABI surface

| # | Name | Linux analogue | Notes |
|---|------|----------------|-------|
| N | `poll` | `poll` | `(fds, nfds, timeout_ms)`. `POLLIN/OUT/ERR/HUP/NVAL`. |
| N+1 | `select` | `select` | `(nfds, rd, wr, ex, timeval*)`. `nfds ≤ 1024`. |

`mmap` gains a device-fd path — no new number. `file_ops` gains
`.mmap` and `.poll_wait`. `ioctl` request numbers added: `TIOCGPTN`,
`TIOCSPTLCK`, `TIOCSWINSZ`, `TIOCSCTTY`, `FBIOGET_VSCREENINFO`,
`FBIOGET_FSCREENINFO`, `FBIOPUT_VSCREENINFO` — all Linux values.

## Data flow (the terminal, once M1b exists — shown for context)

```
keyboard IRQ ─▶ input core ─▶ evdev ring ──(poll+read)──▶ terminal
                                                             │ VT parse in
                                                             ▼
                                        write(pty master) ─▶ slave line discipline
                                                             ▼
                                                     shell/program reads stdin
                                                             │ writes stdout
                                                             ▼
                                        slave output ──▶ pty master outq
                                                             │ (poll+read)
                                                             ▼
                                        terminal VT parse out ─▶ glyphs ─▶ mmap'd /dev/fb0
```

In M1a none of the terminal exists; the tests exercise each seam
directly.

## Testing

No host unit tests (bare metal). Each component ships a userland binary
verified by `make test` (headless QEMU, 4 CPUs) and the parallel
gauntlet. New `REQUIRED_MARKERS`:

- **`[fbtest] ALL PASSED`** — `open("/dev/fb0")`, `FBIOGET_FSCREENINFO`
  + `FBIOGET_VSCREENINFO` sanity (xres·bpp matches pitch), `mmap` the
  whole framebuffer, write a known 32-bit value at three offsets, read
  it back through both the mapping and a separate `pread`, `munmap`.
  On the text-mode fallback: prints `[fbtest] SKIPPED`, still a pass.
- **`[polltest] ALL PASSED`** — pipe pair: `poll(POLLIN, 50ms)` →
  times out (`revents == 0`); child writes; `poll` → `POLLIN`; drain;
  close write end; `poll` → `POLLHUP`. Then `select` over the same fd.
  Then evdev: `poll(POLLIN, 0)` → not ready; inject a key via the
  test hook; `poll` → `POLLIN`; `read` succeeds.
- **`[ptytest] ALL PASSED`** — `open("/dev/ptmx")`, `TIOCGPTN`,
  `ptsname`-style `/dev/pts/N` open, `fork`; child `dup2`s the slave to
  0/1/2 and `execve`s a trivial `catn` helper (reads a line, writes it
  back). Parent writes `"hi\n"` to the master, `read`s `"hi\r\n"`
  (ONLCR). `TCGETS` on the slave succeeds; on a pipe fd returns
  `-ENOTTY`. `write` `"\x03"` (‌^C) to the master → child dies of
  `SIGINT` (parent checks `WIFSIGNALED`).
- **Kernel:** `fbcon_selftest` — draw and read back a glyph cell;
  `[fbcon] selftest passed`. `pty_selftest` — allocate a pair, push a
  line master→slave→reader and back, free; `[pty] selftest passed`.

`make test` marker-scrape and the panic check are unaffected: kernel
output still goes to serial, and the console tty still exists.

**Risk-specific checks:**
- Force the text-mode fallback (`qemu -vga std` without the multiboot
  framebuffer honoured is hard to arrange; instead a `FB_FORCE_TEXT=1`
  compile flag) and confirm the full suite + panic still work.
- A panic *after* `fbcon_init` renders legibly (manual: a temporary
  `PANIC()` in `kmain`, screenshot, revert).
- Gauntlet ×3 after the PTY task (the tty refactor is the highest-risk
  change): `PGAUNTLET PASSED: 15/15` three times.

## Documentation at milestone close

- **`docs/stdlib.md`:** new sections — `/dev/fb0` and `<linux/fb.h>`
  (the two ioctls, `mmap`, the "no mode setting" divergence);
  `poll`/`select` (the supported flag subset, no `epoll`/`ppoll`
  sigmask, `select` `FD_SETSIZE` cap); the PTY interface (`/dev/ptmx`,
  `/dev/pts/N`, `grantpt`/`unlockpt` no-ops, `TIOCSWINSZ` stores but
  sends no `SIGWINCH`). Update the evdev "read never blocks" note to
  point at `poll`.
- **`docs/abi-compatibility.md`:** refresh to close-of-M1a — `poll`/
  `select` implemented (subset); fbdev present (subset); pty present
  (subset). What a ported app still hits: no `epoll`, no `SIGWINCH`,
  no mode setting.
- **`README.md`:** drivers list gains the framebuffer console and pty;
  "where it's going" note.
- **This spec** committed under `docs/superpowers/specs/`.

## Risks

- **The `struct tty` refactor** touches the one code path every
  userland test uses for output. Mitigation: `console_tty` keeps its
  exact semantics — the refactor is "same fields, now in a struct with
  a backend vtable"; the console backend is a straight extraction.
  Gauntlet ×3 gates the commit.
- **`fbcon` is the panic path.** A bug there hides the next bug.
  Mitigation: `fbcon_init` runs first; a `fbcon_selftest` at boot; the
  serial mirror is always the source of truth for `make test`.
- **`poll_wait` / `waitq_link` foreign-waiter machinery** is new
  synchronisation. Mitigation: the fallback is the "simpler first cut"
  (bounded re-scan on a fixed sleep) if the foreign-waiter approach
  shows races under the gauntlet — same syscall surface, worse
  latency, ~1/3 the code.
- **Write-combining PAT setup** on the framebuffer — getting the PAT
  MSR wrong slows every framebuffer write ~10×. Mitigation: fall back
  to plain uncached (`PCD`) if PAT is not cleanly available; correct,
  just slower, and M1a does no heavy drawing.
- **Multiboot2 framebuffer request refused by a real GRUB.** The
  text-mode fallback (1.4) keeps the boot alive; only the framebuffer
  features degrade.

## Self-review

**Placeholder scan:** the `struct fb_var_screeninfo` trailing timing
fields are "all zero in M1a" — that is a stated value, not a TODO. The
`catn`/`catn` helper for `ptytest` is named consistently. No bare
"add error handling" / "write tests".

**Internal consistency:** `console_write` (the fbcon/vga indirection)
is used in §2, §5.1 (`console_backend.output`), and the testing
section — same name throughout. `poll_wait` / `waitq_link` named
identically in §4.3 and Risks. `PTY_MAX = 16`, `DEVFS_DYN_MAX = 32`,
`FD_SETSIZE = 1024` stated once each.

**Scope check:** four components, but they share one theme (the
console the userspace terminal will need) and one integration point
(the tty). Each is a separable implementation-plan task. Not
decomposed further because M1a's value is exactly "the terminal has
something to build on" — splitting it defers that with no benefit.

**Ambiguity check:** "the panic path renders through fbcon" — means
`console_putc` dispatches to `fbcon_putc` when `fb.present`, made
explicit in §2. "`poll` supports a subset" — the exact flag list is in
Non-goals and §4.1. "`TIOCSWINSZ` is stubbed" — stores the value,
sends no signal, returns 0; stated in §5.2 and Non-goals.
