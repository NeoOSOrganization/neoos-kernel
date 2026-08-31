# M1a — Console plumbing (framebuffer, poll/select, PTY) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the kernel a linear framebuffer (`/dev/fb0`, mmap-able), `poll`/`select`, and a PTY subsystem — the plumbing a userspace terminal (M1b) and `init` (M2) will stand on.

**Architecture:** A Multiboot2 framebuffer replaces VGA text mode; a dumb in-kernel `fbcon` renders boot/panic output through it while serial stays a byte-for-byte mirror. `poll`/`select` are blocking multiplexors over the existing `file_ops.poll`, waking on the polled objects' own wait queues via a new `poll_wait` hook. `struct tty` becomes an allocatable object with a backend vtable; `/dev/ptmx` allocates a master/slave pair, `/dev/pts/N` is the slave, and `devfs_register` makes the `pts` entries dynamic.

**Tech Stack:** C (freestanding, `-mcmodel=kernel`, no SSE in kernel), NASM (`boot/boot.asm`, `kernel/arch/ap_trampoline.asm`), x86-64, GNU Make, headless QEMU + serial-log assertions, the parallel gauntlet (`.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh`, CONC=2 on a loaded host).

**Spec:** `docs/superpowers/specs/2026-08-31-m1a-console-plumbing-design.md`

## Global Constraints

