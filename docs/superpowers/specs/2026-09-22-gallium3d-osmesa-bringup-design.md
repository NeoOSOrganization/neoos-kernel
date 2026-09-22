# Gallium3D bring-up: Mesa's OSMesa target on NeoOS

## Context

`docs/project-goal.md` names "getting Gallium3D working on NeoOS" as
the project's current goal, replacing the abandoned TinyGL-everywhere
compositor plumbing. That document records the research that ruled out
one shape of the goal (a virtio-gpu/virglrenderer guest driver
treating a host's Gallium3D as a remote service) as not a scoped,
near-term project: `VIRTIO_GPU_CMD_SUBMIT_3D`'s wire protocol has no
independent specification beyond Mesa's own C source, and NeoOS's
SPIR-V-subset VM has no TGSI representation to bridge with.

This spec picks up the other shape: a **native** port of Mesa's
Gallium3D to NeoOS, targeting a software pipe driver instead of real
GPU hardware. This is architecturally the right fit for the stated
requirement of something that works under QEMU today and can later
target real hardware without a rewrite: Gallium3D's own design already
separates a state tracker (the GL API surface) from a swappable pipe
driver underneath it, and a software pipe driver (`softpipe`, later
`llvmpipe`) never touches a GPU either way, so "works in QEMU" and
"works on real hardware" are the same code path.

**This is the first of several sub-projects**, not the whole goal.
The full decomposition, agreed with the user before this spec was
written:

1. **This spec: build infrastructure and a minimal proof of life** —
   cross-compile Mesa with a software pipe driver and render one real
   triangle through the actual OpenGL API, off-screen.
2. A NeoOS-specific EGL/windowing integration, so an application using
   standard `libGL.so`/EGL context creation (not the OSMesa-specific
   API this spec uses) can run.
3. Adoption: swap `neoos-wm`'s TinyGL usage, and/or port a real GL
   application, onto the new stack.

Only sub-project 1 is brainstormed to a spec here. Later sub-projects
get their own brainstorming pass once this one is implemented and
verified.

## Goals

- A new repo, `neoos-mesa`, cross-compiles Mesa (a version pinned
  during implementation for compatibility with the existing hosted
  toolchain's GCC 9.4.0/C++17 ceiling) for NeoOS, producing a static
  `libOSMesa` built with `-Dosmesa=true -Dgallium-drivers=softpipe`
  and no window-system integration (`-Dplatforms=`, `-Dglx=disabled`,
  `-Degl=disabled`, `-Dgbm=disabled`, `-Dvulkan-drivers=`,
  `-Dllvm=disabled`, `-Dshader-cache=disabled`).
- A NeoOS test program links against `libOSMesa`, creates an
  off-screen context (`OSMesaCreateContext`/`OSMesaMakeCurrent`),
  issues real OpenGL draw calls to render one triangle, and reads back
  the pixel buffer.
- The test boots under headless QEMU with serial log capture (this
  project's standard verification method) and asserts specific
  pixels match the expected triangle, not merely "didn't crash" —
  matching the existing pixel-sampling-assertion standard set by
  `wm-glass`'s screenshot check.
- If Mesa's minimal build genuinely requires zlib and/or libexpat as
  hard dependencies (not merely optional), this sub-project ports
  them first, following the existing single-C-library port pattern
  (`neoos-openssl`: vendored upstream submodule, `build.sh`,
  `build-output/`) — this is in scope, not a blocker that reduces
  scope.
- The full 15/15-zero-retry gauntlet regression stays green.

## Non-goals

- Any windowing-system integration (EGL, GLX, a NeoOS-specific winsys
  surface). This sub-project's test renders into a plain heap buffer
  via OSMesa's own API, which is not how a real application creates a
  GL context. That is sub-project 2.
- `llvmpipe`, or any pipe driver other than `softpipe`. LLVM is a much
  larger dependency this sub-project does not need to take on to prove
  the pipeline works.
- Any change to `neoos-wm`, TinyGL, or any existing graphics-stack
  code. Nothing here is wired into the compositor yet — that is
  sub-project 3.
- A NeoOS-specific `os_*` porting layer, unless the build actually
  proves one is needed. The target triple's OS component already
  reports `linux` (confirmed: `x86_64-neoos-linux-musl-gcc` predefines
  `__linux__`), so Mesa's existing Linux/POSIX `os_time.c`/`os_thread.c`/
  `os_mman.c` paths are expected to compile against NeoOS's musl
  unmodified. This is a design expectation to verify empirically
  during implementation, not a foregone conclusion.
- Vulkan, any GPU hardware driver, and any pipe driver beyond
  `softpipe`. Out of scope for the whole project's current phase, per
  `docs/project-goal.md`, let alone this first sub-project.

## Design

### 1. Repo and toolchain

`neoos-mesa` (new repo, matching the existing `neoos-<port>` sibling
layout: `Makefile`, `build.sh`, `build-output/`, `upstream/` as a
pinned git submodule).

Unlike every prior C/C++ port (openssl, curl, libssh2), this one uses
the **hosted** `x86_64-neoos-linux-musl` toolchain
(`~/opt/cross-x86_64-neoos`, from
`docs/superpowers/specs/2026-09-07-hosted-neoos-gcc-design.md`) rather
than the freestanding `x86_64-elf-*` compiler. Reasons: Mesa's C++
code (parts of its util/glsl/NIR layers) needs real exception handling
and RTTI, which only the hosted toolchain's `libgcc_eh.a`/
`crtbeginT.o` provide; and the hosted toolchain links normally
(`x86_64-neoos-linux-musl-gcc -static foo.c -o foo`, no hand-rolled
`user.ld`/manual `crt1.o`), which this port relies on instead of
reproducing every other port's freestanding linker-script convention.

### 2. Build system: a Meson cross-file

Mesa is Meson-only (autotools was dropped years ago upstream). Both
`meson` (1.10.1) and `ninja` (1.13.2) are already present on this
host. `neoos-mesa/cross-file.txt` names the hosted toolchain's
`gcc`/`g++`/`ar`/`strip` and declares `host_machine.system() =
'linux'` — the Meson equivalent of the `toolchain.cmake` every
CMake-based port already carries.

A host-side build prerequisite this port adds: Mako (`pip install
mako`), Mesa's code-generation templating engine, used by the build
itself (host tool, not cross-compiled) to generate shader-related
source. `python3`, `bison`, and `flex` — also host-side build
requirements — are already present.

### 3. Mesa version and configuration

The exact Mesa release is pinned as the first implementation task: it
must build cleanly against GCC 9.4.0 in C++17 mode (recent Mesa
releases have raised their minimum compiler baseline for driver code
this port does not use — verify the pinned tag configures and builds
before committing to it, rather than assuming a version number here).

Meson options, all justified by Goals/Non-goals above:

```
-Dosmesa=true
-Dgallium-drivers=softpipe
-Dplatforms=
-Dglx=disabled
-Degl=disabled
-Dgbm=disabled
-Dvulkan-drivers=
-Dllvm=disabled
-Dshader-cache=disabled
```

If configuring with these options fails because Mesa's build treats
zlib and/or libexpat as hard requirements (not optional), this
sub-project's implementation plan gains a prerequisite task: port
whichever is missing as its own `neoos-<lib>` repo, using the same
pattern `neoos-openssl` already established, before returning to
`neoos-mesa`.

### 4. Proof-of-life test program

A small NeoOS-side C program (`neoos-mesa/test/`, linked with the
hosted toolchain against the freshly built `libOSMesa.a` and NeoOS's
musl/CRT) that:

1. `OSMesaCreateContext` for an RGBA off-screen buffer.
2. `OSMesaMakeCurrent` against a heap-allocated pixel buffer of a
   fixed, known size.
3. Issues real OpenGL calls to draw one triangle. The implementation
   plan decides between fixed-function (`glBegin`/`glVertex3f`) and a
   minimal GLSL shader (`glCreateShader`/`glCompileShader`) — GLSL
   exercises more of the real pipeline (Mesa's shader compiler down to
   TGSI, not just the rasterizer) and is the better long-term proof if
   it works without extra complication; fixed-function is the fallback
   if GLSL compilation surfaces problems unrelated to this sub-project's
   actual goal.
4. Reads back the pixel buffer and asserts specific pixels match the
   expected triangle's color at expected coordinates. A go/no-go
   printed to the serial console, matching this project's existing
   headless-QEMU-plus-serial-log verification convention.

### 5. Error handling

This is build infrastructure plus an offline test, not a runtime
service — there is no user-facing error path to design. Build
failures are debugged through Meson's own configure/build logs
(standard practice, no NeoOS-specific tooling needed). The test
program's own failure mode is: print which assertion failed (context
creation, `MakeCurrent`, shader compile, or pixel mismatch with
expected-vs-actual values) to the serial console and exit non-zero,
so a failed headless-QEMU run is diagnosable from the captured log
alone, the same standard every other port's test already meets.

## Testing

- Mesa's own build completing (`ninja` exits 0) for the pinned
  version and option set above is the first checkpoint, verified
  before writing the NeoOS-side test program.
- The proof-of-life test program (section 4) boots under headless
  QEMU with serial log capture and passes its pixel-sampling
  assertion.
- Full `tools/gauntlet.sh 15 3`, zero retries. This sub-project adds a
  new host toolchain user and a new repo but touches no kernel code
  and no shared musl-shim code, so a regression here would be a real
  signal.

## Known limitation carried forward

This sub-project deliberately produces nothing an application can use
yet: OSMesa's context-creation API is Mesa-specific, not how a real
GL application (written against EGL or GLX) creates a context. That
gap is sub-project 2's job, to be brainstormed separately once this
one is implemented and verified.
