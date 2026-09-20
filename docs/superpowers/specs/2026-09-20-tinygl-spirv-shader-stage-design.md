# TinyGL SPIR-V shader stage (vertex + fragment)

## Context

NeoOS's desktop plan is Apple-style "Liquid Glass" throughout the UI:
translucent panels that lens (refract) and blur whatever is behind
them, with adaptive tint and specular highlights. Real-time lensing is
a per-pixel warp of the background image -- a genuine shader
operation, not something a fixed-function 2D blit can approximate.

`neoos-tinygl` (software OpenGL 1.x, ported from TinyGL) has no
concept of a shader. `ztriangle.c`/`zraster.c` rasterize with a fixed
per-pixel path: interpolate color/depth/texcoord, sample a texture,
write. There is no hook a caller can use to run its own per-pixel or
per-vertex logic.

This is the first of four dependent sub-projects toward a Liquid-Glass
desktop:

1. **This spec** -- a SPIR-V shader stage in TinyGL (vertex +
   fragment).
2. Compositor-level lensing in `neoos-wm` -- glass panels sampling
   *other* windows'/desktop content behind them, built on (1).
3. `neoos-uikit` -- a Liquid Glass themed layer wrapping LVGL widgets
   and panels, invoking the shader stage from (1)/(2).
4. A terminal emulator app, in pure C, as the first real application
   built on (3).

Only (1) is in scope here. (2)-(4) get their own specs once this
lands.

There is no real GPU driver in this plan. x86 GPU register-level specs
are not something a hobby OS can target in reasonable time, and the
only virtualization-only alternative (virtio-gpu + virglrenderer)
would mean porting a meaningful slice of Mesa's guest stack for a
QEMU-only capability -- a different, much larger undertaking than
anything else NeoOS has done, disproportionate to what a glass effect
needs. Lensing does not require rendering *speed* from real silicon,
it requires a per-pixel warp function -- which a CPU-side interpreter
provides directly.

## Goals

- A `.spv` (SPIR-V binary) shader module can be loaded into TinyGL and
  bound as the active vertex and/or fragment stage for subsequent draw
  calls.
- The fragment stage can sample a bound 2D texture -- this is how
  "sample what's behind the glass" is expressed: whatever populates
  that texture (a captured framebuffer region, in later sub-projects)
  is just a normal texture sample from the shader's point of view.
- Shaders are authored in GLSL and compiled to SPIR-V **on the build
  host** with `glslang`, the same way musl and GCC are host-cross-compiled
  today -- NeoOS ships an interpreter, never a compiler.
- Anything outside the supported SPIR-V subset fails to load, loudly,
  naming the exact unsupported opcode. Never silently degrades or
  partially emulates.
- Existing fixed-function TinyGL callers are unaffected when no
  program is bound.

## Non-goals

- No real GPU driver, no virtio-gpu/virglrenderer/Mesa port.
- No compute shaders, no geometry/tessellation stages.
- No dynamic (unbounded) loops -- every loop must have a static
  iteration bound, checked at load time.
- No general pointer/struct support beyond what vertex attributes,
  uniforms, and a bound 2D texture require.
- Compositor changes, the uikit glass layer, and the terminal app are
  out of scope -- separate specs.

## Supported SPIR-V subset

Full SPIR-V has hundreds of opcodes and arbitrary structured control
flow. This milestone supports only what a vertex-transform plus a
lensing/blur/tint fragment shader needs. Anything else is a load-time
rejection.

**Types:** `float`, `vec2`, `vec3`, `vec4`, `mat4`, fixed-size arrays
of those. No structs-of-structs, no pointers-to-pointers.

**Arithmetic / vector ops:** `OpFAdd`, `OpFSub`, `OpFMul`, `OpFDiv`,
`OpFNegate`, `OpDot`, `OpVectorTimesMatrix`, `OpVectorShuffle`,
`OpCompositeConstruct`, `OpCompositeExtract`.

**Control flow:** `OpSelectionMerge` + `OpBranchConditional` (if/else).
`OpLoopMerge` is accepted only when the loop's iteration count is
statically determinable from its bound-check operands at load time
(constant trip count); anything else is rejected as an unbounded loop.
No bare metal preemption exists inside a shader invocation, so an
unbounded loop is a full hang, not a slow frame -- this must be caught
before it runs, not survived after.

**Sampling:** `OpImageSampleImplicitLod` against exactly one bound 2D
texture per fragment invocation.

**Entry points:** exactly one `Vertex` and one `Fragment` entry point
per module. A module supplying more, fewer, or any other execution
model is rejected.

## Architecture

