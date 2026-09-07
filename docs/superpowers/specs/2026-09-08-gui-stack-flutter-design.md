# NeoOS GUI stack: software rendering, Flutter, and the road to a compositor

## Status

**Milestone 0 is DONE and landed.** Software OpenGL runs on NeoOS:
`neoos-tinygl` renders Brian Paul's gears at 1280x800 into `/dev/fb0`,
depth-tested and lit, and the scene appears on the real screen. See that
repo's README for why TinyGL rather than Mesa.

The rest of this document is the plan for what the user actually asked
for: **a Flutter application running on NeoOS**, with the eventual goal
of the whole UI, desktop and shell being written in Flutter, and a
windower/compositor after that.

Nothing past Milestone 0 is implemented. This spec exists so the next
session starts from measured facts rather than from scratch.

## The single most important finding

**Flutter does not need OpenGL.** The Flutter embedder API
(`flutter_embedder.h`) offers three renderer configurations: OpenGL,
Metal/Vulkan, and **software**. `FlutterSoftwareRendererConfig` asks the
embedder for exactly one thing:

```c
bool (*surface_present_callback)(void *user_data,
                                 const void *allocation,
                                 size_t row_bytes,
                                 size_t height);
```

That is a pointer to a premultiplied-BGRA8888 buffer and its stride —
i.e. precisely a framebuffer blit. `neoos-fb/tglfb.c` in the TinyGL port
already does the `/dev/fb0` half of that in about a hundred lines, and
the same code serves here with the colour order adjusted.

So the software-GL port and the Flutter port are **parallel tracks, not
sequential ones**. TinyGL is worth having on its own merits (games,
demos, anything wanting GL 1.x) but it is not on Flutter's critical
path, and no version of "port Mesa first" is.

## What NeoOS already provides, measured

