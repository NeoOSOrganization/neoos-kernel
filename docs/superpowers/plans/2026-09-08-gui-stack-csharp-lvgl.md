# NeoOS GUI Stack (C compositor + LVGL + C#) Implementation Plan

> **STATUS 2026-09-09: DELIVERED.** All six phases are implemented and
> on `main`. A C# application draws through LVGL into a window
> composited by `neoos-wm`; `make hello` reproduces it and screenshots
> the result.
>
> **Two things changed from this plan while executing it, both for the
> better:**
>
> 1. **The compositor is written in C, not C#** (user's direction). It
>    is the process that holds the screen for the whole machine, so it
>    should not carry a garbage collector, and this keeps a managed
>    runtime off the display path entirely. C# is the application
>    language, which is where the request put it. Tasks 12-15 below
>    describe the C# compositor that was NOT built; `userland/wm.c` is
>    what exists.
> 2. **G1 was already mostly implemented.** `KDSETMODE`/`KD_GRAPHICS`
>    and the console render-path skip existed; only restore-on-death
>    and the inactive-VT gate were missing.
>
> Everything else landed as written. The task-by-task detail below is
> kept as the record of what was done and why.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A window, drawn by a C# application using LVGL, composited by
a C# window manager that owns the framebuffer, containing a Hello World
label.

**Architecture:** Six phases. G1 closes the two remaining holes in
framebuffer ownership. G2 adds a PS/2 mouse, which first requires the
input subsystem to grow past its single hardcoded device. G3 is the
kernel IPC that a windowing system needs and NeoOS does not have:
AF_UNIX sockets, `memfd_create`, `ftruncate`, and `SCM_RIGHTS`. G4 is
`neoos-wm`, a C# NativeAOT compositor. G5 ports LVGL and binds it from
C# through NativeAOT's `DirectPInvoke`, which links a static archive
and resolves calls at link time -- mandatory here, because NeoOS's ELF
loader handles only `PT_LOAD` and `PT_TLS` and there is no `dlopen`.
G6 puts the two together.

**Tech Stack:** C11 freestanding kernel; `x86_64-neoos-linux-musl-gcc`
9.4.0 for userland C; .NET NativeAOT (static, AOT-only -- NeoOS refuses
`W|X` mappings); LVGL 9.x built as `liblvgl.a`; QEMU headless with
serial capture for all verification.

**Spec:** `docs/superpowers/specs/2026-09-08-gui-stack-csharp-lvgl-design.md`

## Global Constraints

- **No host tests exist and none can.** This is bare-metal code with no
  host runtime. Every task is verified by building, booting headless in
  QEMU, and grepping the serial log for a marker. The TDD cycle is:
  write the selftest so its `passed` marker is *absent* (or a `FAILED:`
  marker appears), implement, boot again, see `passed`.
- Verification one-liner, used verbatim in "run" steps below:

  ```bash
  cd ~/projects/personal/NeoOS && \
  make clean-kernel iso disk-image QUIET=1 2>&1 | grep -iE "error:|Error [0-9]" ; \
  timeout 150 qemu-system-x86_64 -cpu Nehalem -smp 4 -boot order=d \
    -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
    -drive file=build/disk2.img,format=raw -vga std \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -audiodev none,id=a0 -device AC97,audiodev=a0,addr=0x6 \
    -device usb-ehci -device usb-mouse \
    -no-reboot -display none -serial file:build/serial.gui.log > /dev/null 2>&1 ; \
  grep -nE "\[vt\]|\[fb\]|\[input\]|\[mouse\]|\[memfd\]|\[unix\]|\[scm\]|PANIC|\[exception\]" build/serial.gui.log
  ```
- **`tools/gauntlet.sh 15 3` is the acceptance bar for every task that
  changes kernel runtime behaviour**, not only the last one. Per
  `docs/superpowers/specs/`, a concurrency bug shows up as a hang, not
  a compile error. The gauntlet needs an **exclusive build** -- any
  concurrent `make` kills it silently while appearing to still run.
- **Syscall numbers are NeoOS's own.** `SYS_MAX` is currently 134.
  This plan adds exactly two: `SYS_FTRUNCATE 134`, `SYS_MEMFD_CREATE
  135`, and moves `SYS_MAX` to 136. AF_UNIX and `SCM_RIGHTS` reuse the
  existing `SYS_SOCKET`/`SYS_BIND`/`SYS_LISTEN`/`SYS_ACCEPT4`/
  `SYS_CONNECT`/`SYS_SENDMSG`/`SYS_RECVMSG` numbers.
- **Everything crossing into userland is Linux-shaped.** Struct
  layouts, flag values, errno numbers and edge-case semantics match
  Linux x86-64. The syscall numbers are ours; the shapes behind them
  are not. Exact values are given in each task -- copy them, do not
  invent them.
- **Every kernel feature reachable from userland needs a musl shim
  entry or a `lib/` wrapper, plus a `docs/stdlib.md` update.** A raw
  syscall number with no library support is an incomplete task, per
  `CLAUDE.md`. This is folded into the task that adds the feature, not
  deferred to the end.
- **Rebuild `neoos-musl` before concluding a syscall is missing.** A
  stale `upstream/` tree produces a phantom `[shim] ENOSYS` and has
  already cost one session; see `docs/aspnet-missing-syscalls.md`.
- Work happens directly on `main`. No feature branches.

## File Structure

**Kernel, modified:**
- `kernel/tty/vt.c` — gains per-VT ownership of `kd_mode` by fd, and
  release-on-close. Already holds `KDSETMODE`/`KDGETMODE`.
- `kernel/drivers/video/vesafb.c` — gains the active-VT gate on write
  and on `mmap` fault.
- `kernel/drivers/input/input.c` / `input.h` — the single global client
  list and grab become `struct input_dev`; keyboard and mouse are
  instances.
- `kernel/drivers/input/evdev.c` — ioctls become device-parameterised.
- `kernel/fs/devfs.c` — `input/event1`, and the hardcoded input-dir
  inode ids untangled.
- `kernel/arch/isr.c`, `kernel/kernel.c` — `VECTOR_MOUSE`, GSI 12.
- `kernel/net/socket.c` — `socket_create` learns `AF_UNIX`.
- `kernel/syscall/syscall_nr.h`, `syscall.c`, `sys_mem.c`, `sys_file.c`.

**Kernel, created:**
- `kernel/drivers/input/mouse.c` / `mouse.h` — i8042 aux port, packet
  state machine, decoder selftest.
- `kernel/ipc/memfd.c` / `memfd.h` — anonymous shared memory objects.
- `kernel/ipc/unix_sock.c` / `unix_sock.h` — AF_UNIX stream sockets,
  abstract and pathname namespaces, `SCM_RIGHTS` in-flight fds.

**Userland / repos, created:**
- `userland/uxtest.c` — AF_UNIX + memfd + `SCM_RIGHTS` conformance
  test, `make uxtest`.
- `lib/` — `neoos_wm_client.c/h`, the C client for the compositor
  protocol (NeoOS-native, no POSIX analogue, so `lib/` is where
  `CLAUDE.md` puts it).
- `neoos-wm` (new sibling repo) — the C# compositor.
- `neoos-lvgl` (new sibling repo) — `liblvgl.a` plus C# bindings.

---
## Phase G1 — Framebuffer ownership

`KDSETMODE`/`KDGETMODE`, `KD_TEXT`/`KD_GRAPHICS` and the render-path
skip already exist in `kernel/tty/vt.c`. Only the two gaps below are
open.

### Task 1: Restore KD_TEXT when the claiming fd is released

Today every write to `kd_mode` is init, an explicit `KDSETMODE`, or
`vt_panic_reset`. A graphical process that crashes leaves its VT in
`KD_GRAPHICS` and the machine has no console.

`vt_file_ops` currently stores the VT index directly in `f->priv` as an
`int` cast through a pointer (`vt_fop_index` at `kernel/tty/vt.c:284`),
and `vt_fop_close` is an empty stub at line 333. Replace the raw int
with a small heap struct so the fd can also remember whether *it*
claimed graphics mode, and refcount it so `dup`/`fork` behave.

**Files:**
- Modify: `kernel/tty/vt.c` (`vt_fop_index`, `vt_fop_close`, add
  `vt_fop_dup`, `KDSETMODE` case, `vt_dev_open` in `kernel/fs/devfs.c`)