```
Build host                          NeoOS target
-----------                          ------------
shader.vert.glsl  --\
                      glslang  -->  shader.spv  --> spv_module.c (parse + validate)
shader.frag.glsl  --/                                      |
                                                             v
                                                        spv_vm.c (interpreter)
                                                             |
                          ztriangle.c (vertex path)  <------+------>  zraster.c (fragment path)
                          per-vertex VM invocation                    per-fragment VM invocation
```

`.spv` binaries are build artifacts checked in like any other asset --
NeoOS never runs glslang itself. On target:

- `spv_module.c` parses a `.spv` byte buffer into an internal
  instruction list plus type/constant tables, and validates it against
  the supported subset above. Validation happens once, at bind time --
  never mid-frame.
- `spv_vm.c` is the interpreter: a small per-invocation register file
  (SSA id -> value) executing one instruction stream per vertex
  invocation or per fragment invocation.
- `ztriangle.c` gains a hook in its vertex path: when a vertex program
  is bound, the vertex VM runs per vertex, producing clip-space
  position plus varyings, in place of TinyGL's fixed-function
  transform. When none is bound, the existing fixed-function path is
  unchanged.
- `zraster.c` gains a hook in its span-fill inner loop: when a fragment
  program is bound, the fragment VM runs per pixel, given the
  rasterizer's already-interpolated varyings (TinyGL already
  interpolates color/texcoord per pixel; varyings are interpolated the
  same way) plus the currently bound texture, producing the output
  RGBA. When none is bound, the existing fixed-function path is
  unchanged.

## Public API

```c
typedef struct TGLspvModule TGLspvModule;

/* Parses and validates a .spv buffer. Returns NULL and logs the
 * specific rejection reason (unsupported opcode, unbounded loop,
 * wrong entry point count, ...) on failure. Never partially loads. */
TGLspvModule *tglLoadSpvModule(const void *spv_bytes, size_t len);

void tglFreeSpvModule(TGLspvModule *mod);

/* Binds a vertex and/or fragment program for subsequent draw calls.
 * Either argument may be NULL to fall back to TinyGL's fixed-function
 * path for that stage. */
void tglUseSpvProgram(TGLspvModule *vs, TGLspvModule *fs);
```

This is the only new public surface. Existing TinyGL calls
(`tglBegin`/`tglVertex3f`/... and fixed-function texturing) are
unchanged and behave exactly as before when no program is bound.

## Data flow (runtime)

1. App loads `.spv` bytes (embedded at build time) and calls
   `tglLoadSpvModule` once per shader; `spv_module.c` parses and
   validates, rejecting immediately on any unsupported construct.
2. App calls `tglUseSpvProgram(vs, fs)` before issuing draw calls.
3. Per draw call, for each vertex: `ztriangle.c` invokes the vertex VM,
   producing clip-space position and varying outputs.
4. TinyGL's existing rasterizer interpolates those varyings per pixel,
   exactly as it interpolates color/texcoord today.
5. Per pixel, if a fragment program is bound: `zraster.c` invokes the
   fragment VM with the interpolated varyings and the currently bound
   texture, producing the output RGBA that gets written to the
   destination buffer.

## Error handling

- Unsupported opcode, an unbounded loop, or a module with the wrong
  entry-point count: `tglLoadSpvModule` returns `NULL` and logs the
  exact opcode/reason. This is a load-time failure, never a runtime
  fallback and never a silent partial emulation -- consistent with
  NeoOS's "translation not emulation" rule for the musl adaptor
  (CLAUDE.md).
- Out-of-bounds register or texture access inside a validated,
  running shader: the VM clamps/aborts that single invocation's output
  (matches typical GPU undefined-but-safe behavior for that pixel)
  rather than corrupting adjacent framebuffer or heap memory. This
  should not be reachable for a module that passed load-time
  validation, but the VM must not trust its own validator for memory
  safety.

## Testing

All verified via headless QEMU + serial log capture, matching every
other NeoOS milestone (no host-runnable unit tests -- this is
bare-metal code with no host runtime).

- **Golden-output test:** a trivial fragment shader
  (`gl_FragColor = texture(tex, uv) * tint`) rendered against a known
  input texture and tint uniform; the test computes a checksum of the
  output buffer over serial and compares it against an expected
  value.
- **Load-time rejection test:** a `.spv` module using an unsupported
  opcode (e.g. `OpImageSampleExplicitLod`) must be rejected by
  `tglLoadSpvModule`, with that opcode's name present in the serial
  log -- not silently accepted.
- **Unbounded-loop rejection test:** a `.spv` module containing a loop
  without a statically determinable trip count must be rejected at
  load time, not hung at runtime.
- **Fixed-function regression test:** existing TinyGL draw calls with
  no program bound must produce identical output to before this
  change (run the existing `glgears` smoke path and compare).