| need | status |
|---|---|
| Linear framebuffer | `/dev/fb0`, 32bpp, **the full Linux fbdev ABI** — `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, `mmap`, read/write/lseek. Unmodified Linux framebuffer code works against it. |
| Input | `kernel/drivers/input/` — evdev, keyboard. Mouse is the gap (see Risks). |
| Threads | musl `pthread_create` over `sys_clone`. Real, and exercised by .NET's thread pool. |
| C++17 | `x86_64-neoos-linux-musl-g++` 9.4.0, `libstdc++.a` present. A static C++17 binary using `<vector>`, `<string>` and `<memory>` links and runs. |
| epoll / poll / timers | present; `epolltcp` exercises epoll under concurrency. |
| `mmap`/`mprotect`/`madvise` | present, demand-paged. |

## The gate that shapes everything: W^X

`vma_mmap_locked` (kernel/mm/vma.c) **refuses any mapping that is both
writable and executable**, and `mprotect` refuses the same transition.
This is deliberate and recorded in `docs/stdlib.md`.

The consequence for Flutter is not small and should be decided up front:

- **Dart JIT is impossible** as things stand. The JIT writes code into a
  page and then executes it. On NeoOS that is `-EINVAL`, twice over.
- **Dart AOT is fine**, and is what Flutter release builds use anyway
  (`--release` produces `libapp.so`, precompiled machine code, no JIT).

So the target is **Flutter AOT/release from day one**. Debug-mode
Flutter with hot reload is out of reach until either NeoOS gains a
dual-mapping escape hatch (map the same frames W at one address and X at
another — the standard W^X-preserving JIT trick, which NeoOS's
`vma_map_phys` is already most of the way to supporting) or the W^X rule
is relaxed for an opted-in process. **That is a design decision for the
user, not something to quietly work around.**

## Milestones

### M0 — Software GL to the framebuffer ✅ DONE

`neoos-tinygl`. Proves the framebuffer path end to end and gives NeoOS
a real rasteriser. Also produced the reusable `/dev/fb0` backend.

### M1 — A framebuffer window/session owner

The blocker M0 surfaced: **the kernel console and a graphical app write
to the same framebuffer and overwrite each other.** During the gears
demo the boot self-tests repainted text over the scene continuously; the
only way to photograph it was to build the kernel `QUIET=1`.

Before any GUI, one thing has to own the screen. The smallest correct
version is not a compositor:

- a way for a process to CLAIM the framebuffer, after which the kernel
  console stops drawing to it (NeoOS already has the concept —
  `term.nex` claims the tty and framebuffer; generalise it),
- and release on exit or death, so a crashed app does not leave the
  machine with no console.

This is small, unblocks screenshots and demos, and is a prerequisite for
everything below.

### M2 — Input plumbing

Flutter needs pointer and keyboard events
(`FlutterEngineSendPointerEvent`, `FlutterEngineSendKeyEvent`). NeoOS
has evdev keyboard. **A mouse driver is the missing piece** — PS/2
mouse is the least work under QEMU (`-device usb-mouse` or the i8042
aux port) and is a self-contained kernel task with its own selftest.

### M3 — Build the Flutter Engine for NeoOS

The large one. The engine is Skia + the Dart VM + the embedder, built
with `gn`/`ninja` out of `flutter/engine`.

Known work, in rough order of risk:
1. **A custom target in the engine's GN config.** The engine knows
   `linux-x64`; NeoOS is a new triple. The nearest precedent is the
   engine's existing "custom embedder" builds.
2. **Toolchain.** The engine is built with Clang and assumes a fairly
   recent one. NeoOS's cross toolchain is **GCC 9.4.0**, which supports
   C++17 but is older than the engine expects. Either build a Clang
   targeting `x86_64-neoos-linux-musl`, or accept GCC and fix what
   breaks. **Establish this before anything else — it decides whether
   M3 is weeks or months.**
3. **Skia's software backend** (`SkSurface::MakeRaster`) — no GPU
   dependency, which is the whole point of the software renderer.
4. **Syscall gaps.** Expect to find them the way .NET's port did:
   run it, read `[shim] ENOSYS <n>`, implement, repeat. The lesson from
   the ASP.NET work is recorded in `docs/aspnet-missing-syscalls.md` —
   **rebuild `neoos-musl` before concluding a syscall is missing**, since
   a stale `upstream/` tree produced a phantom ENOSYS that cost a
   session.

### M4 — A Flutter app on the framebuffer

Wire the engine to `/dev/fb0` via `FlutterSoftwareRendererConfig` and
the M1 screen owner, feed it M2's input, and run
`flutter create`'s default counter app. That is the "it works" moment.

### M5 — Windower and compositor

Only now does the user's stated end goal make sense to design, and it
should get its own spec. The shape worth considering, given M1 already
exists: the compositor is a Flutter app that owns the screen, and other
apps render into shared-memory surfaces it composites — which needs a
surface-sharing primitive (NeoOS has `vma_map_phys` and MAP_SHARED
groundwork) and an IPC channel.

## Risks, honestly

- **M3 dominates the schedule.** Everything else here is days; the
  engine build is the project. It is worth timeboxing the toolchain
  question (M3.2) first and reporting back before committing.
- **GCC 9 vs the engine's Clang assumption** is the most likely hard
  blocker.
- **No GPU means software Skia at 1280x800.** The gears demo already
  shows the cost: a 1280x800 software frame under TCG is not fast. On
  KVM it will be far better, and Flutter's damage-region redraw helps,
  but a full-screen animated UI at 60fps is not the expectation to set.
- **W^X vs the Dart JIT** — decide AOT-only, or design the dual-mapping
  escape hatch. Do not discover this halfway through M3.

## A cheaper alternative worth pricing first

If the goal is "a Flutter-shaped UI on NeoOS" rather than "the Flutter
engine on NeoOS", **flutter-elinux** and the embedder-only path with a
prebuilt engine are much smaller. So is writing the desktop against
Skia (or TinyGL) directly and using Flutter only for application
windows. The user should be asked which of these the goal actually is
before M3 is started, because M3 is the expensive one and the answer
changes whether it is needed at all.