- Modify: `kernel/tty/vt.h` (declare `vt_selftest_kd_mode`)
- Modify: `kernel/fs/devfs.c` (the `/dev/ttyN` open that sets `f->priv`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `int vt_selftest_kd_mode(int vt_index)` — returns the VT's
  current `kd_mode`, for the selftest to assert against.

- [ ] **Step 1: Write the failing selftest**

Add to `kernel/tty/vt.c`, and call it from the existing `vt_selftest()`:

```c
// A VT left in KD_GRAPHICS by a process that died must come back to
// KD_TEXT when the last fd that claimed it is released. Otherwise a
// crashed graphical app leaves the machine with no console.
static void vt_kd_release_selftest(void) {
    struct file_descriptor f;
    int idx = 1;                       // not the active VT; no repaint

    if (vt_dev_open_for_test(&f, idx) != 0) {
        serial_write_string("[vt] kd-release selftest FAILED: open\n");
        return;
    }
    int g = KD_GRAPHICS;
    if (vt_ioctl_fd(&f, KDSETMODE, &g) != 0) {
        serial_write_string("[vt] kd-release selftest FAILED: set\n");
        return;
    }
    if (vt_selftest_kd_mode(idx) != KD_GRAPHICS) {
        serial_write_string("[vt] kd-release selftest FAILED: not graphics\n");
        return;
    }

    // Releasing the claiming fd restores text mode.
    vt_file_ops.close(&f);
    if (vt_selftest_kd_mode(idx) != KD_TEXT) {
        serial_write_string("[vt] kd-release selftest FAILED: not restored\n");
        return;
    }

    // A dup'd fd holds the claim until the LAST reference goes.
    struct file_descriptor a, b;
    vt_dev_open_for_test(&a, idx);
    vt_ioctl_fd(&a, KDSETMODE, &g);
    b = a;
    vt_file_ops.dup(&b);
    vt_file_ops.close(&a);
    if (vt_selftest_kd_mode(idx) != KD_GRAPHICS) {
        serial_write_string("[vt] kd-release selftest FAILED: dup released early\n");
        return;
    }
    vt_file_ops.close(&b);
    if (vt_selftest_kd_mode(idx) != KD_TEXT) {
        serial_write_string("[vt] kd-release selftest FAILED: dup never released\n");
        return;
    }

    serial_write_string("[vt] kd-release selftest passed\n");
}
```

- [ ] **Step 2: Run it and confirm it fails**

Run the verification one-liner from Global Constraints.
Expected: a compile error (`vt_selftest_kd_mode` undefined) or, once
stubbed, `[vt] kd-release selftest FAILED: not restored`. It must NOT
print `passed`.

- [ ] **Step 3: Replace the raw-int priv with a refcounted claim**

In `kernel/tty/vt.c`:

```c
// What /dev/ttyN's f->priv points at. It was a bare int cast to a
// pointer; it has to carry the graphics claim too, so that releasing
// the fd -- including by dying -- can put the VT back to KD_TEXT.
struct vt_fd {
    int index;          // 0 == "whichever VT is active"
    int claimed;        // this fd put its VT into KD_GRAPHICS
    int refs;           // dup/fork share one vt_fd
};

static int vt_fop_index(struct file_descriptor *f) {
    struct vt_fd *v = f->priv;
    return v ? v->index : 0;
}

static void vt_fop_dup(struct file_descriptor *f) {
    struct vt_fd *v = f->priv;
    if (v) { __atomic_add_fetch(&v->refs, 1, __ATOMIC_SEQ_CST); }
}

static void vt_fop_close(struct file_descriptor *f) {
    struct vt_fd *v = f->priv;
    if (!v) { return; }
    if (__atomic_sub_fetch(&v->refs, 1, __ATOMIC_SEQ_CST) != 0) { return; }

    // Last reference. If this fd claimed graphics mode, hand the screen
    // back -- this is the path a crashing process takes, so it must not
    // depend on the process having done anything.
    if (v->claimed) {
        int idx = v->index ? v->index - 1 : vt_active;
        uint64_t fl = spin_lock_irqsave(&vt_lock);
        vts[idx].kd_mode = KD_TEXT;
        if (idx == vt_active) { render_full_locked(&vts[idx]); }
        spin_unlock_irqrestore(&vt_lock, fl);
    }
    f->priv = 0;
    kfree(v);
}

int vt_selftest_kd_mode(int vt_index) {
    uint64_t fl = spin_lock_irqsave(&vt_lock);
    int m = vts[vt_index].kd_mode;
    spin_unlock_irqrestore(&vt_lock, fl);
    return m;
}
```

In the `KDSETMODE` case, record the claim on the fd. `vt_ioctl` takes
an index rather than the fd today, so thread the `struct vt_fd *`
through: add `int64_t vt_ioctl_fd(struct file_descriptor *f, uint64_t
request, void *arg)` which resolves the `vt_fd`, calls the existing
`vt_ioctl`, and on a successful `KDSETMODE` sets
`v->claimed = (a == KD_GRAPHICS)`. Point `vt_file_ops.ioctl` at it.

In `kernel/fs/devfs.c`, the `/dev/ttyN` open allocates a `struct vt_fd`
with `refs = 1`, `claimed = 0` instead of stuffing an int into `priv`.

- [ ] **Step 4: Run and confirm it passes**

Run the verification one-liner.
Expected: `[vt] kd-release selftest passed`, and the pre-existing
`[vt] selftest passed` still present.

- [ ] **Step 5: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15. Nothing else may be building at the same time — a
concurrent `make` kills the gauntlet silently while it appears to run.

- [ ] **Step 6: Commit**

```bash
git add kernel/tty/vt.c kernel/tty/vt.h kernel/fs/devfs.c
git commit -m "vt: restore KD_TEXT when the claiming fd is released

A VT left in KD_GRAPHICS by a process that crashed left the machine
with no console -- every kd_mode write was init, an explicit ioctl, or
vt_panic_reset, and nothing ran on the way out. /dev/ttyN's f->priv
becomes a refcounted vt_fd that remembers whether this fd made the
claim, so the last close puts the screen back whether or not the
process asked."
```

### Task 2: Gate /dev/fb0 on the owning VT being active

`kernel/drivers/video/vesafb.c` contains no reference to the VT layer.
A process on an inactive VT can `write()`, or keep painting through an
already-established `mmap`, straight over the foreground VT.

**Files:**
- Modify: `kernel/drivers/video/vesafb.c` (`fb_write` at line 274,
  `fb_mmap` at line 252)
- Modify: `kernel/tty/vt.h` (declare `vt_process_owns_screen`)
- Modify: `kernel/tty/vt.c` (implement it)

**Interfaces:**
- Consumes: `vt_selftest_kd_mode` from Task 1.
- Produces: `int vt_process_owns_screen(void)` — 1 if the calling
  process's VT is the active one and that VT is in `KD_GRAPHICS`.

- [ ] **Step 1: Write the failing selftest**

```c
// A process must not paint the framebuffer from an inactive VT.
static void fb_gate_selftest(void) {
    int saved = vt_active_index();

    vt_switch(0);
    int g = KD_GRAPHICS;
    vt_ioctl(1, KDSETMODE, &g);        // claim VT 0 (1-based arg)
    if (!vt_process_owns_screen()) {
        serial_write_string("[fb] gate selftest FAILED: active VT denied\n");
        vt_switch(saved); return;
    }

    vt_switch(1);                       // switch away
    if (vt_process_owns_screen()) {
        serial_write_string("[fb] gate selftest FAILED: inactive VT allowed\n");
        vt_switch(saved); return;
    }

    vt_switch(saved);
    serial_write_string("[fb] gate selftest passed\n");
}
```

Call it from `vesafb`'s existing init-time selftest hook.

- [ ] **Step 2: Run it and confirm it fails**

Run the verification one-liner.
Expected: compile error, or `[fb] gate selftest FAILED: inactive VT
allowed`. Not `passed`.

- [ ] **Step 3: Implement the gate**

In `kernel/tty/vt.c`:

```c
// Whether the CALLING process may paint the screen right now: its VT
// must be the active one, and that VT must be in KD_GRAPHICS. NeoOS
// enforces this in the kernel rather than negotiating with
// VT_SETMODE/VT_PROCESS and a signal the way Linux does -- an app here
// is not told it lost the screen, it is simply prevented from
// painting. Recorded as a divergence in docs/stdlib.md.
int vt_process_owns_screen(void) {
    uint64_t fl = spin_lock_irqsave(&vt_lock);
    int ok = (vts[vt_active].kd_mode == KD_GRAPHICS);
    spin_unlock_irqrestore(&vt_lock, fl);
    return ok;
}
```

In `kernel/drivers/video/vesafb.c`, at the top of `fb_write` and
`fb_mmap`:

```c
    if (!vt_process_owns_screen()) { return -EBUSY; }
```

`-EBUSY` rather than `-EPERM`: this is a transient condition that will
clear on the next VT switch, and Linux fbdev returns `-EBUSY` for a
framebuffer in use.

- [ ] **Step 4: Run and confirm it passes**

Run the verification one-liner.
Expected: `[fb] gate selftest passed`. The TinyGL gears demo
(`neoos-tinygl`) must still render — check it separately if that repo
is checked out.

- [ ] **Step 5: Update docs/stdlib.md**

Add the divergence: VT screen ownership is enforced in-kernel, not via
`VT_SETMODE`/`VT_PROCESS`; `VT_SETMODE` is accepted for ABI
compatibility but the process-mode handshake is not implemented, and an
inactive VT's `fb_write`/`fb_mmap` returns `-EBUSY`.

- [ ] **Step 6: Commit**

```bash
git add kernel/drivers/video/vesafb.c kernel/tty/vt.c kernel/tty/vt.h docs/stdlib.md
git commit -m "fb: refuse writes and mmap from a VT that is not active

vesafb.c had no reference to the VT layer, so a process on a background
VT could write, or keep painting through an existing mapping, over
whatever the foreground was showing. Both paths now check
vt_process_owns_screen() and return -EBUSY, which is what Linux fbdev
returns for a framebuffer in use and is honest about the condition
being transient."
```

---

## Phase G2 — PS/2 mouse and a multi-device input subsystem

### Task 3: Split input.c's single global device into struct input_dev

`kernel/drivers/input/input.c` has one global client list, one global
grab, and fans every event to every client. A mouse cannot be added to
that — `/dev/input/event0` would start emitting pointer events. This
task is a pure refactor: no new behaviour, and the existing
`input_selftest` is the regression check.

**Files:**
- Modify: `kernel/drivers/input/input.h`, `input.c`, `evdev.c`

**Interfaces:**
- Produces:
  - `struct input_dev` with fields `{struct spinlock lock; struct
    evdev_client *clients; struct evdev_client *grab; const char *name;
    uint32_t ev_bits; uint8_t key_bits[KEY_CNT/8]; uint16_t rel_bits;}`
  - `extern struct input_dev input_kbd;`
  - `struct evdev_client *evdev_client_open(struct input_dev *dev);`
  - `struct input_dev *evdev_client_dev(struct evdev_client *c);`
  - `void input_post(struct input_dev *dev, const struct input_event *evs, int n);`
    — pushes `n` events atomically to every client of `dev`, wakes the
    readers and notifies the poll heads. Task 5 uses this for the mouse.

- [ ] **Step 1: Confirm the existing selftest passes before touching anything**

Run the verification one-liner.
Expected: `[input] selftest passed`. Record that this is the baseline;
it must still pass at Step 4 with no edits to the selftest itself.

- [ ] **Step 2: Introduce struct input_dev and move the globals into it**

In `input.h`, add the struct above and `extern struct input_dev
input_kbd;`. In `input.c`, replace the file-scope

```c
static struct {
    struct spinlock lock;
    struct evdev_client *clients;
    struct evdev_client *grab;
} input;
```

with `struct input_dev input_kbd;`, give `struct evdev_client` a
`struct input_dev *dev;` back-pointer set in `evdev_client_open`, and
mechanically rewrite `input.clients` → `dev->clients`,
`input.grab` → `dev->grab`, `input.lock` → `dev->lock`.
`input_key_event` operates on `&input_kbd`. `evdev_client_close`,
`evdev_client_grab`, `evdev_client_read` and `evdev_client_poll` take
their device from `c->dev`.

`input_init` becomes:

```c
static void input_dev_init(struct input_dev *d, const char *name) {
    spin_init(&d->lock, LOCK_RANK_INPUT, name);
    d->clients = 0;
    d->grab = 0;
    d->name = name;
}

void input_init(void) {
    input_dev_init(&input_kbd, "NeoOS AT keyboard");
    input_kbd.ev_bits = (1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_MSC);
}
```

- [ ] **Step 3: Make evdev.c's ioctls read the device, not constants**

In `kernel/drivers/input/evdev.c`, `EVIOCGNAME` currently returns the
literal `"NeoOS AT keyboard"` and `get_ev_bitmap` hardcodes
`EV_SYN|EV_KEY|EV_MSC`. Both take their answer from
`evdev_client_dev(c)`:

```c
    case 0x06: {  // EVIOCGNAME(len)
        if (!arg || size == 0) { return -EINVAL; }
        return copy_string_to_user(evdev_client_dev(c)->name, arg, size);
    }
```

and `get_ev_bitmap` becomes a loop over `dev->ev_bits`.

- [ ] **Step 4: Run and confirm the untouched selftest still passes**

Run the verification one-liner.
Expected: `[input] selftest passed`, unchanged. If it fails, the
refactor changed behaviour — that is the bug, not the test.

- [ ] **Step 5: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15.

- [ ] **Step 6: Commit**

```bash
git add kernel/drivers/input/
git commit -m "input: give the subsystem a device object

input.c had one global client list and one global grab, and fanned
every event to every client. A second device could not be added to
that -- event0 would have started emitting pointer events. The globals
become struct input_dev with the keyboard as the first instance, and
evdev's name and capability ioctls read the device instead of
returning constants. Pure refactor: input_selftest is unchanged and
still passes."
```

### Task 4: Make evdev reads actually block

`evdev_client_read` returns `-EAGAIN` even for a blocking fd; the
in-tree comment attributes this to lock ordering. `poll`/`epoll` work,
which is why nothing has hit it yet — but the compositor will do a
blocking read on the event fd and would spin at 100% CPU.

**Files:**
- Modify: `kernel/drivers/input/input.c` (`evdev_client_read`)

**Interfaces:**
- Consumes: `struct input_dev` from Task 3.
- Produces: no signature change; `evdev_client_read(c, buf, len, 0)`
  now sleeps until an event arrives.

- [ ] **Step 1: Write the failing selftest**

```c
// A blocking read with an empty ring must sleep, not spin. Inject from
// a second thread so the sleeper has something to wake for.
static void input_blocking_read_selftest(void) {
    struct evdev_client *c = evdev_client_open(&input_kbd);
    struct input_event ev[8];

    input_selftest_wake_after_ms(20, 30 /* KEY_A */);
    uint64_t t0 = timer_ticks();
    int64_t n = evdev_client_read(c, ev, sizeof(ev), 0);
    uint64_t elapsed = timer_ticks() - t0;

    if (n <= 0) {
        serial_write_string("[input] blocking-read selftest FAILED: no events\n");
    } else if (elapsed < 1) {
        serial_write_string("[input] blocking-read selftest FAILED: did not sleep\n");
    } else {
        serial_write_string("[input] blocking-read selftest passed\n");
    }
    evdev_client_close(c);
}
```

- [ ] **Step 2: Run it and confirm it fails**

Run the verification one-liner.
Expected: `[input] blocking-read selftest FAILED: no events` (the
current code returns `-EAGAIN` immediately).

- [ ] **Step 3: Implement the wait**

```c
int64_t evdev_client_read(struct evdev_client *c, void *buf,
                          uint64_t len, int nonblock) {
    if (!c || !buf) { return -EINVAL; }
    if (len < sizeof(struct input_event)) { return -EINVAL; }

    struct input_dev *dev = c->dev;
    struct input_event *out = (struct input_event *)buf;
    uint32_t max_events = len / sizeof(struct input_event);

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&dev->lock);
        uint32_t copied = 0;
        while (copied < max_events && !ring_empty(c)) {
            out[copied++] = c->ring[c->tail++ & 0xFF];
        }
        if (copied > 0) {
            spin_unlock_irqrestore(&dev->lock, flags);
            return copied * (int64_t)sizeof(struct input_event);
        }
        if (nonblock) {
            spin_unlock_irqrestore(&dev->lock, flags);
            return -EAGAIN;
        }
        // Prepare to sleep BEFORE dropping the lock, so an event
        // arriving in the window wakes us instead of being missed.
        waitq_prepare(&c->readers);
        spin_unlock_irqrestore(&dev->lock, flags);
        if (waitq_wait_interruptible(&c->readers) < 0) { return -EINTR; }
    }
}
```

Use whichever prepare/wait pair `kernel/sync/waitq.h` actually exposes;
the requirement is that the sleep is armed while the lock is still held
so the wakeup cannot be lost.

- [ ] **Step 4: Run and confirm it passes**

Run the verification one-liner.
Expected: `[input] blocking-read selftest passed` and the original
`[input] selftest passed` both present.

- [ ] **Step 5: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15. A lost-wakeup bug here shows up as a hang, not a
failure — that is what the gauntlet is for.

- [ ] **Step 6: Commit**

```bash
git add kernel/drivers/input/input.c
git commit -m "input: make blocking evdev reads block

evdev_client_read returned -EAGAIN for a blocking fd, with a comment
blaming lock ordering. poll and epoll cover for it, which is why
nothing had hit it, but the compositor reads the event fd directly and
would have spun. The sleep is armed while the device lock is still
held so an event arriving in the drop-and-sleep window cannot be
missed."
```

### Task 5: PS/2 mouse packet decoder

Decoder first, hardware second: the state machine is pure and can be
tested by feeding it bytes, exactly as `keyboard_decode_selftest` does.

**Files:**
- Create: `kernel/drivers/input/mouse.c`, `kernel/drivers/input/mouse.h`
- Modify: `Makefile` (add `mouse.c` to the kernel sources)

**Interfaces:**
- Consumes: `input_post`, `struct input_dev` from Task 3.
- Produces:
  - `struct mouse_packet { int16_t dx, dy, dwheel; uint8_t buttons; }`
    where `buttons` is a bitmask, bit 0 left, bit 1 right, bit 2 middle.
  - `int mouse_decode(uint8_t byte, struct mouse_packet *out)` — returns
    1 and fills `*out` when a packet completes, 0 while mid-packet.
  - `void mouse_set_packet_size(int n)` — 3 or 4; Task 6 calls it after
    the IntelliMouse negotiation.
  - `void mouse_decode_selftest(void)`

- [ ] **Step 1: Write the failing decoder selftest**

```c
// PS/2 packets: byte 0 flags (bit0 left, bit1 right, bit2 middle,
// bit3 always 1, bit4 X sign, bit5 Y sign), byte 1 dx, byte 2 dy,
// byte 3 wheel (IntelliMouse 4-byte mode only). dy is positive UP in
// PS/2 and positive DOWN in evdev, so the decoder negates it.
void mouse_decode_selftest(void) {
    struct mouse_packet p;

    mouse_set_packet_size(3);

    // +5 right, +5 up, no buttons: flags 0x08, dx 5, dy 5
    if (mouse_decode(0x08, &p) || mouse_decode(0x05, &p)) {
        serial_write_string("[mouse] decode selftest FAILED: early complete\n");
        return;
    }
    if (!mouse_decode(0x05, &p) || p.dx != 5 || p.dy != -5 || p.buttons != 0) {
        serial_write_string("[mouse] decode selftest FAILED: simple move\n");
        return;
    }

    // Negative dx: X sign bit set (0x10), dx byte 0xFB == -5
    mouse_decode(0x18, &p); mouse_decode(0xFB, &p);
    if (!mouse_decode(0x00, &p) || p.dx != -5 || p.dy != 0) {
        serial_write_string("[mouse] decode selftest FAILED: negative dx\n");
        return;
    }

    // Left button held, no motion
    mouse_decode(0x09, &p); mouse_decode(0x00, &p);
    if (!mouse_decode(0x00, &p) || p.buttons != 0x01) {
        serial_write_string("[mouse] decode selftest FAILED: left button\n");
        return;
    }

    // A byte-0 with bit 3 clear is not a valid header: resync, do not
    // silently decode three bytes of garbage as a huge jump.
    if (mouse_decode(0x00, &p)) {
        serial_write_string("[mouse] decode selftest FAILED: bad header accepted\n");
        return;
    }

    // 4-byte IntelliMouse: wheel one notch down == 0xFF
    mouse_set_packet_size(4);
    mouse_decode(0x08, &p); mouse_decode(0x00, &p); mouse_decode(0x00, &p);
    if (!mouse_decode(0xFF, &p) || p.dwheel != -1) {
        serial_write_string("[mouse] decode selftest FAILED: wheel\n");
        return;
    }

    serial_write_string("[mouse] decode selftest passed\n");
}
```

- [ ] **Step 2: Run it and confirm it fails**

Run the verification one-liner.
Expected: compile error — `mouse.c` does not exist yet.

- [ ] **Step 3: Implement the decoder**

`kernel/drivers/input/mouse.c`, state machine only — no port I/O in
this task:

```c
static uint8_t pkt[4];
static int pkt_len = 3;      // 4 after a successful IntelliMouse knock
static int pkt_pos = 0;

void mouse_set_packet_size(int n) { pkt_len = (n == 4) ? 4 : 3; pkt_pos = 0; }

int mouse_decode(uint8_t byte, struct mouse_packet *out) {
    // Bit 3 of the first byte is always 1 on a real PS/2 packet. If it
    // is not, the stream is out of sync -- drop the byte rather than
    // decode three bytes of garbage into a huge cursor jump.
    if (pkt_pos == 0 && !(byte & 0x08)) { return 0; }

    pkt[pkt_pos++] = byte;
    if (pkt_pos < pkt_len) { return 0; }
    pkt_pos = 0;

    uint8_t flags = pkt[0];

    // Overflow bits (6, 7) mean the counters saturated; the deltas are
    // meaningless, so report buttons and no motion rather than a jump.
    if (flags & 0xC0) {
        out->dx = out->dy = out->dwheel = 0;
    } else {
        int dx = pkt[1], dy = pkt[2];
        if (flags & 0x10) { dx |= ~0xFF; }   // sign-extend from the flag
        if (flags & 0x20) { dy |= ~0xFF; }
        out->dx = (int16_t)dx;
        out->dy = (int16_t)(-dy);            // PS/2 is +up, evdev is +down
        out->dwheel = 0;
        if (pkt_len == 4) {
            int8_t w = (int8_t)(pkt[3] & 0x0F);
            if (w & 0x08) { w |= (int8_t)0xF0; }
            out->dwheel = (int16_t)(-w);     // wheel down is negative
        }
    }
    out->buttons = flags & 0x07;
    return 1;
}
```

- [ ] **Step 4: Run and confirm it passes**

Run the verification one-liner.
Expected: `[mouse] decode selftest passed`.

- [ ] **Step 5: Commit**

```bash
git add kernel/drivers/input/mouse.c kernel/drivers/input/mouse.h Makefile
git commit -m "input: PS/2 mouse packet decoder

State machine only, no port I/O -- which is what makes it testable the
way keyboard_decode is: bytes in, packets out, no hardware. Handles
3-byte and IntelliMouse 4-byte packets, sign extension from the flag
bits, the overflow bits (report buttons, no motion, rather than a
cursor jump), the always-1 header bit as a resync guard, and the PS/2
+up to evdev +down inversion."
```

### Task 6: Wire the mouse to the hardware and to /dev/input/event1

**Files:**
- Modify: `kernel/drivers/input/mouse.c` (aux port init, IRQ handler)
- Modify: `kernel/arch/isr.c` (`VECTOR_MOUSE` dispatch),
  `kernel/drivers/input/keyboard.h` (`#define VECTOR_MOUSE 0x2C`)
- Modify: `kernel/kernel.c` (IOAPIC redirection for GSI 12)
- Modify: `kernel/fs/devfs.c` (`input/event1`)
- Modify: `kernel/drivers/input/evdev.c` (`EVIOCGBIT(EV_REL)`, nr `0x22`)

**Interfaces:**
- Consumes: `mouse_decode`, `mouse_set_packet_size`, `input_post`.
- Produces: `extern struct input_dev input_mouse;`, `/dev/input/event1`.

Linux constants, to be used exactly: `EV_REL` `0x02`, `REL_X` `0x00`,
`REL_Y` `0x01`, `REL_WHEEL` `0x08`, `BTN_LEFT` `0x110`, `BTN_RIGHT`
`0x111`, `BTN_MIDDLE` `0x112`.

- [ ] **Step 1: Write the failing fan-out isolation selftest**

The important property is not that the mouse works — QEMU's input is
hard to drive from a selftest — but that the two devices are
**isolated**. Inject on each and assert the other saw nothing:

```c
static void input_isolation_selftest(void) {
    struct evdev_client *k = evdev_client_open(&input_kbd);
    struct evdev_client *m = evdev_client_open(&input_mouse);
    struct input_event ev[16];

    input_inject_key(30, 1);                 // KEY_A press
    if (evdev_client_read(m, ev, sizeof(ev), 1) > 0) {
        serial_write_string("[input] isolation selftest FAILED: key reached the mouse\n");
        goto out;
    }
    if (evdev_client_read(k, ev, sizeof(ev), 1) <= 0) {
        serial_write_string("[input] isolation selftest FAILED: key missed the keyboard\n");
        goto out;
    }

    struct mouse_packet p = { .dx = 3, .dy = -4, .dwheel = 0, .buttons = 0x01 };
    mouse_post_packet(&p);
    if (evdev_client_read(k, ev, sizeof(ev), 1) > 0) {
        serial_write_string("[input] isolation selftest FAILED: motion reached the keyboard\n");
        goto out;
    }
    int64_t n = evdev_client_read(m, ev, sizeof(ev), 1);
    // REL_X, REL_Y, BTN_LEFT, SYN_REPORT
    if (n != 4 * (int64_t)sizeof(struct input_event) ||
        ev[0].type != EV_REL || ev[0].code != REL_X    || ev[0].value != 3 ||
        ev[1].type != EV_REL || ev[1].code != REL_Y    || ev[1].value != -4 ||
        ev[2].type != EV_KEY || ev[2].code != BTN_LEFT || ev[2].value != 1) {
        serial_write_string("[input] isolation selftest FAILED: mouse event content\n");
        goto out;
    }
    serial_write_string("[input] isolation selftest passed\n");
out:
    evdev_client_close(k);
    evdev_client_close(m);
}
```

- [ ] **Step 2: Run it and confirm it fails**

Run the verification one-liner.
Expected: compile error — `input_mouse` and `mouse_post_packet` do not
exist.

- [ ] **Step 3: Implement aux-port init, the IRQ path, and event posting**

In `mouse.c`:

```c
#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define PS2_STAT 0x64

static void ps2_wait_write(void) { while (inb(PS2_STAT) & 0x02) { } }
static void ps2_wait_read(void)  { while (!(inb(PS2_STAT) & 0x01)) { } }

// Commands to the MOUSE go through the controller's 0xD4 prefix;
// unprefixed bytes would be read by the keyboard.
static uint8_t aux_cmd(uint8_t cmd) {
    ps2_wait_write(); outb(PS2_CMD, 0xD4);
    ps2_wait_write(); outb(PS2_DATA, cmd);
    ps2_wait_read();  return inb(PS2_DATA);       // ACK == 0xFA
}

void mouse_init(void) {
    ps2_wait_write(); outb(PS2_CMD, 0xA8);        // enable the aux port

    // Turn on IRQ12 in the controller's config byte.
    ps2_wait_write(); outb(PS2_CMD, 0x20);
    ps2_wait_read();  uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                                   // aux interrupt enable
    cfg &= (uint8_t)~0x20;                         // aux clock enable
    ps2_wait_write(); outb(PS2_CMD, 0x60);
    ps2_wait_write(); outb(PS2_DATA, cfg);

    aux_cmd(0xF6);                                 // set defaults

    // IntelliMouse knock: sample rates 200, 100, 80 make a wheel mouse
    // report ID 3 and switch to 4-byte packets. A plain mouse stays 0.
    aux_cmd(0xF3); aux_cmd(200);
    aux_cmd(0xF3); aux_cmd(100);
    aux_cmd(0xF3); aux_cmd(80);
    aux_cmd(0xF2);                                 // get device id
    ps2_wait_read();
    uint8_t id = inb(PS2_DATA);
    mouse_set_packet_size(id == 3 ? 4 : 3);

    aux_cmd(0xF4);                                 // enable reporting
    input_dev_init(&input_mouse, "NeoOS PS/2 mouse");
    input_mouse.ev_bits  = (1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_REL);
    input_mouse.rel_bits = (1u << REL_X) | (1u << REL_Y) | (1u << REL_WHEEL);
}

// Turn a decoded packet into the evdev event group and post it. Only
// the axes that CHANGED are reported, which is what Linux does and what
// keeps a still mouse silent.
void mouse_post_packet(const struct mouse_packet *p) {
    static uint8_t prev_buttons;
    struct input_event ev[8];
    int n = 0;

    if (p->dx)     { ev[n].type = EV_REL; ev[n].code = REL_X;     ev[n].value = p->dx; n++; }
    if (p->dy)     { ev[n].type = EV_REL; ev[n].code = REL_Y;     ev[n].value = p->dy; n++; }
    if (p->dwheel) { ev[n].type = EV_REL; ev[n].code = REL_WHEEL; ev[n].value = p->dwheel; n++; }

    static const uint16_t btn[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    for (int i = 0; i < 3; i++) {
        uint8_t was = prev_buttons & (1u << i), now = p->buttons & (1u << i);
        if (was != now) {
            ev[n].type = EV_KEY; ev[n].code = btn[i]; ev[n].value = now ? 1 : 0; n++;
        }
    }
    prev_buttons = p->buttons;

    if (n == 0) { return; }                 // nothing changed, stay silent
    ev[n].type = EV_SYN; ev[n].code = SYN_REPORT; ev[n].value = 0; n++;
    input_post(&input_mouse, ev, n);
}

void mouse_handler(void) {
    struct mouse_packet p;
    if (mouse_decode(inb(PS2_DATA), &p)) { mouse_post_packet(&p); }
}
```

`kernel/arch/isr.c`, alongside the existing `VECTOR_KEYBOARD` case:

```c
    if (regs->vector_number == VECTOR_MOUSE) {
        mouse_handler();
        lapic_send_eoi();
        return;
    }
```

`kernel/kernel.c`, mirroring the keyboard routing that already resolves
`acpi.irq1_gsi`: route GSI 12 to `VECTOR_MOUSE`, then call
`mouse_init()`.

- [ ] **Step 4: Add /dev/input/event1 and the EV_REL ioctl**

`kernel/fs/devfs.c` reaches `input/event0` through inode ids hardcoded
as 5 and 6 in `devfs_lookup` and `devfs_readdir`, with a comment
warning that they must not shift. Untangle that rather than adding a
second special case: give the input directory a real child table and
iterate it, so `event0` and `event1` are entries rather than constants.

In `evdev.c`, add `EVIOCGBIT(EV_REL)` — ioctl nr `0x22` — returning
`evdev_client_dev(c)->rel_bits`.

- [ ] **Step 5: Run and confirm it passes**

Run the verification one-liner (it already passes `-device usb-mouse`).
Expected: `[mouse] decode selftest passed`, `[input] isolation selftest
passed`, and `[input] selftest passed`.

Then check the device node is real:

```bash
grep -nE "\[devfs\]|event1" build/serial.gui.log
```

- [ ] **Step 6: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15. A new IRQ source is exactly the kind of change that
shows up as a rare hang.

- [ ] **Step 7: Update docs/stdlib.md and commit**

Document `/dev/input/event1` (PS/2 mouse, `EV_REL`/`EV_KEY`, wheel when
the IntelliMouse knock succeeds) and note that there is no
`EVIOCGBIT(EV_ABS)` because there is no absolute pointing device.

```bash
git add kernel/ docs/stdlib.md
git commit -m "input: PS/2 mouse on /dev/input/event1

Aux port enable, the 0xD4-prefixed command path, the IntelliMouse
200/100/80 knock for a scroll wheel, IRQ12 routed to VECTOR_MOUSE, and
the packet decoder from the previous commit wired to a second
input_dev. Only changed axes and changed buttons are reported, so a
still mouse is silent.

devfs reached input/event0 through inode ids hardcoded as 5 and 6 with
a comment warning they must not shift; the input directory now has a
real child table instead, which is what made room for event1."
```

---
## Phase G3 — Kernel IPC: AF_UNIX, memfd, SCM_RIGHTS

The largest phase. Nothing here exists today: `ftruncate` has no
syscall number, `memfd_create` does not exist, `socket_create` refuses
every domain but `AF_INET` (AF_UNIX is `socketpair` only, backed by two
pipes), and `recvmsg` ignores `msg_control` entirely.

Tasks 7 and 8 are independent of Tasks 9 and 10; Task 11 needs all four.

### Task 7: ftruncate

**Files:**
- Modify: `kernel/syscall/syscall_nr.h` (`SYS_FTRUNCATE 134`, `SYS_MAX 136`)
- Modify: `kernel/syscall/syscall.c` (dispatch table entry),
  `syscall_internal.h`, `kernel/syscall/sys_file.c`
- Modify: `kernel/fs/file.h` (`file_ops.truncate`), `kernel/fs/file.c`
- Modify: `kernel/fs/ramfs.c` (implement `truncate` for ramfs files)
- Modify: `neoos-musl` shim (map musl's `__NR_ftruncate` 77 → 134)

**Interfaces:**
- Produces:
  - `file_ops` gains `int64_t (*truncate)(struct file_descriptor *f, uint64_t length);`
    Returns `-EINVAL` for objects that cannot be sized.
  - `int64_t file_truncate(struct file_descriptor *f, uint64_t length);`
  - `sys_ftruncate(fd, length)` → 0, `-EBADF`, `-EINVAL`.

- [ ] **Step 1: Write the failing userland test**

Create `userland/uxtest.c` (it grows across Tasks 7–11) with a `make
uxtest` target modelled on the existing `epollet` target in the
`Makefile` — its own `INITTAB`, boots alone, greps for
`[uxtest] ALL PASSED`:

```c
static void test_ftruncate(void) {
    int fd = open("/tmp/ux-trunc", O_RDWR | O_CREAT, 0600);
    CHECK(fd >= 0, "open");
    CHECK(ftruncate(fd, 8192) == 0, "ftruncate grow");

    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 8192, "size after grow");

    // Growing must zero-fill, not expose whatever was in the page.
    char *p = malloc(8192);
    CHECK(pread(fd, p, 8192, 0) == 8192, "read back");
    for (int i = 0; i < 8192; i++) { CHECK(p[i] == 0, "grow zero-filled"); }

    CHECK(ftruncate(fd, 100) == 0, "ftruncate shrink");
    CHECK(fstat(fd, &st) == 0 && st.st_size == 100, "size after shrink");
    CHECK(ftruncate(fd, (off_t)-1) == -1 && errno == EINVAL, "negative length");
    close(fd);
    free(p);
    say("ftruncate ok");
}
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `[shim] ENOSYS 77` in the log, no `ALL PASSED`. **If you see
an unexpected ENOSYS, rebuild `neoos-musl` before believing it** — a
stale `upstream/` tree fakes exactly this symptom and has cost a
session before.

- [ ] **Step 3: Implement**

Add the `truncate` op to `struct file_ops` next to the existing
`mmap`/`ready_seq` optional ops, `file_truncate` in `file.c` returning
`-EINVAL` when the op is null, `sys_ftruncate` in `sys_file.c`
rejecting `(int64_t)length < 0` with `-EINVAL`, and the ramfs
implementation (grow zero-fills; shrink frees whole pages past the new
end).

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `[uxtest] ftruncate ok`.

- [ ] **Step 5: Update docs/stdlib.md and commit**

```bash
git add kernel/ userland/uxtest.c Makefile docs/stdlib.md
git commit -m "syscall: ftruncate

New optional file_ops.truncate, implemented for ramfs, plus the
syscall. Needed to size a memfd before mapping it; useful on its own.
Growing zero-fills rather than exposing whatever the page held."
```

### Task 8: memfd_create and shared mmap

**Files:**
- Create: `kernel/ipc/memfd.c`, `kernel/ipc/memfd.h`
- Modify: `kernel/syscall/syscall_nr.h` (`SYS_MEMFD_CREATE 135`),
  `syscall.c`, `syscall_internal.h`, `kernel/syscall/sys_mem.c`
- Modify: `neoos-musl` shim (musl's `__NR_memfd_create` 319 → 135)

**Interfaces:**
- Consumes: `file_ops.truncate` from Task 7.
- Produces:
  - `MFD_CLOEXEC` = 1, `MFD_ALLOW_SEALING` = 2 (Linux values).
  - `int64_t sys_memfd_create(const char *name, unsigned flags)` → fd.
  - A memfd's `file_ops` implements `truncate`, `mmap`, `read`, `write`,
    `lseek`, `dup`, `close`.

`sys_mmap` already dispatches to `file_ops.mmap` for any fd whose ops
provide it — that is how `/dev/fb0` works
(`kernel/syscall/sys_mem.c:38-46`). A memfd rides that path, so **no
general file-backed mmap is needed**.

- [ ] **Step 1: Write the failing userland test**

```c
static void test_memfd(void) {
    int fd = memfd_create("surface", MFD_CLOEXEC);
    CHECK(fd >= 0, "memfd_create");
    CHECK(ftruncate(fd, 4096 * 4) == 0, "size it");

    uint32_t *a = mmap(0, 4096 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(a != MAP_FAILED, "first mapping");
    uint32_t *b = mmap(0, 4096 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(b != MAP_FAILED, "second mapping");
    CHECK(a != b, "distinct addresses");

    // The whole point: two mappings of one object share frames.
    a[0] = 0xDEADBEEF;
    a[4095] = 0x12345678;
    CHECK(b[0] == 0xDEADBEEF, "write visible through the other mapping");
    CHECK(b[4095] == 0x12345678, "and not only on page 0");
    b[1] = 0xFEEDFACE;
    CHECK(a[1] == 0xFEEDFACE, "and in the other direction");

    // A fresh memfd is zero, never a recycled page's contents.
    int fd2 = memfd_create("fresh", 0);
    ftruncate(fd2, 4096);
    uint32_t *c = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    for (int i = 0; i < 1024; i++) { CHECK(c[i] == 0, "fresh memfd is zeroed"); }

    // MAP_PRIVATE of a memfd must NOT propagate writes back.
    uint32_t *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED, "private mapping");
    p[0] = 0xAAAAAAAA;
    CHECK(a[0] == 0xDEADBEEF, "MAP_PRIVATE write did not leak back");

    munmap(a, 4096 * 4); munmap(b, 4096 * 4); munmap(c, 4096); munmap(p, 4096);
    close(fd); close(fd2);
    say("memfd ok");
}
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `[shim] ENOSYS 319`, no `memfd ok`.

- [ ] **Step 3: Implement**

`kernel/ipc/memfd.c`: an object holding a page-pointer array, a size, a
refcount and a lock. `truncate` grows the array (allocating zeroed
frames) or shrinks it (freeing frames past the end). `mmap` with
`MAP_SHARED` maps the object's frames into the caller via the existing
`vma_map_phys` path; with `MAP_PRIVATE` it maps them copy-on-write.
`close` drops the refcount and frees the frames at zero.

Sealing: accept `MFD_ALLOW_SEALING` and store the flag, but implement
no `F_ADD_SEALS` yet — record that in `docs/stdlib.md` as a divergence
rather than silently accepting a flag that does nothing.

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `[uxtest] memfd ok`.

- [ ] **Step 5: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15. New frame lifetime rules are the classic source of a
leak that only shows up under repetition.

- [ ] **Step 6: Update docs/stdlib.md and commit**

```bash
git add kernel/ipc/memfd.c kernel/ipc/memfd.h kernel/ userland/uxtest.c docs/stdlib.md
git commit -m "ipc: memfd_create with shared mmap

Anonymous shared memory objects, sized with ftruncate and mapped
through the file_ops.mmap hook sys_mmap already uses for /dev/fb0 --
so no general file-backed mmap was needed. MAP_SHARED mappings of one
object share frames, which is the whole point; MAP_PRIVATE is
copy-on-write. MFD_ALLOW_SEALING is accepted and stored but F_ADD_SEALS
is not implemented, recorded as a divergence rather than pretended."
```

### Task 9: AF_UNIX stream sockets

**Files:**
- Create: `kernel/ipc/unix_sock.c`, `kernel/ipc/unix_sock.h`
- Modify: `kernel/net/socket.c` (`socket_create` accepts `AF_UNIX`),
  `kernel/net/socket.h`
- Modify: `kernel/syscall/sys_net.c` (bind/listen/accept4/connect route
  to the unix implementation for `AF_UNIX` fds)

**Interfaces:**
- Produces:
  - `struct sockaddr_un { uint16_t sun_family; char sun_path[108]; }`
    — Linux's layout exactly.
  - Abstract namespace: `sun_path[0] == '\0'`, the name is the
    remaining `addrlen - offsetof(sockaddr_un, sun_path) - 1` bytes and
    is **not** NUL-terminated. Implement this first — it needs no
    filesystem node and is all the compositor requires.
  - Pathname binding creates an `S_IFSOCK` node; `connect` looks it up.
  - Byte stream reuses `kernel/ipc/pipe.c`'s ring buffers, the way
    `kernel/ipc/socketpair.c` already does. Do not write a second one.

- [ ] **Step 1: Write the failing userland test**

```c
static void test_unix_socket(void) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    // Abstract namespace: a leading NUL, name in the remaining bytes.
    memcpy(a.sun_path, "\0neoos-uxtest", 13);
    socklen_t alen = offsetof(struct sockaddr_un, sun_path) + 13;

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(srv >= 0, "server socket");
    CHECK(bind(srv, (struct sockaddr *)&a, alen) == 0, "bind abstract");
    CHECK(listen(srv, 8) == 0, "listen");

    // A second bind to the same name must be refused, not silently win.
    int dup_srv = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(dup_srv, (struct sockaddr *)&a, alen) == -1 && errno == EADDRINUSE,
          "duplicate bind refused");
    close(dup_srv);

    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(cli, (struct sockaddr *)&a, alen) == 0, "connect");
    int acc = accept(srv, NULL, NULL);
    CHECK(acc >= 0, "accept");

    CHECK(write(cli, "ping", 4) == 4, "client write");
    char buf[8] = {0};
    CHECK(read(acc, buf, 4) == 4 && memcmp(buf, "ping", 4) == 0, "server read");
    CHECK(write(acc, "pong", 4) == 4, "server write");
    CHECK(read(cli, buf, 4) == 4 && memcmp(buf, "pong", 4) == 0, "client read");

    // Closing one end must give the other EOF, not a hang.
    close(cli);
    CHECK(read(acc, buf, 4) == 0, "EOF after peer close");

    // Connecting to a name nobody bound is ECONNREFUSED.
    int orphan = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un nx = { .sun_family = AF_UNIX };
    memcpy(nx.sun_path, "\0nobody-here", 12);
    CHECK(connect(orphan, (struct sockaddr *)&nx,
                  offsetof(struct sockaddr_un, sun_path) + 12) == -1 &&
          errno == ECONNREFUSED, "connect to unbound name");

    close(orphan); close(acc); close(srv);
    say("unix socket ok");
}
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `bind abstract` fails, or `socket()` returns `-EAFNOSUPPORT`
(`socket_create` refuses every domain but `AF_INET` today).

- [ ] **Step 3: Implement**

A bound-name table (abstract names first, then `S_IFSOCK` vnodes), a
listen backlog queue, and connection objects pairing two pipe rings.
`accept4` honours `SOCK_CLOEXEC` and `SOCK_NONBLOCK`. `poll`/`epoll`
readiness comes from the pipe rings' existing poll heads, so
`epoll_wait` on a unix socket works without new machinery.

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `[uxtest] unix socket ok`.

- [ ] **Step 5: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15.

- [ ] **Step 6: Update docs/stdlib.md and commit**

```bash
git add kernel/ userland/uxtest.c docs/stdlib.md
git commit -m "ipc: real AF_UNIX stream sockets

AF_UNIX existed only as socketpair backed by two pipes; socket_create
refused every domain but AF_INET, so there was no bind, listen, accept
or connect and a server could only ever talk to processes it had
forked. Abstract-namespace names first (no filesystem node, which is
all the compositor needs), pathname binding via S_IFSOCK after. The
byte stream reuses pipe.c's rings rather than growing a second
implementation, which also means poll and epoll work with no new
machinery."
```

### Task 10: SCM_RIGHTS fd passing

**Files:**
- Modify: `kernel/ipc/unix_sock.c` (in-flight fd lists on queued
  messages), `kernel/net/socket.c` (`socket_sendmsg`/`socket_recvmsg`
  stop ignoring `msg_control`), `kernel/net/socket.h`

**Interfaces:**
- Consumes: AF_UNIX sockets from Task 9.
- Produces: Linux `cmsghdr` — `size_t cmsg_len; int cmsg_level; int
  cmsg_type;` then data, 8-byte aligned. `SOL_SOCKET` = 1,
  `SCM_RIGHTS` = 1, `MSG_CMSG_CLOEXEC` = 0x40000000.

- [ ] **Step 1: Write the failing userland test**

```c
static void test_scm_rights(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    int mfd = memfd_create("passed", 0);
    ftruncate(mfd, 4096);
    uint32_t *mine = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    mine[0] = 0xC0FFEE;

    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = { .iov_base = "x", .iov_len = 1 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type  = SCM_RIGHTS;
    c->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &mfd, sizeof(int));
    CHECK(sendmsg(sv[0], &m, 0) == 1, "sendmsg with SCM_RIGHTS");

    char rbuf[8], rcbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec riov = { .iov_base = rbuf, .iov_len = sizeof(rbuf) };
    struct msghdr rm = { .msg_iov = &riov, .msg_iovlen = 1,
                         .msg_control = rcbuf, .msg_controllen = sizeof(rcbuf) };
    CHECK(recvmsg(sv[1], &rm, 0) == 1, "recvmsg");

    struct cmsghdr *rc = CMSG_FIRSTHDR(&rm);
    CHECK(rc && rc->cmsg_level == SOL_SOCKET && rc->cmsg_type == SCM_RIGHTS,
          "cmsg came back");
    int got; memcpy(&got, CMSG_DATA(rc), sizeof(int));
    CHECK(got >= 0 && got != mfd, "a NEW descriptor, not the same number");

    // The received fd must name the same object, not a copy.
    uint32_t *theirs = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, got, 0);
    CHECK(theirs != MAP_FAILED, "map the received fd");
    CHECK(theirs[0] == 0xC0FFEE, "same object, not a copy");
    theirs[1] = 0xBEEF;
    CHECK(mine[1] == 0xBEEF, "and writes go both ways");

    // Closing the sender's fd must not invalidate the receiver's.
    close(mfd);
    CHECK(theirs[0] == 0xC0FFEE, "survives the sender closing");

    // A socket closed with an undelivered message must not leak the fd.
    int sv2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
    int leak = memfd_create("leak", 0);
    memcpy(CMSG_DATA(CMSG_FIRSTHDR(&m)), &leak, sizeof(int));
    sendmsg(sv2[0], &m, 0);
    close(sv2[0]); close(sv2[1]);      // never received
    close(leak);
    // If the in-flight reference leaked, the object is never freed.
    // Checked by the kernel-side [memfd] live-object count, below.

    munmap(mine, 4096); munmap(theirs, 4096);
    close(got); close(sv[0]); close(sv[1]);
    say("scm_rights ok");
}
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `cmsg came back` fails — `msg_control` is ignored today.

- [ ] **Step 3: Implement**

Sending parses the control buffer, resolves each fd, takes a reference,
and attaches the list to the queued message. Receiving installs them at
the lowest free descriptors, writes the `cmsghdr` back, and honours
`MSG_CMSG_CLOEXEC`. **Closing a socket must walk its queued messages
and release their in-flight references** — this is the part most likely
to leak, and the test above is written to catch exactly it.

Add a `memfd_live_count()` debug counter and assert it returns to its
starting value at the end of the kernel-side selftest, so the leak is
observable rather than theoretical.

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make uxtest
```
Expected: `[uxtest] scm_rights ok` and `[memfd] live count returned to
baseline`.

- [ ] **Step 5: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15.

- [ ] **Step 6: Update docs/stdlib.md and commit**

Record the divergence: Linux runs a garbage collector for cycles of
unix sockets holding references to each other; NeoOS does not, so a
deliberately constructed cycle leaks. Ordinary use does not.

```bash
git add kernel/ userland/uxtest.c docs/stdlib.md
git commit -m "ipc: SCM_RIGHTS fd passing over AF_UNIX

recvmsg ignored msg_control entirely -- a documented divergence that
had to end, because a compositor hands out surface buffers as file
descriptors. Sending takes a reference per fd onto the queued message;
receiving installs at the lowest free descriptors and honours
MSG_CMSG_CLOEXEC; closing a socket walks its undelivered messages and
releases their references, which is the case that leaks if forgotten,
so memfd gained a live-object count to make it observable.

No garbage collector for reference cycles between unix sockets, unlike
Linux. Recorded in stdlib.md."
```

### Task 11: Library wrappers for the new primitives

Per `CLAUDE.md`, a kernel feature reachable from userland is not
finished until it has a musl-visible path or a `lib/` wrapper plus a
`docs/stdlib.md` entry.

**Files:**
- Modify: `neoos-musl` arch shim — `ftruncate` (77), `memfd_create`
  (319), and confirm `socket`/`bind`/`listen`/`accept4`/`connect`/
  `sendmsg`/`recvmsg` already map
- Modify: `docs/stdlib.md`, `docs/abi-compatibility.md`

- [ ] **Step 1: Rebuild neoos-musl and confirm no ENOSYS remains**

```bash
cd ~/projects/personal/neoos-musl && make clean && make
cd ~/projects/personal/NeoOS && make uxtest
grep -n "ENOSYS" build/uxtest.log
```
Expected: no output from the grep, and `[uxtest] ALL PASSED`.

- [ ] **Step 2: Refresh docs/abi-compatibility.md**

Move `ftruncate`, `memfd_create`, AF_UNIX and `SCM_RIGHTS` from missing
to implemented; record `F_ADD_SEALS`, `VT_SETMODE`'s process-mode
handshake, and SCM_RIGHTS cycle GC as the remaining gaps.

- [ ] **Step 3: Commit**

```bash
git add docs/
git commit -m "docs: record G3's syscalls in stdlib and abi-compatibility"
```

---
## Phase G4 — `neoos-wm`, the compositor

NeoOS runs **stock `linux-musl-x64` NativeAOT binaries** — no custom
RID, no new target. The publish recipe is already established in
`docs/aspnet-missing-syscalls.md` and is reused verbatim below.

### Task 12: neoos-wm skeleton that builds, boots, and exits cleanly

Prove the toolchain end to end before writing any compositor logic.

**Files:**
- Create (new sibling repo `~/projects/personal/neoos-wm`):
  `NeoOS.Wm/NeoOS.Wm.csproj`, `NeoOS.Wm/Program.cs`,
  `Makefile`, `README.md`, `gcc-wrapper.sh`
- Modify: `~/projects/personal/NeoOS/Makefile` (a `wm` target that
  copies `wm.nex` onto the disk image and boots it alone)

**Interfaces:**
- Produces: `build/wm.nex`, a static NeoOS executable.

- [ ] **Step 1: Write the csproj and the failing smoke test**

`NeoOS.Wm.csproj`:

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <PublishAot>true</PublishAot>
    <InvariantGlobalization>true</InvariantGlobalization>
    <ServerGarbageCollection>false</ServerGarbageCollection>
    <StaticExecutable>true</StaticExecutable>
    <PositionIndependentExecutable>false</PositionIndependentExecutable>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
  </PropertyGroup>
  <ItemGroup>
    <ExtraLinkerArg Include="-T,$(NeoosUserLd)" />
    <ExtraLinkerArg Include="--no-relax" />
  </ItemGroup>
</Project>
```

`Program.cs`:

```csharp
// The whole point of this first version is to prove the toolchain, so
// it does exactly one observable thing and exits.
internal static class Program
{
    private static int Main()
    {
        Console.WriteLine("[wm] hello from NativeAOT");
        return 0;
    }
}
```

`gcc-wrapper.sh` — required, not optional. The .NET 10 ILCompiler
always appends clang's `--target=x86_64-linux-musl` to the link step,
the NeoOS cross **gcc** rejects it, and the `TargetTriple` property it
comes from is set inside the targets file so `-p:TargetTriple=` on the
command line does not clear it:

```sh
#!/bin/sh
# Drop any --target=* the ILCompiler appends; exec the real cross gcc.
for arg in "$@"; do
    case "$arg" in --target=*) ;; *) set -- "$@" "$arg" ;; esac
    shift
done
exec x86_64-neoos-linux-musl-gcc "$@"
```

- [ ] **Step 2: Publish and confirm it fails or produces a binary**

```bash
cd ~/projects/personal/neoos-wm && \
PATH=$HOME/opt/cross-x86_64-neoos/bin:$PATH \
dotnet publish -c Release -r linux-musl-x64 -p:PublishAot=true \
  -p:ServerGarbageCollection=false -p:InvariantGlobalization=true \
  -p:CppCompilerAndLinker=$PWD/gcc-wrapper.sh \
  -p:ObjCopyName=x86_64-neoos-linux-musl-objcopy \
  -p:StaticExecutable=true -p:PositionIndependentExecutable=false \
  -p:NeoosUserLd=$HOME/projects/personal/NeoOS/userland/user.ld
```
Expected on the first run: a link error naming the `--target` flag if
the wrapper is not wired, or a static ELF if it is.

- [ ] **Step 3: Boot it**

Add a `wm` target to NeoOS's `Makefile`, modelled on the existing
`epollet` target: `nexify.sh` the binary, `mcopy` it, write an
`INITTAB` containing only `wait /wm.nex`, boot headless, grep.

```bash
cd ~/projects/personal/NeoOS && make wm
```
Expected: `[wm] hello from NativeAOT` in `build/wm.log`, no `PANIC`, no
`[exception]`.

- [ ] **Step 4: Commit both repos**

```bash
cd ~/projects/personal/neoos-wm && git init && git add -A && \
git commit -m "wm: NativeAOT skeleton that builds and boots on NeoOS

Stock linux-musl-x64 -- NeoOS's Linux ABI conformance means no custom
RID is needed. The gcc wrapper is load-bearing: the ILCompiler always
appends clang's --target=x86_64-linux-musl and the cross gcc rejects
it, and the property it comes from cannot be cleared from the command
line."
cd ~/projects/personal/NeoOS && git add Makefile && \
git commit -m "make: wm target -- boot neoos-wm alone and capture serial"
```

### Task 13: The wire protocol, in C# and in lib/

Both sides of the protocol before either side has logic, so the two
implementations are written against the same definition.

**Files:**
- Create: `neoos-wm/NeoOS.Wm.Protocol/Messages.cs`
- Create: `~/projects/personal/NeoOS/lib/neoos_wm_client.c`,
  `lib/neoos_wm_client.h`
- Modify: `docs/stdlib.md`

**Interfaces:**
- Produces, shared by Tasks 14–18. Fixed-size little-endian structs, an
  8-byte header (`uint16 type; uint16 length; uint32 surface_id;`)
  followed by a per-type body:

  | type | direction | body |
  |---|---|---|
  | 1 `Hello` | c→s | `uint32 version` (currently 1) |
  | 2 `CreateSurface` | c→s | `uint32 width; uint32 height` |
  | 3 `AttachBuffer` | c→s | `uint32 stride; uint32 format` + one memfd via `SCM_RIGHTS` |
  | 4 `Damage` | c→s | `int32 x, y; uint32 w, h` |
  | 5 `Commit` | c→s | — |
  | 6 `SetTitle` | c→s | `uint16 len` + UTF-8 bytes |
  | 7 `DestroySurface` | c→s | — |
  | 128 `Configure` | s→c | `uint32 width; uint32 height` |
  | 129 `PointerMotion` | s→c | `int32 x, y` (surface-relative) |
  | 130 `PointerButton` | s→c | `uint16 button; uint16 state` |
  | 131 `Key` | s→c | `uint16 keycode; uint16 state` |
  | 132 `Focus` | s→c | `uint32 focused` |
  | 133 `Close` | s→c | — |

  `format` is `1` = `XRGB8888`, the only value G4–G6 use: 32bpp,
  opaque, no alpha, no blending. `ARGB8888` premultiplied would be `2`
  and is not implemented.

- [ ] **Step 1: Write the failing round-trip test**

A host-runnable xUnit test is possible here because the protocol codec
is pure managed code with no syscalls — the one part of this project
that *can* be tested on the host. Use that.

```csharp
[Fact]
public void CreateSurface_RoundTrips()
{
    var sent = new CreateSurface(SurfaceId: 7, Width: 320, Height: 240);
    Span<byte> buf = stackalloc byte[Message.MaxSize];
    int n = sent.Encode(buf);

    Assert.True(Message.TryDecode(buf[..n], out var got, out int consumed));
    Assert.Equal(n, consumed);
    var cs = Assert.IsType<CreateSurface>(got);
    Assert.Equal(7u, cs.SurfaceId);
    Assert.Equal(320u, cs.Width);
    Assert.Equal(240u, cs.Height);
}

[Fact]
public void TryDecode_ReturnsFalse_OnPartialMessage()
{
    // A stream socket splits messages anywhere; the decoder must ask
    // for more rather than misread a truncated header as a short one.
    Span<byte> buf = stackalloc byte[Message.MaxSize];
    int n = new CreateSurface(1, 10, 10).Encode(buf);
    for (int cut = 1; cut < n; cut++)
        Assert.False(Message.TryDecode(buf[..cut], out _, out _));
}

[Fact]
public void Decode_RejectsLengthLongerThanBuffer()
{
    // A hostile or buggy client must not steer a read past the buffer.
    Span<byte> buf = stackalloc byte[8];
    BinaryPrimitives.WriteUInt16LittleEndian(buf, 2);        // CreateSurface
    BinaryPrimitives.WriteUInt16LittleEndian(buf[2..], 4096); // absurd length
    Assert.False(Message.TryDecode(buf, out _, out _));
}
```

- [ ] **Step 2: Run and confirm it fails**

```bash
cd ~/projects/personal/neoos-wm && dotnet test
```
Expected: compile failure — `Message` does not exist.

- [ ] **Step 3: Implement the codec, then the C client**

`Messages.cs` with `Encode`/`TryDecode` over `Span<byte>` and
`BinaryPrimitives` (explicit little-endian, never `BitConverter`).
`lib/neoos_wm_client.c` mirrors it: `wm_connect`, `wm_create_surface`,
`wm_attach_buffer` (does the `sendmsg`/`SCM_RIGHTS` dance),
`wm_damage`, `wm_commit`, `wm_next_event`.

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/neoos-wm && dotnet test
```
Expected: all green.

- [ ] **Step 5: Document and commit**

`docs/stdlib.md` gains the protocol and the `lib/` wrapper as a NeoOS
extension with no POSIX analogue.

### Task 14: The compositor core

**Files:**
- Create: `neoos-wm/NeoOS.Wm/Framebuffer.cs`, `Surface.cs`,
  `Compositor.cs`, `Server.cs`
- Modify: `neoos-wm/NeoOS.Wm/Program.cs`

**Interfaces:**
- Consumes: the protocol from Task 13; `KDSETMODE` from G1; AF_UNIX,
  `memfd`, `SCM_RIGHTS` from G3.
- Produces: `Compositor.Run()`; a bound abstract socket named
  `\0neoos-wm`.

- [ ] **Step 1: Write the failing end-to-end test**

The test is a second process. Add `neoos-wm/test-client/` — a small C#
program that connects, creates a 100x100 surface, fills it with a known
colour, commits, and exits. The compositor prints a checksum of the
region it composited:

```csharp
// Program.cs in test-client
var wm = WmClient.Connect();
var surface = wm.CreateSurface(100, 100);
var px = surface.Map();                     // memfd, mapped MAP_SHARED
for (int i = 0; i < 100 * 100; i++) px[i] = 0x00FF8000;   // known colour
surface.Damage(0, 0, 100, 100);
surface.Commit();
Console.WriteLine("[wmtest] committed");
```

and in the compositor, after each composite pass:

```csharp
Console.WriteLine($"[wm] composited {_surfaces.Count} surface(s), checksum {sum:X8}");
```

The expected checksum for one 100x100 surface of `0x00FF8000` is
computable and asserted in the `make wm` target's grep.

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make wm
```
Expected: `[wmtest]` never appears, or the compositor never prints a
composite line.

- [ ] **Step 3: Implement**

- `Framebuffer.cs`: open `/dev/fb0`, `FBIOGET_VSCREENINFO` and
  `FBIOGET_FSCREENINFO` for geometry and stride, `mmap` it, and claim
  the screen with `KDSETMODE(KD_GRAPHICS)` on `/dev/tty0`. **Restore
  `KD_TEXT` in a `finally`** — G1 Task 1 makes the kernel restore it on
  death anyway, but a clean exit should not rely on that.
- `Surface.cs`: id, geometry, the mapped memfd, the pending damage
  rectangle list.
- `Compositor.cs`: the window stack, and a composite pass that walks
  **only the damaged rectangles**. Damage tracking is not an
  optimisation here; a full 1280x800 software copy per frame under TCG
  is not viable.
- `Server.cs`: the accept loop and per-client message dispatch, over
  `epoll` (which works on unix sockets because G3 built them on pipe
  rings that already have poll heads).

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make wm
```
Expected: `[wmtest] committed` and a `[wm] composited 1 surface(s)`
line with the expected checksum.

- [ ] **Step 5: Commit**

### Task 15: Input routing, focus, and a cursor

**Files:**
- Create: `neoos-wm/NeoOS.Wm/InputRouter.cs`, `Cursor.cs`
- Modify: `Compositor.cs`

**Interfaces:**
- Consumes: `/dev/input/event0` and `/dev/input/event1` from G2.
- Produces: `PointerMotion`/`PointerButton`/`Key`/`Focus` messages to
  clients.

- [ ] **Step 1: Write the failing test**

Extend the test client to log the events it receives, and drive input
from the kernel side using the existing `input_inject_key` test hook
plus a new `mouse_post_packet` call, both already built in G2:

```csharp
// test-client
foreach (var ev in wm.Events())
    Console.WriteLine($"[wmtest] event {ev}");
```

Expected markers: `[wmtest] event PointerMotion`, then
`[wmtest] event PointerButton`, then `[wmtest] event Key`.

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make wm
```
Expected: no `[wmtest] event` lines.

- [ ] **Step 3: Implement**

`InputRouter` reads both event fds (blocking reads, which G2 Task 4
made actually block), accumulates `REL_X`/`REL_Y` into an absolute
cursor position clamped to the screen, hit-tests the window stack to
find the surface under the cursor, and sends surface-relative
coordinates. Keyboard events go to the focused surface, which is set by
a click and is the topmost surface otherwise. The cursor is drawn last,
into the damage region, and its previous position is added to the
damage list so it does not smear.

- [ ] **Step 4: Run and confirm it passes**

Expected: all three `[wmtest] event` markers.

- [ ] **Step 5: Commit**

---

## Phase G5 — `neoos-lvgl` and its C# bindings

### Task 16: Cross-build liblvgl.a and prove it renders fullscreen

Prove LVGL works on NeoOS in C, with no C# and no compositor in the
picture, so that a later failure has only one possible cause.

**Files:**
- Create (new sibling repo `~/projects/personal/neoos-lvgl`):
  `Makefile`, `lv_conf.h`, `upstream/` (LVGL 9.x), `demo/fullscreen.c`,
  `README.md`

**Interfaces:**
- Produces: `build/liblvgl.a`.

- [ ] **Step 1: Write lv_conf.h and the fullscreen demo**

`lv_conf.h` essentials: `LV_COLOR_DEPTH 32`, `LV_USE_OS LV_OS_NONE`,
`LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB` (musl's), `LV_USE_LINUX_FBDEV 1`,
`LV_USE_EVDEV 1`, and the widgets the hello-world app needs
(`LV_USE_LABEL`, `LV_USE_BTN`, `LV_USE_FLEX`).

`demo/fullscreen.c` uses `lv_linux_fbdev_create` and
`lv_linux_fbdev_set_file(disp, "/dev/fb0")` — **the only place in this
project where an application touches `/dev/fb0` directly.** It exists
to isolate "does LVGL render at all" from "does the compositor work".

- [ ] **Step 2: Build and confirm it fails**

```bash
cd ~/projects/personal/neoos-lvgl && make
```
Expected: compile errors from LVGL's Linux backend against the NeoOS
headers, to be worked through.

- [ ] **Step 3: Fix the port until it links**

Expect gaps to surface the way .NET's port did: run it, read
`[shim] ENOSYS <n>`, implement or stub, repeat. **Rebuild `neoos-musl`
before concluding a syscall is missing.**

- [ ] **Step 4: Boot it and confirm it renders**

Add an `lvgldemo` target to NeoOS's `Makefile` on the `epollet` model.
Expected: `[lvgl] fullscreen demo drew N frames` on serial, and — run
once with a display rather than headless — a visible label.

- [ ] **Step 5: Commit**

### Task 17: C# bindings and DirectPInvoke static linking

**Files:**
- Create: `neoos-lvgl/bindings/NeoOS.Lvgl/Lvgl.cs`,
  `NeoOS.Lvgl.csproj`
- Create: `neoos-lvgl/bindings/smoke/Program.cs`

**Interfaces:**
- Produces: `NeoOS.Lvgl` — P/Invoke declarations for the subset used:
  `lv_init`, `lv_display_create`, `lv_display_set_buffers`,
  `lv_display_set_flush_cb`, `lv_display_flush_ready`,
  `lv_indev_create`, `lv_indev_set_type`, `lv_indev_set_read_cb`,
  `lv_screen_active`, `lv_label_create`, `lv_label_set_text`,
  `lv_obj_center`, `lv_timer_handler`, `lv_tick_inc`.

- [ ] **Step 1: Write the failing smoke test**

A C# program that calls `lv_init()` and creates a label on a 64x64
in-memory display, then prints the first non-zero pixel:

```csharp
Lvgl.lv_init();
var disp = Lvgl.lv_display_create(64, 64);
// ... set buffers to a pinned managed array, flush_cb writes into it
var label = Lvgl.lv_label_create(Lvgl.lv_screen_active());
Lvgl.lv_label_set_text(label, "hi");
Lvgl.lv_timer_handler();
Console.WriteLine($"[lvglsmoke] first non-zero pixel at {index}");
```

- [ ] **Step 2: Publish and confirm it fails**

Expected: a link error for the `lv_*` symbols — the P/Invokes are
declared but nothing is linked.

- [ ] **Step 3: Wire DirectPInvoke**

In `NeoOS.Lvgl.csproj`:

```xml
<ItemGroup>
  <DirectPInvoke Include="lvgl" />
  <NativeLibrary Include="$(NeoosLvglDir)/build/liblvgl.a" />
</ItemGroup>
```

`DirectPInvoke` is what makes this work at all: it resolves the
P/Invokes at **link** time into the static archive. A normal
`DllImport` would need `dlopen`, and NeoOS's ELF loader handles only
`PT_LOAD` and `PT_TLS` — there is no dynamic linker.

Callbacks passed to LVGL (`flush_cb`, `read_cb`) must be
`[UnmanagedCallersOnly]` static methods, not delegates: NativeAOT
cannot marshal a managed delegate to a function pointer without a
reverse P/Invoke stub, and `[UnmanagedCallersOnly]` is how you ask for
one.

- [ ] **Step 4: Boot and confirm it passes**

Expected: `[lvglsmoke] first non-zero pixel at <n>` with `n` inside the
buffer.

- [ ] **Step 5: Commit**

### Task 18: Bind LVGL's display and input to a compositor surface

**Files:**
- Create: `neoos-lvgl/bindings/NeoOS.Lvgl.Wm/WmDisplay.cs`

**Interfaces:**
- Consumes: `NeoOS.Lvgl` (Task 17), `WmClient` (Task 13).
- Produces: `WmDisplay.Create(WmClient wm, int w, int h)` — an LVGL
  display whose draw buffer is the surface's mapped memfd, and an
  `lv_indev` fed from compositor events.

- [ ] **Step 1: Write the failing test**

Extend the test client to draw an LVGL label into a *windowed* surface
and assert the compositor's checksum changes from the blank-surface
value.

- [ ] **Step 2: Run and confirm it fails**

- [ ] **Step 3: Implement**

`flush_cb` converts LVGL's dirty area into a `Damage` message and a
`Commit`, then calls `lv_display_flush_ready`. `read_cb` drains the
compositor's pointer events into `lv_indev_data_t`. **Applications do
not use `lv_evdev_create` here** — the compositor owns
`/dev/input/*`, and an app reading it directly would bypass focus
entirely.

- [ ] **Step 4: Run and confirm it passes**

- [ ] **Step 5: Commit**

---

## Phase G6 — The deliverable

### Task 19: Hello World in a window

**Files:**
- Create: `neoos-lvgl/examples/hello/Program.cs`
- Modify: `~/projects/personal/NeoOS/Makefile` (a `gui` target booting
  `wm.nex` plus `hello.nex`)

- [ ] **Step 1: Write the app**

```csharp
var wm = WmClient.Connect();
var display = WmDisplay.Create(wm, 400, 200);
wm.SetTitle("Hello");

var label = Lvgl.lv_label_create(Lvgl.lv_screen_active());
Lvgl.lv_label_set_text(label, "Hello World");
Lvgl.lv_obj_center(label);

while (wm.Pump())
{
    Lvgl.lv_timer_handler();
    Thread.Sleep(16);
}
```

- [ ] **Step 2: Boot both and capture**

```bash
cd ~/projects/personal/NeoOS && make gui
```
Expected: `[wm] composited 1 surface(s)`, no `PANIC`, no
`[exception]`, and — run once with a display — a 400x200 window
containing "Hello World".

- [ ] **Step 3: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15.

- [ ] **Step 4: Commit**

### Task 20: os-builder integration and the ABI refresh

Per `CLAUDE.md`, a port nobody can select through the tool that exists
to select ports is not finished.

**Files:**
- Modify: `~/projects/personal/neoos-os-builder/scripts/build.sh`,
  `docs/BUILD_ORDER.md`, `config/gui.yaml` (new)
- Modify: `~/projects/personal/NeoOS/docs/abi-compatibility.md`

- [ ] **Step 1: Teach build.sh the new ports**

`neoos-lvgl` and `neoos-wm` are new ports. The per-port build loop
currently passes only `MUSL_DIR` and has **no notion of build order**,
which is already known-broken for `openssl`/`libssh2`/`curl`. `neoos-wm`
depends on nothing but musl; `neoos-lvgl`'s bindings depend on
`neoos-wm`'s protocol assembly, so the loop must build `neoos-wm`
first and pass `WM_DIR=` to `neoos-lvgl`.

- [ ] **Step 2: Add config/gui.yaml**

Modelled on `busybox-doom.yaml`, listing `wm` and `lvgl`.

- [ ] **Step 3: Update docs/BUILD_ORDER.md**

Add the new chain to the pipeline diagram and dependency list.

- [ ] **Step 4: Build an image through os-builder and boot it**

```bash
cd ~/projects/personal/neoos-os-builder && ./scripts/build.sh config/gui.yaml
```
Expected: an image that boots to the compositor with the hello window.

- [ ] **Step 5: Refresh docs/abi-compatibility.md**

What of the Linux ABI is implemented, what is stubbed, what diverges
and why, what a real ported application would still hit. The four
divergences this plan introduces: in-kernel VT screen ownership instead
of `VT_SETMODE`/`VT_PROCESS`, `-EBUSY` from an inactive VT's
`fb_write`/`fb_mmap`, no `F_ADD_SEALS`, and no `SCM_RIGHTS` cycle GC.

- [ ] **Step 6: Commit all three repos**

---

## Self-Review Notes

- **Spec coverage.** G1 Tasks 1–2 cover both measured gaps. G2 Tasks
  3–6 cover the `input_dev` refactor, the blocking-read fix, the
  decoder, and `event1`. G3 Tasks 7–11 cover `ftruncate`,
  `memfd_create`, shared `mmap`, AF_UNIX and `SCM_RIGHTS`, plus the
  `lib/`+`stdlib.md` obligation. G4 Tasks 12–15, G5 Tasks 16–18 and G6
  Tasks 19–20 cover the compositor, LVGL, the deliverable and the
  os-builder obligation.
- **Naming consistency.** `input_post`, `evdev_client_dev`,
  `mouse_post_packet`, `vt_process_owns_screen`, `vt_selftest_kd_mode`,
  `file_truncate`, `memfd_live_count`, `WmClient`, `WmDisplay` and
  `Message.TryDecode` are each defined once and used with the same
  signature everywhere.
- **Known soft spot.** Tasks 14, 15 and 18 specify structure and the
  observable markers rather than complete source — a compositor does
  not fit in a plan document. Their test markers are exact, which is
  what a reviewer gates on.