- **No host unit tests.** Every test is a kernel selftest or a userland binary, verified by `make test` (headless QEMU, 4 CPUs) and, after each task, the parallel gauntlet reaching `PGAUNTLET PASSED: 15/15`. `make test` now powers off in ~11s (commit `733db3e`); `BOOT_TIMEOUT=60` is the hang detector.
- **`make test` compiles with `-DNEOOS_TEST_HOOKS`; a plain `make iso` (what the gauntlet builds) does not.** Any test-only kernel path must return `-ENOSYS` in the production build, and any userland test that calls it must tolerate `-ENOSYS` (see `smptest.c:check_migration_counter`). The gauntlet runs `make clean-kernel iso disk-image` first.
- **Lock ranks are enforced at runtime.** Every new lock gets a `LOCK_RANK_*` slot in `kernel/sync/lock.h` with a one-paragraph rationale; acquiring out of ascending order panics the boot. Current ranks of interest: `LOCK_RANK_WAITQ` 14, `LOCK_RANK_RUNQUEUE` 15, `LOCK_RANK_FDTABLE` 16, `LOCK_RANK_DRIVER` 8 (today's `console_tty.lock`).
- **The ABI is not ours.** `struct fb_var_screeninfo`/`fb_fix_screeninfo`, `struct pollfd`, `fd_set`, `struct winsize`, the pty `TIOC*` and `FBIO*` ioctl numbers all match Linux x86-64 values and layouts. Every deliberate divergence is recorded in `docs/stdlib.md` and, at milestone close, `docs/abi-compatibility.md`.
- **Kernel `#include`s are relative to `kernel/`** (e.g. `#include "dev/fb.h"`). `phys_to_virt()` is valid only after `physmap_installed` (set in `paging_init`).
- **W^X is enforced** (commits `9cabab4`..`d872e7c`): no page is mapped writable + executable. A framebuffer mapping is `PROT_READ|PROT_WRITE` only — never `PROT_EXEC`.
- **Work on `main`, one commit per task**, trailer:
  `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01CNR4gEkyMq6qhFWfxt3KXE`.
- **Never run `make` while the gauntlet runs** — shared `build/`. Regenerate disk images (`rm -f build/disk.img build/disk2.img`) before a bare `make test` if a prior write selftest mutated them.

## File Structure

| File | Responsibility |
|---|---|
| `boot/boot.asm` | +Multiboot2 framebuffer request tag (type 5) |
| `kernel/dev/fb.h` / `fb.c` (new) | framebuffer detection (MBI tag 8 parse), mapping, `struct fb_info`, `/dev/fb0` `file_ops` (mmap + fbdev ioctls + byte r/w) |
| `kernel/dev/font8x16.c` (new) | one embedded 8×16 bitmap font, `const uint8_t font8x16[256][16]` |
| `kernel/dev/fbcon.h` / `fbcon.c` (new) | dumb glyph-grid console: `fbcon_putc`/`fbcon_write`/`fbcon_clear`, wrap, memmove-scroll, no escapes |
| `kernel/dev/console.h` / `console.c` (new) | `console_putc`/`console_write` — dispatch to `fbcon_*` when `fb.present`, else `vga_*` |
| `kernel/mm/paging.h` / `paging.c` | `FB_VIRT_BASE`; `paging_map_range(virt, phys, len, flags)` helper for the framebuffer |
| `kernel/mm/vma.h` / `vma.c` | `VMA_PHYS` kind (backing = fixed phys, never `pmm_free`d); honoured in fault + munmap + teardown |
| `kernel/fs/file.h` / `file.c` | `file_ops.mmap`, `file_ops.poll_wait`; `file_mmap`/`file_poll_wait` dispatch; `ops_complete` updated |
| `kernel/syscall/sys_mem.c` | `sys_mmap` device-fd branch → `file_mmap` |
| `kernel/syscall/syscall_nr.h` / `syscall.c` / `syscall_internal.h` | `SYS_POLL` 67, `SYS_SELECT` 68, `SYS_MAX` 69 |
| `kernel/syscall/sys_poll.c` (new) | `sys_poll`, `sys_select`, `poll_core`, `struct pollfd`, `fd_set` handling |
| `kernel/sync/waitq.h` / `waitq.c` | `struct poll_waiter`; `waitq_link`/`waitq_unlink` (foreign-waiter attach) |
| `kernel/ipc/pipe.c`, `kernel/net/socket.c`, `kernel/dev/tty.c`, `kernel/dev/evdev.c` | one `poll_wait` each |
| `kernel/dev/tty.h` / `tty.c` | `struct tty` allocatable; `struct tty_backend`; `console_backend`; every function takes `struct tty *` |
| `kernel/dev/pty.h` / `pty.c` (new) | `/dev/ptmx` open, `ptm_file_ops` (master), `pts_file_ops` (slave), the `ptys[16]` pool, `pty_backend` |
| `kernel/fs/devfs.h` / `devfs.c` | `devfs_register(path, ops, priv)` / `devfs_unregister(path)`; dynamic table checked after the static one; static `pts` dir entry |
| `kernel/sync/lock.h` | `LOCK_RANK_TTY`, `LOCK_RANK_PTY`, `LOCK_RANK_DEVFS` |
| `kernel/kernel.c` | `fb_init()` first thing in `kmain`; `fbcon_init()` right after; `pty_init()` with the other subsystem inits |
| `Makefile` | new `REQUIRED_MARKERS`; `kernel/mm`, `kernel/dev`, `kernel/syscall` already in `KERNEL_DIRS` (auto-discovered) |
| `userland/fbtest.c`, `userland/polltest.c`, `userland/ptytest.c`, `userland/catn.c` (new) | + Makefile build rules + `mcopy` into the disk image + `$(DISK_IMG)` prereqs |
| `lib/syscall.c`, `lib/include/*` | `poll`, `select` wrappers; `struct pollfd`, `fd_set`, `FBIO*` in headers |
| `third_party/shim/` (musl arch dir) | `poll`/`select`/`pselect6`/`ppoll` → `SYS_POLL`/`SYS_SELECT` |
| `docs/stdlib.md`, `docs/abi-compatibility.md` | fb, poll/select, pty sections + divergences |

---

## Task 1: Framebuffer detection, mapping, and the in-kernel console

**Files:**
- Modify: `boot/boot.asm` (Multiboot2 header, after the existing tags, before the end tag)
- Create: `kernel/dev/fb.h`, `kernel/dev/fb.c`, `kernel/dev/font8x16.c`, `kernel/dev/fbcon.h`, `kernel/dev/fbcon.c`, `kernel/dev/console.h`, `kernel/dev/console.c`
- Modify: `kernel/mm/paging.h`, `kernel/mm/paging.c` (`FB_VIRT_BASE`, `paging_map_range`)
- Modify: `kernel/kernel.c` (call `fb_init` + `fbcon_init` early), `kernel/dev/tty.c` (`vga_putc` → `console_putc`), `kernel/arch/isr.c` (`vga_print_string` → `console_write`), `kernel/kernel.c:91` (`vga_print_string` → `console_write`)
- Modify: `Makefile` (`REQUIRED_MARKERS`: `"[fb] framebuffer"`, `"[fbcon] selftest passed"`)

**Interfaces:**
- Produces:
  ```c
  // fb.h
  struct fb_info {
      uint64_t phys, size;          // size = pitch*height, page-rounded
      uint32_t pitch, width, height;
      uint8_t  bpp;                 // 32 only in M1a
      struct { uint8_t pos, size; } r, g, b;
      volatile uint8_t *virt;       // 0 until fb_map()
      int      present;             // 0 => text-mode fallback
  };
  extern struct fb_info fb;
  void fb_init(void *mbi);          // parse MBI tag 8; sets fb.present
  void fb_map(void);                // map [fb.phys, fb.phys+fb.size); sets fb.virt

  // fbcon.h
  void fbcon_init(void);            // cols/rows from fb + font; clears
  void fbcon_putc(char c);
  void fbcon_write(const char *s, uint64_t n);
  void fbcon_clear(void);
  void fbcon_selftest(void);

  // console.h
  void console_putc(char c);        // -> fbcon if fb.present else vga
  void console_write(const char *s, uint64_t n);

  // paging.h
  #define FB_VIRT_BASE 0xFFFFC00000000000ULL
  int paging_map_range(uint64_t virt, uint64_t phys, uint64_t len, uint64_t flags);

  // font8x16.c
  extern const uint8_t font8x16[256][16];
  ```

- [ ] **Step 1: Multiboot2 framebuffer request tag**

In `boot/boot.asm`, inside `section .multiboot_header`, between `header_start`'s checksum and the `; required end tag`:
```asm
    ; --- framebuffer request (type 5) ---
    align 8
fb_tag_start:
    dw 5            ; MULTIBOOT_HEADER_TAG_FRAMEBUFFER
    dw 0            ; flags: 0 = required (GRUB still substitutes a mode)
    dd fb_tag_end - fb_tag_start
    dd 1280        ; width  hint
    dd 800         ; height hint
    dd 32          ; depth  hint
fb_tag_end:
```
The end tag already follows. Build (`make build`) and confirm it links.

- [ ] **Step 2: `fb_init` — parse the MBI, `fb.present`**

`kernel/dev/fb.c`: walk the Multiboot2 tag list (`mbi` points at a `uint32_t total_size, reserved` header, then 8-byte-aligned tags each `{uint32_t type, size; ...}`). Tag type 8 = framebuffer:
```c
struct mb2_fb_tag {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t  bpp, fb_type, reserved;
    // followed by colour-info union; for fb_type 1 (RGB):
    uint8_t  r_pos, r_size, g_pos, g_size, b_pos, b_size;
};
```
Fill `fb` from it; require `bpp == 32 && fb_type == 1`, else `fb.present = 0`. Print `[fb] framebuffer 1280x800x32 @ 0x<phys>` or `[fb] no framebuffer tag -- VGA text fallback`.

- [ ] **Step 3: `paging_map_range` + `fb_map`**

`paging.c`: a thin loop over `paging_map_into(p4_table, virt+i, phys+i, flags)` for `i` in `[0, len)` step 4096, returning `<0` on the first failure. `fb_map()`: `paging_map_range(FB_VIRT_BASE, fb.phys, fb.size, PAGE_PRESENT|PAGE_WRITABLE|PAGE_NO_EXECUTE)` (uncached is fine for M1a — no PAT), set `fb.virt = (void *)FB_VIRT_BASE`. Call `fb_init(mbi)` as the **first** line of `kmain` after `serial_init()`, and `fb_map()` right after `paging_init()` (physmap must exist for `paging_map_into`'s table walk).

- [ ] **Step 4: the font**

`kernel/dev/font8x16.c`: paste a public-domain 8×16 VGA font as `const uint8_t font8x16[256][16] = { ... }` (the classic IBM VGA 8×16 ROM font; ~4 KiB of initialiser). Glyph `c`, row `y`: bit `7-x` of `font8x16[c][y]` set → foreground pixel.

- [ ] **Step 5: `fbcon` — the dumb renderer**

`kernel/dev/fbcon.c`:
```c
static uint32_t cols, rows, cx, cy;
#define FG 0x00cccccc
#define BG 0x00000000

static void put_glyph(uint32_t gx, uint32_t gy, char ch) {
    const uint8_t *g = font8x16[(uint8_t)ch];
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t px = gx*8 + x, py = gy*16 + y;
            uint32_t *p = (uint32_t *)(fb.virt + py*fb.pitch + px*4);
            *p = (g[y] >> (7-x)) & 1 ? FG : BG;
        }
}
static void scroll(void) {
    uint8_t *base = (uint8_t *)fb.virt;
    uint64_t row_bytes = fb.pitch * 16;
    for (uint64_t i = 0; i < (uint64_t)(rows-1)*row_bytes; i++) base[i] = base[i + row_bytes];
    for (uint64_t i = (uint64_t)(rows-1)*row_bytes; i < (uint64_t)rows*row_bytes; i++) base[i] = 0;
}
void fbcon_putc(char c) {
    if (!fb.present) return;
    if (c == '\n')      { cx = 0; cy++; }
    else if (c == '\r') { cx = 0; }
    else if (c == '\b') { if (cx) cx--; }
    else if (c == '\t') { cx = (cx + 8) & ~7u; }
    else { put_glyph(cx, cy, c); cx++; }
    if (cx >= cols) { cx = 0; cy++; }
    if (cy >= rows) { scroll(); cy = rows - 1; }
}
```
`fbcon_init`: `cols = fb.width/8; rows = fb.height/16; cx = cy = 0; fbcon_clear();`. `fbcon_clear`: zero `[fb.virt, fb.virt+fb.size)`. `fbcon_selftest`: `put_glyph(0,0,'A')`, read back the pixel at `(2,2)` (a set bit of 'A'), assert `== FG`, then `fbcon_clear()`; print `[fbcon] selftest passed` or `... FAILED`.

- [ ] **Step 6: `console` indirection + wire it**

`kernel/dev/console.c`:
```c
void console_putc(char c)                     { if (fb.present) fbcon_putc(c); else vga_putc(c); }
void console_write(const char *s, uint64_t n) { for (uint64_t i = 0; i < n; i++) console_putc(s[i]); }
```
Replace: `tty.c:102` `vga_putc(c)` → `console_putc(c)`; `tty.c:120` loop → `console_putc(s[i])`; `isr.c:51,64` `vga_print_string(X)` → `console_write(X, __builtin_strlen(X))`; `kernel.c:91` likewise. Call `fbcon_init()` in `kmain` right after `fb_map()`. **Do not remove `vga.c`** — it's the `!fb.present` fallback and gets deleted in an M1b cleanup.
Add `fbcon_selftest()` to `kmain` after `fbcon_init()`.

- [ ] **Step 7: build, boot, verify**

`make test`. Expect: serial log **unchanged** (all existing markers), plus `[fb] framebuffer 1280x800x32 ...` and `[fbcon] selftest passed`. The framebuffer content isn't checked headlessly here (that's Task 2's `fbtest`), but `-display none` still gives QEMU the framebuffer so `fbcon` writes don't fault.
Add the two markers to `REQUIRED_MARKERS`.

- [ ] **Step 8: gauntlet + commit**

`bash .superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 2` → `PGAUNTLET PASSED: 15/15`. Commit `"M1a: Multiboot2 framebuffer + in-kernel fbcon; console indirection"`.

---

## Task 2: `/dev/fb0` — mmap and the fbdev ioctls

**Files:**
- Modify: `kernel/fs/file.h` (`file_ops.mmap`), `kernel/fs/file.c` (`file_mmap` dispatch, `vnode_file_ops.mmap = 0`, `ops_complete`)
- Modify: `kernel/mm/vma.h` (`VMA_PHYS`), `kernel/mm/vma.c` (`vma_map_phys`, fault + munmap + teardown honour it)
- Modify: `kernel/syscall/sys_mem.c` (`sys_mmap` device-fd branch)
- Modify: `kernel/dev/fb.c` (add `fb_file_ops`, `fb_open`), `kernel/fs/devfs.c` (`fb0` static entry), `kernel/dev/fb.h`
- Create: `userland/fbtest.c` (+ Makefile rule + disk copy)
- Modify: `Makefile` (`REQUIRED_MARKERS`: `"[fbtest] ALL PASSED"`), `lib/include/sys/mman.h` or a new `lib/include/linux/fb.h`

**Interfaces:**
- Consumes: `struct fb_info fb` (Task 1)
- Produces:
  ```c
  // file.h
  struct mmap_req { uint64_t addr, len, prot, flags, off; uint64_t out_addr; };
  int64_t (*mmap)(struct file_descriptor *f, struct mmap_req *req);  // in file_ops
  int64_t file_mmap(struct file_descriptor *f, struct mmap_req *req);

  // vma.h
  #define VMA_PHYS 0x40   // OR-ed into flags stored on the vma; backing is fixed phys
  int64_t vma_map_phys(struct process *p, uint64_t phys, uint64_t len, uint32_t prot);

  // linux/fb.h (userland)
  #define FBIOGET_VSCREENINFO 0x4600
  #define FBIOGET_FSCREENINFO 0x4602
  #define FBIOPUT_VSCREENINFO 0x4601
  struct fb_bitfield { uint32_t offset, length, msb_right; };
  struct fb_var_screeninfo { uint32_t xres, yres, xres_virtual, yres_virtual,
      xoffset, yoffset, bits_per_pixel, grayscale;
      struct fb_bitfield red, green, blue, transp;
      uint32_t nonstd, activate, height, width, accel_flags,
      pixclock, left_margin, right_margin, upper_margin, lower_margin,
      hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4]; };
  struct fb_fix_screeninfo { char id[16]; uint64_t smem_start; uint32_t smem_len,
      type, type_aux, visual; uint16_t xpanstep, ypanstep, ywrapstep;
      uint32_t line_length; uint64_t mmio_start; uint32_t mmio_len, accel;
      uint16_t capabilities, reserved[2]; };
  ```

- [ ] **Step 1: failing test — `userland/fbtest.c`**

```c
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/fb.h>
extern long mmap_raw(unsigned long addr, unsigned long len, int prot, int flags);
extern int  ioctl(int fd, unsigned long req, void *arg);
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 0x01

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { printf("[fbtest] SKIPPED: /dev/fb0 open %d\n", fd); return 0; }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) != 0) { printf("[fbtest] FAILED: GET_VSCREENINFO\n"); return 1; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) != 0) { printf("[fbtest] FAILED: GET_FSCREENINFO\n"); return 1; }
    if (v.bits_per_pixel != 32 || v.xres == 0 || f.line_length < v.xres * 4) {
        printf("[fbtest] FAILED: geometry %ux%ux%u pitch %u\n", v.xres, v.yres, v.bits_per_pixel, f.line_length);
        return 1;
    }

    unsigned long len = (unsigned long)f.line_length * v.yres;
    long m = mmap_raw(0, len, PROT_READ | PROT_WRITE, MAP_SHARED);
    if (m < 0) { printf("[fbtest] FAILED: mmap %ld\n", m); return 1; }
    volatile unsigned int *px = (volatile unsigned int *)(unsigned long)m;
    px[0] = 0x00abcdef; px[v.xres + 1] = 0x00123456;
    if (px[0] != 0x00abcdef || px[v.xres + 1] != 0x00123456) {
        printf("[fbtest] FAILED: pixel readback\n"); return 1;
    }
    // also readable through a separate pread
    unsigned int back = 0;
    if (pread(fd, &back, 4, 0) != 4 || back != 0x00abcdef) { printf("[fbtest] FAILED: pread\n"); return 1; }

    printf("[fbtest] ALL PASSED\n");
    return 0;
}
```
Add the Makefile build rule (copy the `MMAPTEST.ELF` recipe), add `$(USERLAND_BUILD)/FBTEST.ELF` to the `$(DISK_IMG)` prereq list and an `mcopy ... ::BIN/FBTEST.ELF` line, and `spawn("/BIN/FBTEST.ELF")` in `kmain`'s spawn list.

- [ ] **Step 2: run — expect FAIL**

`make test 2>&1 | grep fbtest` → `[fbtest] FAILED: /dev/fb0 open -2` (ENOENT — no such device yet). Or `-ENOSYS` from `mmap` once the node exists but has no `.mmap`.

- [ ] **Step 3: `file_ops.mmap` + `VMA_PHYS`**

`file.h`: add `struct mmap_req` and the `mmap` slot after `poll`. `file.c`: `int64_t file_mmap(...)` = `o->mmap ? o->mmap(f, req) : -ENODEV;` (nullable — unlike the other slots; update `ops_complete` to NOT require it). `vnode_file_ops` / `pipe_ops` / `sock_ops` / `tty_file_ops` / `evdev_file_ops` leave `.mmap` unset (0).

`vma.c`: `vma_map_phys(p, phys, len, prot)` — pick an address from `p->mmap_next` (like `vma_mmap_locked`'s non-FIXED path), `vma_insert(p, addr, addr+len, prot, flags | VMA_PHYS)`, then **map every page immediately** `paging_map_into(pml4, addr+i, phys+i, PAGE_USER|PAGE_WRITABLE|PAGE_NO_EXECUTE)` (no `PROT_EXEC` — assert `!(prot & PROT_EXEC)` → `-EINVAL`). In `vma_fault_locked`: if `v->flags & VMA_PHYS` the page is already mapped, so a fault there is a genuine SIGSEGV — `return 0`. In `unmap_range` / `vma_destroy_all_locked`: when `v->flags & VMA_PHYS`, unmap the PTEs but **do not `pmm_free`** the frames (they're the framebuffer). Confirm `paging_unmap_from` / whatever the teardown calls does not blindly free — thread a `bool free_frames` or check the vma flag at the call site.

- [ ] **Step 4: `sys_mmap` device branch**

`sys_mem.c`, in `sys_mmap`, replace the `if (!(flags & MAP_ANONYMOUS) || fd >= 0) { return -ENOSYS; }` guard with:
```c
if (fd >= 0) {
    struct file_descriptor *f = fd_get(current_proc(), (int)fd);
    if (!f) { return -EBADF; }
    struct mmap_req req = { addr, len, prot, flags, (uint64_t)a->frame->r9, 0 };
    int64_t rc = file_mmap(f, &req);
    return rc < 0 ? rc : (int64_t)req.out_addr;
}
if (!(flags & MAP_ANONYMOUS)) { return -ENOSYS; }
return vma_mmap(current_proc(), addr, len, prot, flags);
```

- [ ] **Step 5: `fb_file_ops`**

`fb.c`:
```c
static int fb_open(struct file_descriptor *f) {
    if (!fb.present) return -ENODEV;
    f->ops = &fb_file_ops; f->priv = 0; return 0;
}
static int64_t fb_mmap(struct file_descriptor *f, struct mmap_req *r) {
    (void)f;
    if (r->off + r->len > fb.size) return -EINVAL;
    if (r->prot & 4 /*PROT_EXEC*/) return -EINVAL;
    int64_t rc = vma_map_phys(current_proc(), fb.phys + r->off, r->len, (uint32_t)r->prot);
    if (rc < 0) return rc;
    r->out_addr = (uint64_t)rc;
    return 0;
}
static int64_t fb_read(struct file_descriptor *f, void *buf, uint64_t n) {
    if (f->position >= fb.size) return 0;
    uint64_t k = fb.size - f->position; if (k > n) k = n;
    for (uint64_t i = 0; i < k; i++) ((uint8_t *)buf)[i] = fb.virt[f->position + i];
    f->position += k; return (int64_t)k;
}
// fb_write: symmetric. fb_lseek: clamp to [0, fb.size].
static int64_t fb_ioctl(struct file_descriptor *f, uint64_t req, void *arg) {
    (void)f;
    if (req == FBIOGET_FSCREENINFO) { struct fb_fix_screeninfo x; /* memset 0 */
        /* id="neoosfb"; smem_start=fb.phys; smem_len=fb.size; type=0; visual=2;
           line_length=fb.pitch */ copy_to_user(arg,&x,sizeof x); return 0; }
    if (req == FBIOGET_VSCREENINFO) { struct fb_var_screeninfo x; /* memset 0 */
        /* xres=xres_virtual=fb.width; yres=yres_virtual=fb.height; bits_per_pixel=32;
           red={fb.r.pos,fb.r.size,0}; green/blue similarly */ copy_to_user(arg,&x,sizeof x); return 0; }
    if (req == FBIOPUT_VSCREENINFO) return -EINVAL;  // no mode setting
    return -ENOTTY;
}
```
Use the kernel's existing user-copy path (`user_range_writable` + a `memcpy`, as `evdev.c`'s ioctls do). Add `fb0` to the devfs static table: `{ "fb0", VNODE_DEVICE, &fb_file_ops, fb_open }`.

- [ ] **Step 6: test passes; suite green**

`make test 2>&1 | grep -E 'fbtest|FAILED'` → `[fbtest] ALL PASSED`. Every other suite unchanged. Add `"[fbtest] ALL PASSED"` to `REQUIRED_MARKERS`.

- [ ] **Step 7: gauntlet + commit**

15/15. Commit `"M1a: /dev/fb0 with mmap and Linux fbdev ioctls"`.

---

## Task 3: `poll` / `select` — syscalls and the blocking core

**Files:**
- Modify: `kernel/syscall/syscall_nr.h` (`SYS_POLL` 67, `SYS_SELECT` 68, `SYS_MAX` 69), `kernel/syscall/syscall.c` (table rows), `kernel/syscall/syscall_internal.h` (decls)
- Create: `kernel/syscall/sys_poll.c`
- Modify: `kernel/sync/waitq.h` / `waitq.c` (`struct poll_waiter`, `waitq_link`/`waitq_unlink`), `kernel/fs/file.h` / `file.c` (`file_ops.poll_wait`, `file_poll_wait`)
- Modify: `kernel/ipc/pipe.c` (one `poll_wait`)
- Create: `userland/polltest.c` (+ Makefile + disk copy); `lib/syscall.c` + `lib/include/poll.h` (wrappers)
- Modify: `Makefile` (`REQUIRED_MARKERS`: `"[polltest] ALL PASSED"`)

**Interfaces:**
- Consumes: `file_poll(f, events)` (exists), `waitq_sleep_timeout(q, release, deadline)` (exists), `timer_ticks()` (exists)
- Produces:
  ```c
  // waitq.h
  struct poll_waiter {
      struct waitq q;               // poll_core sleeps here
      struct { struct waitq *wq; struct poll_link *node; } links[/*bounded by nfds*/ ];  // see below
  };
  void waitq_link(struct waitq *wq, struct poll_waiter *w);    // attach w to wq's wake set
  void waitq_unlink_all(struct poll_waiter *w);                // detach from every wq

  // file.h
  void (*poll_wait)(struct file_descriptor *f, struct poll_waiter *w);  // in file_ops
  void file_poll_wait(struct file_descriptor *f, struct poll_waiter *w);

  // uapi (poll.h / lib)
  struct pollfd { int fd; short events; short revents; };
  #define POLLIN 1
  #define POLLOUT 4
  #define POLLERR 8
  #define POLLHUP 16
  #define POLLNVAL 32
  int poll(struct pollfd *fds, unsigned long nfds, int timeout_ms);
  int select(int nfds, void *rd, void *wr, void *ex, void *tv);  // fd_set = long[16]
  ```

- [ ] **Step 1: `waitq_link` / `waitq_unlink_all` design**

The cleanest low-risk shape: give `struct waitq` a small intrusive list of "foreign waiters" separate from its sleeper list.
```c
// waitq.h additions
struct poll_link { struct poll_link *next; struct poll_waiter *w; struct waitq *wq; };
// struct waitq gains:  struct poll_link *pollers;
```
`waitq_link(wq, w)`: allocate a `struct poll_link` (from a fixed per-`poll_waiter` array — see Step 3), push onto `wq->pollers` under `wq->lock`, record it in `w` for teardown.
In `waitq_wake_all` **and** `waitq_wake_one`, after waking the real sleepers, walk `wq->pollers` and `waitq_wake_all(&pl->w->q)` for each (drop `wq->lock` first — `pl->w->q` has its own lock; ranks: both are `LOCK_RANK_WAITQ`, so take them one at a time, never nested).
`waitq_unlink_all(w)`: for each recorded link, remove it from its `wq->pollers` under `wq->lock`.

- [ ] **Step 2: failing test — `userland/polltest.c`**

```c
#include <unistd.h>
#include <stdio.h>
#include <poll.h>
extern int pipe2_raw(int fds[2], int flags);
extern int poll(struct pollfd *fds, unsigned long n, int timeout_ms);

int main(void) {
    int p[2];
    if (pipe2_raw(p, 0) != 0) { printf("[polltest] FAILED: pipe2\n"); return 1; }

    struct pollfd pf = { p[0], POLLIN, 0 };
    int r = poll(&pf, 1, 50);
    if (r != 0 || pf.revents != 0) { printf("[polltest] FAILED: empty pipe not a timeout (r=%d rev=%d)\n", r, pf.revents); return 1; }

    int kid = fork();
    if (kid == 0) { write(p[1], "x", 1); _exit(0); }
    wait(kid);
    pf.revents = 0;
    r = poll(&pf, 1, 1000);
    if (r != 1 || !(pf.revents & POLLIN)) { printf("[polltest] FAILED: POLLIN after write (r=%d rev=%d)\n", r, pf.revents); return 1; }

    char c; read(p[0], &c, 1);
    close(p[1]);
    pf.revents = 0;
    r = poll(&pf, 1, 1000);
    if (!(pf.revents & POLLHUP)) { printf("[polltest] FAILED: POLLHUP after writer close (rev=%d)\n", pf.revents); return 1; }

    printf("[polltest] ALL PASSED\n");
    return 0;
}
```
(`pipe2_raw` wrapper: `syscall2(SYS_PIPE2, (long)fds, flags)` — check `lib/syscall.c` for the existing `pipe`/`pipe2` name; reuse it.)
Wire the Makefile rule + disk copy + `spawn("/BIN/POLLTEST.ELF")`.

- [ ] **Step 3: run — expect FAIL**

`make test 2>&1 | grep polltest` → link error (`poll` undefined) or, once the wrapper exists, `-ENOSYS`.

- [ ] **Step 4: syscall numbers + `sys_poll.c`**

`syscall_nr.h`: `#define SYS_POLL 67`, `#define SYS_SELECT 68`, `#define SYS_MAX 69`. `syscall.c` table: `[SYS_POLL] = { sys_poll, "poll" }`, `[SYS_SELECT] = { sys_select, "select" }`. `syscall_internal.h`: `int64_t sys_poll(struct syscall_args *); int64_t sys_select(struct syscall_args *);`.

`sys_poll.c`:
```c
#define POLL_MAX 64   // nfds ceiling for M1a; bump later

static int64_t poll_core(struct pollfd *pfd, unsigned n, int64_t deadline) {
    struct process *p = current_proc();
    struct poll_waiter w;
    waitq_init(&w.q);
    for (unsigned i = 0; i < n; i++) {
        struct file_descriptor *f = pfd[i].fd < 0 ? 0 : fd_get(p, pfd[i].fd);
        if (f) file_poll_wait(f, &w);
    }
    int ready = 0;
    for (;;) {
        ready = 0;
        for (unsigned i = 0; i < n; i++) {
            struct file_descriptor *f = pfd[i].fd < 0 ? 0 : fd_get(p, pfd[i].fd);
            short want = pfd[i].events | POLLERR | POLLHUP;
            short got  = f ? (short)file_poll(f, want) : POLLNVAL;
            pfd[i].revents = got & (pfd[i].events | POLLERR | POLLHUP | POLLNVAL);
            if (pfd[i].revents) ready++;
        }
        if (ready) break;
        if (deadline == 0) break;                       // timeout 0 = non-blocking scan
        int rc = waitq_sleep_timeout(&w.q, 0, (uint64_t)deadline);
        if (rc == -EINTR) { waitq_unlink_all(&w); return -EINTR; }
        if (rc == -ETIMEDOUT) break;
    }
    waitq_unlink_all(&w);
    return ready;
}

int64_t sys_poll(struct syscall_args *a) {
    uint64_t uptr = a->a1; unsigned n = (unsigned)a->a2; int tmo = (int)a->a3;
    if (n > POLL_MAX) return -EINVAL;
    if (!user_range_writable(uptr, n * sizeof(struct pollfd))) return -EFAULT;
    struct pollfd pfd[POLL_MAX];
    for (unsigned i = 0; i < n; i++) copy_from_user(&pfd[i], uptr + i*sizeof(struct pollfd), sizeof(struct pollfd));
    int64_t deadline = tmo < 0 ? -1 : (tmo == 0 ? 0 : (int64_t)(timer_ticks() + (uint64_t)tmo / 10));
    int64_t r = poll_core(pfd, n, deadline < 0 ? (int64_t)UINT64_MAX : deadline);
    if (r >= 0) for (unsigned i = 0; i < n; i++) copy_to_user(uptr + i*sizeof(struct pollfd) + 6, &pfd[i].revents, 2);
    return r;
}
```
`sys_select`: translate the three `fd_set` (1024-bit = `long[16]`) into a `struct pollfd[]` (`POLLIN` for read set, `POLLOUT` for write, `POLLERR` for except), call the same `poll_core`, then write the sets back from `revents`. `nfds > 1024` → `-EINVAL`.

`file.c`: `void file_poll_wait(struct file_descriptor *f, struct poll_waiter *w) { const struct file_ops *o = ops_of(f); if (o && o->poll_wait) o->poll_wait(f, w); }`.

- [ ] **Step 5: pipe `poll_wait`**

`pipe.c`:
```c
static void pipe_poll_wait(struct file_descriptor *f, struct poll_waiter *w) {
    struct pipe *p = f->priv;
    if (!p) return;
    if (f->readable) waitq_link(&p->readers, w);
    if (f->writable) waitq_link(&p->writers, w);
}
```
Add `.poll_wait = pipe_poll_wait` to `pipe_ops`. (`vnode_file_ops`, `fb_file_ops` leave it 0 — a regular file / framebuffer is always ready; `poll_core`'s immediate scan already returns their readiness.)

- [ ] **Step 6: musl + lib wrappers**

`lib/syscall.c`: `int poll(struct pollfd *fds, unsigned long n, int t) { return (int)syscall3(SYS_POLL, (long)fds, (long)n, t); }` and `select` similarly. `lib/include/poll.h` with the struct + `POLL*`. In `third_party/shim/` (musl arch syscall dir), map Linux `SYS_poll` (7), `SYS_select` (23), `SYS_pselect6` (270, drop the 6th sigmask arg), `SYS_ppoll` (271, drop sigmask) onto `SYS_POLL`/`SYS_SELECT`.

- [ ] **Step 7: test passes; suite green**

`make test 2>&1 | grep -E 'polltest|FAILED'` → `[polltest] ALL PASSED`. `nettest`, `pipetest`, `sigtest`, `tlstest` unchanged. Add the marker.

- [ ] **Step 8: gauntlet + commit**

15/15. Commit `"M1a: poll/select over file_ops.poll with a foreign-waiter wake path"`.

---

## Task 4: `poll_wait` for sockets, the tty, and evdev

**Files:**
- Modify: `kernel/net/socket.c`, `kernel/dev/tty.c`, `kernel/dev/evdev.c` (one `poll_wait` each)
- Modify: `userland/polltest.c` (add an evdev case), `docs/stdlib.md` (evdev "read never blocks" note → "use poll")

**Interfaces:**
- Consumes: `waitq_link` (Task 3), each driver's existing wait queues (`sock->readers`, `console_tty.readers`, `evdev_client->readers`)

- [ ] **Step 1: extend `polltest.c` — the evdev case**

After the pipe checks, before `ALL PASSED`:
```c
    int ev = open("/dev/input/event0", O_RDONLY);
    if (ev >= 0) {
        struct pollfd e = { ev, POLLIN, 0 };
        if (poll(&e, 1, 30) != 0) { printf("[polltest] FAILED: evdev ready with no input\n"); return 1; }
        long rc = neoos_test_inject_key(30 /*KEY_A*/, 1);
        if (rc == 0) {                       // test-hooks build only
            e.revents = 0;
            if (poll(&e, 1, 1000) != 1 || !(e.revents & POLLIN)) {
                printf("[polltest] FAILED: evdev POLLIN after inject\n"); return 1;
            }
            struct input_event iev[4]; read(ev, iev, sizeof iev);
            neoos_test_inject_key(30, 0);
        }
        close(ev);
    }
```
(`#include <neoos_test.h>`, `#include <linux/input.h>`; tolerate `rc == -38` / `ev < 0`.)

- [ ] **Step 2: run — expect the evdev sub-case to FAIL**

`make test 2>&1 | grep polltest` → `[polltest] FAILED: evdev POLLIN after inject` (the poll blocks the full second and times out, because `evdev`'s ring fills but nothing wakes the poll waiter).

- [ ] **Step 3: implement the three `poll_wait`s**

`socket.c`:
```c
static void sock_poll_wait(struct file_descriptor *f, struct poll_waiter *w) {
    struct socket *s = f->priv;
    if (s) waitq_link(&s->readers, w);
}
```
`+ .poll_wait = sock_poll_wait` in `sock_ops`.

`tty.c` (still pre-refactor here — bound to `console_tty`):
```c
static void tty_fop_poll_wait(struct file_descriptor *f, struct poll_waiter *w) {
    (void)f; waitq_link(&console_tty.readers, w);
}
```
`+ .poll_wait = tty_fop_poll_wait` in `tty_file_ops`.

`evdev.c`:
```c
static void evdev_fop_poll_wait(struct file_descriptor *f, struct poll_waiter *w) {
    struct evdev_client *c = f->priv;
    if (c) waitq_link(&c->readers, w);
}
```
`+ .poll_wait = evdev_fop_poll_wait` in `evdev_file_ops`. Confirm `input_key_event` / `input_inject_key` call `waitq_wake_all(&c->readers)` — it does (Phase 15), so the foreign-waiter walk added in Task 3 Step 1 now also wakes the poll.

- [ ] **Step 4: test passes; suite green**

`make test` → `[polltest] ALL PASSED` including the evdev case (test-hooks build). Production build: the evdev sub-case is skipped on `rc == -38`, still passes.

- [ ] **Step 5: docs + gauntlet + commit**

`docs/stdlib.md`: the evdev "`read` never blocks — use `poll`/`select`" note gains "which are now implemented and correct". Gauntlet 15/15. Commit `"M1a: poll_wait for sockets, tty, and evdev"`.

---

## Task 5: `struct tty` becomes an allocatable object

**Files:**
- Modify: `kernel/dev/tty.h` (public `struct tty`, `struct tty_backend`, function signatures gain `struct tty *`), `kernel/dev/tty.c` (every `console_tty` reference → the passed `struct tty *`; `console_backend`; `console_tty` keeps its exact behaviour)
- Modify: `kernel/sync/lock.h` (`LOCK_RANK_TTY`), `kernel/dev/keyboard.c` / `kernel/dev/input.c` (whatever calls `tty_input_char` — now `tty_input_char(&console_tty_obj, c)` or a `tty_console()` accessor)
- Modify: `kernel/fs/devfs.c` (CONSOLE/TTY entries still resolve to the console tty)

**Interfaces:**
- Produces:
  ```c
  // tty.h  (struct promoted from tty.c file scope)
  struct tty_backend { void (*output)(struct tty *t, const char *s, uint32_t n); };
  struct tty {
      struct spinlock lock;               // LOCK_RANK_TTY
      struct waitq readers, poll_readers;
      char edit[TTY_BUF]; uint32_t edit_len;
      char ready[TTY_BUF]; uint32_t ready_head, ready_len;
      int saw_eof;
      struct termios_k tio; struct winsize_k win;
      int fg_pgid, sid;
      const struct tty_backend *backend; void *backend_priv;
      // pty only:
      char outq[TTY_BUF]; uint32_t out_head, out_len;
      struct waitq out_readers;
  };
  void   tty_obj_init(struct tty *t, const struct tty_backend *b, void *priv);
  int64_t tty_obj_read(struct tty *t, void *buf, uint32_t len, int nonblock);
  int64_t tty_obj_write(struct tty *t, const void *buf, uint32_t len);   // -> backend->output
  int64_t tty_obj_ioctl(struct tty *t, uint64_t req, void *arg);
  int     tty_obj_poll(struct tty *t, int events);
  void    tty_input_char(struct tty *t, char c);                        // was: (char c)
  struct tty *tty_console(void);                                        // the one console tty
  ```

- [ ] **Step 1: no new test — this is a behaviour-preserving refactor**

The gate is "every existing suite still passes and the gauntlet is 15/15 ×3". `ttytest` already exercises the console tty end to end (canonical mode, echo, `TCGETS`, `TIOCGWINSZ`, `SIGINT`, `pgrp`); it must keep passing byte-for-byte.

- [ ] **Step 2: promote the struct, thread the pointer**

Move `struct tty` (and `TTY_BUF`) from `tty.c` to `tty.h`. Add `struct tty_backend`, the `backend`/`backend_priv` and `outq` fields. Rename the file-scope `static struct tty console_tty;` to a still-static object, add `struct tty *tty_console(void) { return &console_obj; }`. Change every internal function (`ready_push`, `line_commit`, `tty_input_char`, `tty_read`, `tty_write`, `tty_ioctl`, `tty_selftest*`) to take `struct tty *t` and use it instead of `&console_tty`. `tty_file_ops`' `tty_fop_*` call `tty_obj_*(tty_console(), ...)`.

- [ ] **Step 3: the console backend**

```c
static void console_output(struct tty *t, const char *s, uint32_t n) {
    (void)t;
    for (uint32_t i = 0; i < n; i++) { serial_putc(s[i]); console_putc(s[i]); }
}
static const struct tty_backend console_backend = { console_output };
```
`tty_init()` calls `tty_obj_init(&console_obj, &console_backend, 0)`. `tty_obj_write` moves the ONLCR / OPOST translation it does today, then calls `t->backend->output(t, ...)`. The keyboard/input path calls `tty_input_char(tty_console(), c)`.

- [ ] **Step 4: lock rank**

`lock.h`: add `#define LOCK_RANK_TTY 8` is taken — pick an unused number that keeps ascending order with the input lock (`LOCK_RANK_INPUT` 21) and the fd table (16). `LOCK_RANK_TTY` should rank **below** `LOCK_RANK_INPUT` (taken from `tty_input_char`, called under the input path) and can sit near `LOCK_RANK_DRIVER` (8). Reuse `LOCK_RANK_DRIVER` if a dedicated slot isn't warranted — but the spec asks for `LOCK_RANK_TTY`; add it just above `DRIVER` with the rationale "a struct tty's lock; taken from the input core and from syscalls; never held across console_write or waitq_* ".

- [ ] **Step 5: build, boot, verify no drift**

`make test` — `[ttytest] ALL PASSED`, `[tty] selftest passed`, and the full suite, unchanged. Diff the serial log against a pre-refactor capture: only timing-sensitive hex (tick counts, epoch) may differ.

- [ ] **Step 6: gauntlet ×3 + commit**

`for i in 1 2 3; do bash .superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 2 || break; done` — 45/45 (the tty path is what every suite's output travels through; this is the milestone's riskiest change). Commit `"M1a: struct tty is an allocatable object with a backend vtable"`.

---

## Task 6: `devfs_register` — dynamic device entries

**Files:**
- Modify: `kernel/fs/devfs.h` (`devfs_register`/`devfs_unregister` decls), `kernel/fs/devfs.c` (dynamic table, checked after the static one in `devfs_lookup` / `devfs_readdir`; static `pts` dir entry)
- Modify: `kernel/sync/lock.h` (`LOCK_RANK_DEVFS`)

**Interfaces:**
- Produces:
  ```c
  int  devfs_register(const char *path, const struct file_ops *ops, void *priv);  // 0 / -ENOSPC / -EEXIST
  void devfs_unregister(const char *path);
  ```

- [ ] **Step 1: failing test — a kernel selftest**

`devfs.c`, a `devfs_selftest()` called from `kmain` after `vfs` is up:
```c
void devfs_selftest(void) {
    static const struct file_ops dummy = { .name="dummy", .read=0 /*...*/ };
    if (devfs_register("pts/7", &dummy, (void *)0x1234) != 0) { serial_write_string("[devfs] selftest FAILED: register\n"); return; }
    uint64_t id;
    struct vnode dir = { .inode_id = /* the pts dir id */ };
    if (devfs_ops.lookup(&dir, "7", &id) != 0) { serial_write_string("[devfs] selftest FAILED: lookup\n"); return; }
    devfs_unregister("pts/7");
    if (devfs_ops.lookup(&dir, "7", &id) == 0) { serial_write_string("[devfs] selftest FAILED: still resolves after unregister\n"); return; }
    serial_write_string("[devfs] selftest passed\n");
}
```
Add `"[devfs] selftest passed"` to `REQUIRED_MARKERS`.

- [ ] **Step 2: run — expect FAIL** (`devfs_register` undefined → link error).

- [ ] **Step 3: implement**

```c
#define DEVFS_DYN_MAX 32
static struct { char path[32]; const struct file_ops *ops; void *priv; int used; } dyn[DEVFS_DYN_MAX];
static struct spinlock dyn_lock;   // LOCK_RANK_DEVFS
```
`devfs_register`: reject a duplicate, find a free slot, copy the path. `devfs_unregister`: clear the matching slot. In `devfs_lookup`: after the static table misses, if `dir` is the `pts` directory, scan `dyn` for `pts/<name>` and return a synthetic inode id (`DEVFS_DYN_BASE + slot`, a range above every static id). `devfs_read_inode` for a dynamic id: `out->type = VNODE_DEVICE`, `out->fs_private` points at a `struct devfs_dev` synthesised from the slot (`ops`, an `open` that sets `f->ops = ops; f->priv = slot->priv`). `devfs_readdir` on the `pts` dir lists the used `dyn` slots. Add a static `{ "pts", VNODE_DIR, 0, 0 }` entry so `/dev/pts` exists even when empty.

- [ ] **Step 4: test passes; suite green.** `make test` → `[devfs] selftest passed`, suite unchanged.

- [ ] **Step 5: gauntlet + commit**

15/15. Commit `"M1a: devfs_register for dynamic /dev entries"`.

---

## Task 7: PTY — `/dev/ptmx`, `/dev/pts/N`

**Files:**
- Create: `kernel/dev/pty.h`, `kernel/dev/pty.c`
- Modify: `kernel/fs/devfs.c` (`ptmx` static entry → `ptmx_open`), `kernel/kernel.c` (`pty_init()`), `kernel/sync/lock.h` (`LOCK_RANK_PTY`)
- Create: `userland/ptytest.c`, `userland/catn.c` (+ Makefile rules + disk copies)
- Modify: `Makefile` (`REQUIRED_MARKERS`: `"[ptytest] ALL PASSED"`)
- Modify: `lib/syscall.c` / headers (`posix_openpt` = `open("/dev/ptmx", O_RDWR)`; `ptsname_r`; `grantpt`/`unlockpt` = ioctl no-ops)

**Interfaces:**
- Consumes: `tty_obj_*` + `struct tty_backend` (Task 5), `devfs_register`/`devfs_unregister` (Task 6), `file_poll`/`file_poll_wait` (Tasks 3–4)
- Produces:
  ```c
  void pty_init(void);
  int  ptmx_open(struct file_descriptor *f);   // devfs open hook for /dev/ptmx
  // ioctl numbers (Linux): TIOCGPTN 0x80045430, TIOCSPTLCK 0x40045431,
  //   TIOCGWINSZ 0x5413, TIOCSWINSZ 0x5414, TIOCSCTTY 0x540E
  ```

- [ ] **Step 1: failing test — `userland/catn.c` then `userland/ptytest.c`**

`catn.c` (the child the pty test runs on the slave):
```c
#include <unistd.h>
int main(void) {
    char buf[128]; int n;
    while ((n = read(0, buf, sizeof buf)) > 0) { write(1, buf, n); if (buf[n-1] == '\n') break; }
    return 0;
}
```
`ptytest.c`:
```c
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
extern int  ioctl(int, unsigned long, void *);
#define TIOCGPTN 0x80045430
#define TCGETS   0x5401

int main(void) {
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0) { printf("[ptytest] FAILED: open ptmx %d\n", m); return 1; }
    int n = -1;
    if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) { printf("[ptytest] FAILED: TIOCGPTN\n"); return 1; }

    char path[32]; /* build "/dev/pts/<n>" */
    int s = open(path, O_RDWR);
    if (s < 0) { printf("[ptytest] FAILED: open %s -> %d\n", path, s); return 1; }

    char t[64];
    if (ioctl(s, TCGETS, t) != 0)  { printf("[ptytest] FAILED: slave not a tty\n"); return 1; }
    int pfd[2]; pipe(pfd);   /* just to have a non-tty fd */
    if (ioctl(pfd[0], TCGETS, t) == 0) { printf("[ptytest] FAILED: pipe claims to be a tty\n"); return 1; }

    int kid = fork();
    if (kid == 0) { dup2(s, 0); dup2(s, 1); close(m); close(s);
                    execve("/BIN/CATN.ELF", 0, 0); _exit(127); }
    close(s);
    write(m, "hi\n", 3);
    char out[16] = {0}; int got = 0;
    while (got < 4) { int r = read(m, out + got, sizeof out - got); if (r <= 0) break; got += r; }
    if (strncmp(out, "hi\r\n", 4) != 0) { printf("[ptytest] FAILED: master read '%s' want 'hi\\r\\n'\n", out); return 1; }

    int st = wait(kid);
    (void)st;
    // ^C to the master kills the foreground child with SIGINT
    int k2 = fork();
    if (k2 == 0) { dup2(s = open(path, O_RDWR), 0); execve("/BIN/CATN.ELF",0,0); _exit(127); }
    write(m, "\x03", 1);
    st = wait(k2);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != 2 /*SIGINT*/) { printf("[ptytest] FAILED: ^C did not SIGINT (st=%d)\n", st); return 1; }

    printf("[ptytest] ALL PASSED\n");
    return 0;
}
```
Wire both into the Makefile + disk image + `spawn("/BIN/PTYTEST.ELF")` (CATN is `execve`d, not spawned).

- [ ] **Step 2: run — expect FAIL** (`open("/dev/ptmx")` → `-ENOENT`).

- [ ] **Step 3: `pty.c` — the pool and the two `file_ops`**

```c
#define PTY_MAX 16
struct pty { struct tty slave; int used, master_open, slave_open; int index; };
static struct pty ptys[PTY_MAX];
static struct spinlock pool_lock;   // LOCK_RANK_PTY

static void pty_output(struct tty *t, const char *s, uint32_t n) {
    struct pty *pt = t->backend_priv;
    uint64_t f = spin_lock_irqsave(&t->lock);
    for (uint32_t i = 0; i < n && t->out_len < TTY_BUF; i++)
        t->outq[(t->out_head + t->out_len++) % TTY_BUF] = s[i];
    spin_unlock_irqrestore(&t->lock, f);
    waitq_wake_all(&t->out_readers);
}
static const struct tty_backend pty_backend = { pty_output };

int ptmx_open(struct file_descriptor *f) {
    uint64_t fl = spin_lock_irqsave(&pool_lock);
    struct pty *pt = 0;
    for (int i = 0; i < PTY_MAX; i++) if (!ptys[i].used) { pt = &ptys[i]; pt->used = 1; pt->index = i; break; }
    spin_unlock_irqrestore(&pool_lock, fl);
    if (!pt) return -ENFILE;
    tty_obj_init(&pt->slave, &pty_backend, pt);
    pt->master_open = 1; pt->slave_open = 0;
    char path[16]; /* "pts/<i>" */ devfs_register(path, &pts_file_ops, pt);
    f->ops = &ptm_file_ops; f->priv = pt;
    return 0;
}
```
`ptm_file_ops`:
- `read` → drain `slave.outq` (blocks on `slave.out_readers` unless `f->nonblock`; EOF when `slave_open == 0` and `outq` empty).
- `write` → `for each byte: tty_input_char(&pt->slave, b)` (echo, canonical assembly, `^C` → `SIGINT` to `slave.fg_pgid` — all already in `tty_input_char`).
- `poll` → `POLLIN` if `outq` non-empty, `POLLOUT` always.
- `poll_wait` → `waitq_link(&pt->slave.out_readers, w)`.
- `ioctl`: `TIOCGPTN` → `pt->index`; `TIOCSPTLCK` → 0; `TIOCSWINSZ` → store into `slave.win`; `TIOCGWINSZ` → return it.
- `close` → `pt->master_open = 0`; if `slave_open`, `signal_send_pgrp(slave.fg_pgid, SIGHUP)` and slave reads get EOF; if both closed, `devfs_unregister(path)`, `pt->used = 0`.

`pts_file_ops`: `read`/`write`/`ioctl`/`poll` = `tty_obj_*(&pt->slave, ...)`. `open` (via the devfs synthetic `open` hook, `f->priv = pt`) sets `pt->slave_open = 1` and, if the caller is a session leader with no controlling tty and no `O_NOCTTY`, `slave.sid = caller->sid; slave.fg_pgid = caller->pgid`. `close` → `pt->slave_open = 0`.

`pty_init()`: `spin_init(&pool_lock, LOCK_RANK_PTY, "pty-pool")` and zero the pool. Add `{ "ptmx", VNODE_DEVICE, 0, ptmx_open }` to the devfs static table (fops NULL — `ptmx_open` sets `f->ops` itself).

- [ ] **Step 4: `grantpt`/`unlockpt`/`ptsname` in the lib**

`lib/syscall.c`: `int posix_openpt(int flags){ return open("/dev/ptmx", flags); }`, `int grantpt(int fd){ (void)fd; return 0; }`, `int unlockpt(int fd){ int z=0; return ioctl(fd, TIOCSPTLCK, &z); }`, `int ptsname_r(int fd, char *buf, size_t n){ int i; if (ioctl(fd, TIOCGPTN, &i)) return -1; /* snprintf "/dev/pts/%d" */ }`.

- [ ] **Step 5: test passes; suite green**

`make test 2>&1 | grep -E 'ptytest|FAILED'` → `[ptytest] ALL PASSED`. `ttytest` (console tty) unchanged. Add the marker.

- [ ] **Step 6: gauntlet ×3 + commit**

45/45. Commit `"M1a: PTY subsystem -- /dev/ptmx, /dev/pts/N"`.

---

## Task 8: docs + ABI refresh

**Files:**
- Modify: `docs/stdlib.md` (new sections), `docs/abi-compatibility.md` (refresh to close-of-M1a)

- [ ] **Step 1: `docs/stdlib.md`**

Three new sections:
- **`/dev/fb0` and `<linux/fb.h>`** — `FBIOGET_VSCREENINFO`/`FSCREENINFO`, `mmap` gives a direct view of the scanout buffer, byte `read`/`write` work too. **DIVERGENCE:** `FBIOPUT_VSCREENINFO` returns `-EINVAL` — the mode is fixed at boot by GRUB, no mode setting. 32bpp packed RGB only.
- **`poll` / `select`** — `struct pollfd`, the Linux flag values, `poll_core` semantics. **DIVERGENCE:** `POLLIN`/`POLLOUT`/`POLLERR`/`POLLHUP`/`POLLNVAL` only — no `POLLPRI`, no `POLLRDHUP`; no `epoll`; `ppoll`/`pselect6` accept but ignore the signal mask; `poll` `nfds` capped at 64, `select` `nfds` at 1024; timeout resolution is one 10 ms tick.
- **PTY (`/dev/ptmx`, `/dev/pts/N`)** — `posix_openpt`/`grantpt`/`unlockpt`/`ptsname_r`, the master/slave model, `TIOCGPTN`. **DIVERGENCES:** `grantpt`/`unlockpt` are no-ops (no pts permission model); `TIOCSWINSZ` stores the size but sends **no `SIGWINCH`** (the framebuffer terminal is fixed-size); 16 ptys; `/dev/pts` is dynamic devfs, not a `devpts` mount.

Update the evdev "read never blocks" note: "`poll`/`select` are implemented and report `POLLIN` correctly — use them to wait."

- [ ] **Step 2: `docs/abi-compatibility.md`**

Refresh the header to "close of M1a (console plumbing)". Add rows/notes:
- `poll`/`select` — **implemented** (subset; §list the divergences).
- fbdev — **present** (subset; no mode setting).
- pty — **present** (subset; no `SIGWINCH`, no devpts).
- In "What a real ported application hits": `epoll` still absent; `SIGWINCH` never delivered; no framebuffer mode setting.

- [ ] **Step 3: commit**

`"M1a: docs -- fb0, poll/select, pty; abi refresh"`.

---

## Self-Review

**Spec coverage:**
- §1 framebuffer (request tag, MBI parse, map, text fallback) → Task 1 Steps 1–3, 7.
- §2 in-kernel fbcon (font, glyph blit, scroll, console.h indirection, panic path) → Task 1 Steps 4–6.
- §3 `/dev/fb0` (`file_ops.mmap`, `VMA_PHYS`, `fb_mmap`, `FBIOGET_*`, read/write) → Task 2.
- §4 poll/select (syscalls, `poll_core`, `poll_wait` hook, `waitq_link`, timeout, evdev, musl) → Tasks 3 (core + pipe) and 4 (socket/tty/evdev + musl in Task 3 Step 6).
- §5 PTY (`struct tty` allocatable, `tty_backend`, `/dev/ptmx`, `/dev/pts/N`, `devfs_register`, controlling terminal, winsize stub) → Tasks 5 (refactor), 6 (`devfs_register`), 7 (pty).
- §6 lock ranks (`LOCK_RANK_TTY`/`PTY`/`DEVFS`) → Tasks 5 Step 4, 7 Step 3, 6 Step 3.
- §7 new syscalls / ioctl numbers → Task 3 Step 4, Task 2 Step 5, Task 7 Step 3.
- Testing (`fbtest`, `polltest`, `ptytest`, `fbcon_selftest`, `devfs_selftest`, `make test` compat) → each task's test step + Task 1 Step 7 (serial unchanged).
- Documentation → Task 8; the W^X-adjacent `.mmap` `PROT_EXEC` rejection is asserted in Task 2 Step 3/5.

**Placeholder scan:** the `struct poll_waiter.links[]` array bound is "bounded by nfds" — made concrete in Task 3 Step 4 (`POLL_MAX 64`, so a fixed `struct poll_link[64]` inside `poll_waiter`, allocated on `poll_core`'s stack). `font8x16.c` says "paste a public-domain 8×16 font" — that is a real, findable artifact (the IBM VGA ROM font is public domain and ~20 lines of `xxd` output), not a design gap. `copy_from_user`/`copy_to_user` in the snippets = the existing `user_range_writable` + `memcpy` pattern from `evdev.c`; named for brevity, spelled out in Task 2 Step 5. No "add error handling" / bare "write tests".

**Type consistency:** `struct fb_info` fields (`phys`, `size`, `pitch`, `width`, `height`, `bpp`, `virt`, `present`) identical in Task 1's interface block and Task 2's usage. `struct mmap_req` (`addr`, `len`, `prot`, `flags`, `off`, `out_addr`) identical in Task 2's `file.h` block and `sys_mmap` / `fb_mmap`. `struct tty` field names (`readers`, `outq`, `out_head`, `out_len`, `out_readers`, `backend`, `backend_priv`, `fg_pgid`, `win`) identical across Tasks 5 and 7. `poll_wait` / `waitq_link` / `waitq_unlink_all` spelled the same in Tasks 3 and 4. `tty_obj_*` names identical in Tasks 5 and 7. `devfs_register` / `devfs_unregister` identical in Tasks 6 and 7. `TIOCGPTN` = `0x80045430` in Task 7 Steps 1, 3, 4.

**Ordering:** Task 1 (framebuffer + console) has no dependency on the rest and is the visible change. Task 2 needs Task 1's `fb`. Task 3 (poll core) is independent of 1–2. Task 4 needs Task 3. Task 5 (tty refactor) is independent but ordered before Task 7 (pty needs the allocatable tty). Task 6 (`devfs_register`) is independent, ordered before Task 7 (pty needs it). Task 7 needs 3–6. Task 8 last. Tasks 3 and 5 could run in parallel; the plan orders them for reviewability.
