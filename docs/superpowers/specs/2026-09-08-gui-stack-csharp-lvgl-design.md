# NeoOS GUI stack: a C# compositor and LVGL as the UI kit

## Status

Supersedes the Flutter half of
`2026-09-08-gui-stack-flutter-design.md`. That document's *measured
facts* remain valid and are not repeated in full here -- the `/dev/fb0`
fbdev ABI status, the W^X/JIT finding, and the software-rendering
argument all still hold. What changed is the answer to "what draws the
UI", and with it the cost of the whole project.

Nothing in this document is implemented yet.

## Why the plan changed

The Flutter plan's critical path was building the Flutter engine --
Skia plus the Dart VM plus an embedder, under a GN target for a triple
that does not exist -- and that single item dominated the schedule.

Three facts make it unnecessary:

1. **NeoOS already runs .NET NativeAOT.** Threads, the GC, TCP and
   ASP.NET Core/Kestrel all work. The runtime that was going to be the
   expensive part of a Flutter port is, for C#, already paid for.
2. **NativeAOT's `DirectPInvoke` links native libraries statically**
   (`<DirectPInvoke Include="lvgl" />` plus
   `<NativeLibrary Include="liblvgl.a" />`), resolving calls at link
   time. This matters more here than anywhere else: NeoOS's ELF loader
   handles only `PT_LOAD` and `PT_TLS`, so there is no dynamic linker
   and no `dlopen`. An ordinary `DllImport` could not work; a direct
   P/Invoke to a static archive can.
3. **LVGL's two supported Linux backends are `/dev/fb0` and
   `/dev/input/event*`** -- precisely the two interfaces NeoOS has.
   Porting it is close to "compile it with the cross toolchain".

So the UI kit becomes a C library bound from C#, and the compositor
becomes a C# program. No new language runtime, no engine build.

## What is deliberately not being done

- **No Flutter, no Dart, no Skia.** The engine build is off the plan.
- **No Nim.** An earlier draft made the compositor Nim and Nim a
  first-class NeoOS language. .NET is already first-class; Nim would
  have been a second runtime port for no gain here.
- **No Wayland.** Its protocol is enormous and none of its ecosystem is
  reachable from NeoOS anyway. The compositor protocol is NeoOS's own,
  small, and documented in `docs/stdlib.md` as an extension.
- **No DRM/KMS, GBM or Mesa.** Software rendering to `/dev/fb0`
  throughout. `flutter-pi` was investigated and rejected for exactly
  this reason: it hard-requires libdrm, gbm, EGL/GLES2, libsystemd,
  libinput, libudev and libxkbcommon, all REQUIRED in its CMake, and
  its README states it needs hardware 3D acceleration.
- **LVGL is an embedded GUI library and will look like one.** Avalonia
  on its `Avalonia.LinuxFramebuffer` backend is the upgrade path if
  that becomes unacceptable; it would need a static `libSkiaSharp`
  built for NeoOS, and nothing in G1-G4 would have to change.

## Measured state of the kernel

Established by reading the tree on 2026-09-08, not assumed:

| capability | state |
|---|---|
| `/dev/fb0` | 32bpp linear, full Linux fbdev ABI (`FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, `mmap`). `FBIOPUT_VSCREENINFO` returns `-EINVAL` -- no mode setting. |
| Screen arbitration | **none.** The kernel console and a graphical app both paint `/dev/fb0` and overwrite each other. |
| evdev | `/dev/input/event0`, keyboard only. Single global client list and a single global grab in `kernel/drivers/input/input.c` -- the subsystem has no notion of more than one device. |
| Mouse | **absent.** |
| `evdev_client_read` | **never blocks.** Returns `-EAGAIN` even for a blocking fd; the in-tree comment attributes this to lock ordering. `poll`/`epoll` work, which is why nothing has hit it yet. |
| devfs | `input/event0` reached through hardcoded inode ids (5 and 6) in `devfs_lookup`/`devfs_readdir`, with a comment warning that they must not shift. |
| `mmap` | anonymous only, plus a special case for a device fd whose `file_ops` implements `mmap` (today only `/dev/fb0`). Non-anonymous otherwise returns `-ENOSYS`. |
| `ftruncate` | **absent** -- no such syscall number. |
| `memfd_create` | **absent.** |
| AF_UNIX | `socketpair(2)` only, backed by two pipes. `socket_create` refuses every domain but `AF_INET`, so there is no `bind`/`listen`/`accept`/`connect` on a unix socket. |
| `sendmsg`/`recvmsg` | present, but `msg_control` is ignored entirely. No `SCM_RIGHTS`. Already recorded as a divergence in `docs/stdlib.md`. |
| ELF loading | `PT_LOAD` and `PT_TLS` only. No `PT_INTERP`, no `ET_DYN` relocation. **Static binaries only; no dynamic linker, no `dlopen`.** |
| W^X | `vma_mmap_locked` refuses `W|X`; `mprotect` refuses the transition. AOT only, which NativeAOT already satisfies. |

## Milestones

Each gets its own implementation plan. They are listed in dependency
order; G1, G2 and G3 are independent of each other and can be done in
any order or in parallel.

### G1 -- Framebuffer ownership

The blocker the gears demo surfaced: one process must be able to claim
the screen and have the kernel console stop painting.

Done the Linux way rather than with a bespoke ioctl: **`KDSETMODE` with
`KD_GRAPHICS` on the tty**, which is exactly how real Linux fbdev
applications claim the display. `KD_TEXT` restores the console and
forces a full repaint.

- `KDSETMODE` = `0x4B3A`, `KDGETMODE` = `0x4B3B`, `KD_TEXT` = 0,
  `KD_GRAPHICS` = 1. Linux values, because an unpatched Linux fbdev
  program must do the right thing here.
- **Restoration on death is mandatory.** The mode is owned by the tty,
  and a process exiting or crashing while its VT is in `KD_GRAPHICS`
  must leave the machine with a usable console. Tie restoration to tty
  release, not to an explicit call the app might never reach.
- VT switching (`Alt+Fn`, already implemented) must keep working. A
  process whose VT is not active must not reach the framebuffer:
  `/dev/fb0` writes and its `mmap`ped pages are gated on the owning
  VT being the active one.
- **Divergence to record:** Linux negotiates this with
  `VT_SETMODE`/`VT_PROCESS` and a signal handshake. NeoOS enforces it
  in the kernel instead. The observable difference is that an app does
  not get told it lost the screen; it is simply prevented from
  painting.

Verified by a headless QEMU run plus a serial-log selftest, per project
convention.

### G2 -- PS/2 mouse, and an input subsystem that holds two devices

`input.c` is hard-wired to a single device: one global client list, one
global grab, and every event goes to every client. A mouse cannot be
bolted onto that -- `event0` would start emitting pointer events.

- Introduce `struct input_dev` holding `{clients, grab, name, ev/key/rel
  bitmaps}`. Keyboard and mouse become two instances; `evdev_client`
  gains a back-pointer to its device. `evdev.c`'s hardcoded
  `"NeoOS AT keyboard"` name and EV bitmaps become device-parameterised.
  No behaviour change for the keyboard -- the existing `input_selftest`
  is the regression check.
- `mouse.c`/`mouse.h`: i8042 aux port enable (`0xA8`, status-byte IRQ12
  bit), `0xD4`-prefixed device commands (`0xF6` set defaults, `0xF4`
  enable), the IntelliMouse `200/100/80` sample-rate knock to negotiate
  4-byte packets with a scroll wheel, and a packet state machine.
- Events, at Linux values: `EV_REL` `0x02` with `REL_X` `0x00`,
  `REL_Y` `0x01`, `REL_WHEEL` `0x08`; `BTN_LEFT` `0x110`,
  `BTN_RIGHT` `0x111`, `BTN_MIDDLE` `0x112`. `EVIOCGBIT(EV_REL)` is
  ioctl nr `0x22`.
- `VECTOR_MOUSE` `0x2C` dispatched in `isr.c`; IOAPIC redirection for
  GSI 12 in `kernel.c`, mirroring the existing keyboard routing.
- `devfs`: add `input/event1`. The hardcoded inode ids for the input
  directory must be untangled rather than worked around.
- **Fix `evdev_client_read` to actually block**: drop the lock,
  `waitq_wait` on `c->readers`, re-check. The compositor will do a
  blocking read on the event fd; today that spins.
- Selftests: a pure packet-decoder test in the style of
  `keyboard_decode_selftest` (byte sequences in, events out, no
  hardware), plus fan-out isolation -- a key event must not appear on
  `event1`, a mouse event must not appear on `event0`.

### G3 -- Kernel IPC: AF_UNIX sockets, memfd, and SCM_RIGHTS

The largest kernel milestone. A windowing system needs unrelated
processes to connect to the compositor and hand it buffers; nothing in
the tree supports either half today.

**AF_UNIX sockets.** `socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|
SOCK_NONBLOCK, 0)`, `bind`, `listen`, `accept4`, `connect`, with
Linux's `struct sockaddr_un` layout (`sa_family_t sun_family;
char sun_path[108]`).

- Support the **abstract namespace** (`sun_path[0] == '\0'`) first: it
  is Linux-shaped, needs no filesystem node, and is enough for the
  compositor. Pathname binding, which creates an `S_IFSOCK` node,
  follows.
- Reuse `kernel/ipc/pipe.c`'s ring buffers for the byte stream the way
  `socketpair.c` already does, rather than inventing a second one.

**`memfd_create(name, flags)`** with `MFD_CLOEXEC` = 1 and
`MFD_ALLOW_SEALING` = 2. Returns an fd on an anonymous shared memory
object.

**`ftruncate`** -- a new syscall, needed to size a memfd before mapping
it.

**Shared `mmap` of a memfd.** This rides the extension point that
already exists: `sys_mmap` dispatches to `file_ops.mmap` for a device
fd, which is how `/dev/fb0` works. A memfd gets an `mmap` op that maps
its frames, so two processes mapping the same object share frames. No
general file-backed mmap is required.

**`SCM_RIGHTS`.** Linux `cmsghdr` layout (`size_t cmsg_len; int
cmsg_level; int cmsg_type;` then data, 8-byte aligned), `SOL_SOCKET` =
1, `SCM_RIGHTS` = 1.

- Sending duplicates the fds into an in-flight list attached to the
  queued message. Receiving installs them at the lowest free
  descriptors and honours `MSG_CMSG_CLOEXEC`.
- **Closing a socket with undelivered messages must release the
  in-flight fds**, or every dropped message leaks a file.
- **Divergence to record:** Linux runs a garbage collector for cycles
  of unix sockets holding references to each other. NeoOS will not. A
  deliberately constructed cycle leaks; ordinary use does not.

### G4 -- `neoos-wm`, the compositor

A C# NativeAOT program. Owns the screen, composites client surfaces,
routes input.

- Claims the framebuffer via G1, maps `/dev/fb0`, reads `event0` and
  `event1` via G2, listens on an abstract AF_UNIX address via G3.
- Maintains a window stack, draws simple decorations and a cursor, and
  blits only damaged regions -- damage tracking is not an optimisation
  here, it is what makes software compositing viable at 1280x800.
- No LVGL. A compositor needs no widget set, and keeping LVGL out of it
  keeps LVGL purely the *application* toolkit.

**Protocol.** Small, fixed-size, little-endian binary messages over the
unix socket. Buffers travel as memfds over `SCM_RIGHTS`.

- Client to server: `Hello(version)`, `CreateSurface(id, w, h)`,
  `AttachBuffer(id, fd, stride, format)`, `Damage(id, x, y, w, h)`,
  `Commit(id)`, `SetTitle(id, utf8)`, `DestroySurface(id)`.
- Server to client: `Configure(id, w, h)`, `PointerMotion(x, y)`,
  `PointerButton(button, state)`, `Key(keycode, state)`,
  `Focus(id, bool)`, `Close(id)`.
- Pixel format is 32bpp `XRGB8888`, matching `/dev/fb0`: opaque
  surfaces, no alpha channel, no blending in the compositor. If
  translucency is wanted later it arrives as a second format,
  `ARGB8888` premultiplied, negotiated in `AttachBuffer`.

**Per CLAUDE.md**, this is a NeoOS-native feature with no POSIX
analogue, so it needs a `lib/` wrapper -- a C client library for
non-C# clients alongside the C# one -- and a `docs/stdlib.md` entry.

First client is a trivial C# program that fills a rectangle. It proves
ownership, the protocol, buffer sharing and input routing with zero
UI-toolkit risk, and it stays in the tree as the compositor's smoke
test.

### G5 -- `neoos-lvgl` and its C# bindings

A port repo that builds `liblvgl.a` with `x86_64-neoos-linux-musl-gcc`.

- `lv_conf.h` tuned for NeoOS: `LV_COLOR_DEPTH 32`, no OS integration
  layer, malloc from musl.
- C# P/Invoke bindings for the subset actually used, wired with
  `DirectPInvoke` and `NativeLibrary` so they link statically.
- A windowed app's LVGL display buffer **is** its compositor surface,
  handed over through `lv_display_set_flush_cb`. Windowed
  applications never open `/dev/fb0`.
- Input reaches LVGL as an `lv_indev` fed from compositor events, not
  from `lv_evdev_create`. The compositor owns `/dev/input/*`; an app
  reading it directly would bypass focus entirely.
- `lv_linux_fbdev_create` and `lv_evdev_create` are used **only** for
  the fullscreen bring-up test, before the compositor is involved, as
  the cheapest possible check that LVGL renders at all.

### G6 -- A window with a Hello World label

The deliverable: `neoos-wm` running, a C# LVGL client connected, one
window, one label. Captured from a headless QEMU run.

## Obligations this milestone incurs

Non-optional, per CLAUDE.md:

- **`docs/stdlib.md`**: `memfd_create`, `ftruncate`, AF_UNIX sockets,
  `SCM_RIGHTS`, `KDSETMODE`/`KDGETMODE`, `/dev/input/event1`, and the
  compositor protocol with its `lib/` wrapper. Plus the three
  divergences named above -- VT ownership without `VT_SETMODE`, no
  SCM_RIGHTS cycle GC, and no fbdev mode setting.
- **`docs/abi-compatibility.md`**: refreshed at the end of the
  milestone.
- **`neoos-os-builder`**: `neoos-lvgl` and `neoos-wm` are new ports.
  `scripts/build.sh` must learn to build them and to pass the right
  `<DEP>_DIR=` variables in dependency order; `docs/BUILD_ORDER.md`
  needs the new chain; an example `config/*.yaml` must demonstrate
  them. A port nobody can select through the tool that exists to
  select ports is not finished.

## Risks

- **G3 is the schedule.** Four new syscalls, a new socket family and
  fd-passing, all in the kernel, all with lifetime rules that leak or
  corrupt if they are got wrong. Everything else here is days.
- **In-flight fd lifetime** is the specific part most likely to
  produce a subtle leak: a socket closed with queued, undelivered
  `SCM_RIGHTS` messages.
- **GC pauses in the compositor.** .NET's collector will occasionally
  stall a frame. Acceptable at this stage; measure rather than
  pre-optimise.
- **Software compositing at 1280x800 under TCG is slow.** Damage
  tracking is the mitigation, and KVM changes the picture entirely.
- **LVGL's look.** It is an embedded GUI library. If that becomes the
  objection, Avalonia on `Avalonia.LinuxFramebuffer` is the documented
  upgrade, and it needs only a static `libSkiaSharp` for NeoOS -- G1
  through G4 are unaffected either way.
