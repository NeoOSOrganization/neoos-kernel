# TinyGL SPIR-V Shader Stage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `neoos-tinygl` a vertex+fragment shader stage driven by a SPIR-V subset interpreter, so a fragment shader can sample a bound texture and produce lensing/blur/tint effects — the dependency every later Liquid-Glass sub-project (compositor lensing, `neoos-uikit`, the terminal app) needs.

**Architecture:** Shaders are authored in GLSL, compiled to `.spv` on the build host with `glslang` (never on-target). NeoOS ships only an interpreter: `spv_module.c` parses and validates a `.spv` binary against a fixed opcode subset at load time (hard rejection, never partial emulation); `spv_vm.c` executes it, once per vertex and once per fragment. Two minimal, clearly marked hooks are added to TinyGL's otherwise-unmodified vendored rasterizer (`ztriangle.c`'s vertex path, `zraster.c`'s span-fill loop) so a bound program runs in place of the fixed-function path.

**Tech Stack:** C (gnu99), `x86_64-elf-gcc` freestanding cross-compile (matches every other NeoOS userland/port build), `spirv-headers`/`spirv-as` for test fixtures, host `gcc` for the portable parser/VM's own test cycle.

**Spec:** `docs/superpowers/specs/2026-09-20-tinygl-spirv-shader-stage-design.md`

## Global Constraints

- Ship an interpreter only. NeoOS never runs `glslang` or any compiler on-target.
- Unsupported opcode / unbounded loop / wrong entry-point shape → `tglLoadSpvModule` returns `NULL` and logs the exact reason. Never a silent fallback, never partial emulation (CLAUDE.md's "translation not emulation" rule).
- Every loop (`OpLoopMerge`) must have a statically determinable trip count, capped at 4096 iterations, checked at load time — no unbounded loop may ever start running (no preemption exists inside a shader invocation on bare metal).
- Supported types: `float`, `vec2`, `vec3`, `vec4`, `mat4`, fixed-size arrays of those. No structs-of-structs, no pointer-to-pointer.
- Exactly one entry point per module, and its execution model (`Vertex` or `Fragment`) must match the stage the caller loads it for.
- `upstream/` stays vendored-unmodified everywhere except the two hook insertions called out in Task 8 — each is a 1-4 line, clearly commented (`/* NEOOS-SPV-HOOK */`) addition, not a rewrite.
- Verification for the parser/VM (Tasks 1-7) runs as host-native C via `gcc` — this code has no OS dependency, so there is no reason to pay for a QEMU boot per test. Verification for the TinyGL integration and end-to-end shader behavior (Tasks 8-11) runs via headless QEMU + serial log capture, per every other NeoOS milestone.

---

## File Structure

```
neoos-tinygl/
  spv/
    spv_defs.h        # opcode enum subset (exact values from the SPIR-V spec), module header layout
    spv_module.h       # TGLspvModule type, tglLoadSpvModule/tglFreeSpvModule declarations
    spv_module.c        # binary parse: header, type/constant table, globals, function decode, subset + loop-bound validation
    spv_vm.h           # spv_vm_run_vertex / spv_vm_run_fragment declarations, spv_value_t
    spv_vm.c            # register file, arithmetic/control-flow/texture-sample execution
    spv_gl.h           # tglUseSpvProgram, the two hook-callable functions TinyGL's rasterizer calls
    spv_gl.c
  test/
    host/
      Makefile          # host-native build of spv_module.c + spv_vm.c + spv_gl-free test harness
      test_module.c      # Task 1-5 tests
      test_vm.c           # Task 6-7 tests
      test_sampling.c     # Task 8 test
      fixtures/*.spvasm  # SPIR-V assembly text, turned into .spv by spirv-as at test time
    target/
      spvtest.c          # Task 9-11: glgears-style self-checking .nex test program
      spvtest.test.json
  upstream/src/ztriangle.c   # +hook (Task 9)
  upstream/src/zraster.c     # +hook (Task 9)

neoos-kernel/ (this repo, NeoOS/)
  Makefile             # +spvtest target (Task 10), modeled on the existing `uxtest` target
```

`spv/` is new, wholly ours, and never touches `upstream/`. The only vendored-file edits are the two hooks in Task 8/9, each isolated behind a single `if (bound_program)` check.

---

## Task 1: SPIR-V module header parsing

**Files:**
- Create: `neoos-tinygl/spv/spv_defs.h`
- Create: `neoos-tinygl/spv/spv_module.h`
- Create: `neoos-tinygl/spv/spv_module.c`
- Create: `neoos-tinygl/test/host/Makefile`
- Test: `neoos-tinygl/test/host/test_module.c`

**Interfaces:**
- Produces: `TGLspvModule *tglLoadSpvModule(const void *bytes, size_t len)` (returns `NULL` on any rejection, logging via `fprintf(stderr, ...)` on host / the existing NeoOS `printf`-to-serial convention on target — the same source file builds both ways since it has no OS dependency), `void tglFreeSpvModule(TGLspvModule *mod)`.

- [ ] **Step 1: Write `spv_defs.h` with the opcode subset and header layout**

```c
/* spv/spv_defs.h -- the supported SPIR-V opcode subset and binary
 * module header layout. Values are exactly the SPIR-V spec's, taken
 * from /usr/include/spirv/unified1/spirv.h -- never renumbered. */
#ifndef NEOOS_SPV_DEFS_H
#define NEOOS_SPV_DEFS_H

#include <stdint.h>

#define SPV_MAGIC 0x07230203u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t generator;
    uint32_t bound;      /* one past the highest <id> used in the module */
    uint32_t schema;      /* must be 0 */
} spv_header_t;

typedef enum {
    SPV_OP_NOP                    = 0,
    SPV_OP_NAME                   = 5,
    SPV_OP_MEMBER_NAME            = 6,
    SPV_OP_EXTINSTIMPORT          = 11,
    SPV_OP_MEMORY_MODEL           = 14,
    SPV_OP_ENTRY_POINT            = 15,
    SPV_OP_EXECUTION_MODE         = 16,
    SPV_OP_CAPABILITY             = 17,
    SPV_OP_TYPE_VOID              = 19,
    SPV_OP_TYPE_BOOL              = 20,
    SPV_OP_TYPE_INT               = 21,
    SPV_OP_TYPE_FLOAT             = 22,
    SPV_OP_TYPE_VECTOR            = 23,
    SPV_OP_TYPE_MATRIX            = 24,
    SPV_OP_TYPE_IMAGE             = 25,
    SPV_OP_TYPE_SAMPLED_IMAGE     = 27,
    SPV_OP_TYPE_ARRAY             = 28,
    SPV_OP_TYPE_POINTER           = 32,
    SPV_OP_TYPE_FUNCTION          = 33,
    SPV_OP_CONSTANT_TRUE          = 41,
    SPV_OP_CONSTANT_FALSE         = 42,
    SPV_OP_CONSTANT               = 43,
    SPV_OP_CONSTANT_COMPOSITE     = 44,
    SPV_OP_FUNCTION               = 54,
    SPV_OP_FUNCTION_PARAMETER     = 55,
    SPV_OP_FUNCTION_END           = 56,
    SPV_OP_VARIABLE               = 59,
    SPV_OP_LOAD                   = 61,
    SPV_OP_STORE                  = 62,
    SPV_OP_ACCESS_CHAIN           = 65,
    SPV_OP_DECORATE               = 71,
    SPV_OP_MEMBER_DECORATE        = 72,
    SPV_OP_VECTOR_SHUFFLE         = 79,
    SPV_OP_COMPOSITE_CONSTRUCT    = 80,
    SPV_OP_COMPOSITE_EXTRACT      = 81,
    SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    SPV_OP_FNEGATE                = 127,
    SPV_OP_FADD                   = 129,
    SPV_OP_FSUB                   = 131,
    SPV_OP_FMUL                   = 133,
    SPV_OP_FDIV                   = 136,
    SPV_OP_VECTOR_TIMES_SCALAR    = 142,
    SPV_OP_VECTOR_TIMES_MATRIX    = 144,
    SPV_OP_DOT                    = 148,
    SPV_OP_IEQUAL                 = 170,
    SPV_OP_SLESSTHAN              = 177,
    SPV_OP_IADD                   = 128,
    SPV_OP_PHI                    = 245,
    SPV_OP_LOOP_MERGE             = 246,
    SPV_OP_SELECTION_MERGE        = 247,
    SPV_OP_LABEL                  = 248,
    SPV_OP_BRANCH                 = 249,
    SPV_OP_BRANCH_CONDITIONAL     = 250,
    SPV_OP_RETURN                 = 253,
} spv_opcode_t;

/* Execution models we accept -- exactly two, matching SpvExecutionModel. */
#define SPV_EXEC_MODEL_VERTEX   0u
#define SPV_EXEC_MODEL_FRAGMENT 4u

typedef struct TGLspvModule TGLspvModule;

#endif
```

- [ ] **Step 2: Write the failing header-parse test**

```c
/* neoos-tinygl/test/host/test_module.c */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../spv/spv_module.h"

static const uint32_t VALID_HEADER[5] = { 0x07230203u, 0x00010000u, 0, 10, 0 };
static const uint32_t BAD_MAGIC[5]    = { 0xDEADBEEFu, 0x00010000u, 0, 10, 0 };

int main(void) {
    /* A 5-word buffer with nothing past the header is too short to be
     * a real module, but the header itself must parse and then the
     * loader must reject it for having no OpEntryPoint -- not crash. */
    TGLspvModule *m = tglLoadSpvModule(VALID_HEADER, sizeof(VALID_HEADER));
    assert(m == NULL); /* no entry point yet -- Task 3 makes this pass with a real module */

    m = tglLoadSpvModule(BAD_MAGIC, sizeof(BAD_MAGIC));
    assert(m == NULL);

    printf("test_module: header parsing OK\n");
    return 0;
}
```

- [ ] **Step 3: Write the host test Makefile and confirm the test fails to build (no `spv_module.c` logic yet)**

```makefile
# neoos-tinygl/test/host/Makefile -- host-native, NOT the cross-compiled
# target build. spv_module.c/spv_vm.c have no OS dependency, so their
# own correctness is verified here, fast, without a QEMU boot. Only
# the TinyGL integration (Task 9+) needs the real target boot.
CC := gcc
CFLAGS := -std=gnu99 -Wall -Wextra -g -I../../spv

SPV_SRCS := ../../spv/spv_module.c ../../spv/spv_vm.c

.PHONY: test clean
test: test_module test_vm test_sampling
	./test_module && ./test_vm && ./test_sampling

test_module: test_module.c $(SPV_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_vm: test_vm.c $(SPV_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_sampling: test_sampling.c $(SPV_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_module test_vm test_sampling
```

Run: `cd neoos-tinygl/test/host && make test_module`
Expected: FAIL -- `spv_module.c`/`spv_module.h` don't exist yet.

- [ ] **Step 4: Write `spv_module.h` and the minimal `spv_module.c` that parses just the header**

```c
/* spv/spv_module.h */
#ifndef NEOOS_SPV_MODULE_H
#define NEOOS_SPV_MODULE_H
#include <stddef.h>
#include "spv_defs.h"

TGLspvModule *tglLoadSpvModule(const void *bytes, size_t len);
void tglFreeSpvModule(TGLspvModule *mod);

#endif
```

```c
/* spv/spv_module.c (Task 1 slice -- header only; Tasks 2-5 fill in the
 * rest of this same file's parsing pipeline) */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "spv_module.h"

struct TGLspvModule {
    spv_header_t header;
    /* Tasks 2-5 add: type/constant tables, globals, function body,
     * entry-point execution model, validated loop trip counts. */
};

static int parse_header(const uint32_t *words, size_t nwords, spv_header_t *out) {
    if (nwords < 5) return 0;
    if (words[0] != SPV_MAGIC) return 0;
    out->magic = words[0];
    out->version = words[1];
    out->generator = words[2];
    out->bound = words[3];
    out->schema = words[4];
    return out->schema == 0;
}

TGLspvModule *tglLoadSpvModule(const void *bytes, size_t len) {
    if (len % 4 != 0) return NULL;
    const uint32_t *words = (const uint32_t *)bytes;
    size_t nwords = len / 4;

    spv_header_t hdr;
    if (!parse_header(words, nwords, &hdr)) return NULL;

    /* Task 3 adds real entry-point discovery; until then, any module
     * (even a structurally valid one) has no entry point and must be
     * rejected here, which is exactly what Step 2's test checks. */
    fprintf(stderr, "spv: rejected -- no entry point (module body not parsed yet)\n");
    return NULL;
}

void tglFreeSpvModule(TGLspvModule *mod) {
    free(mod);
}
```

- [ ] **Step 5: Run the test and confirm it passes**

Run: `cd neoos-tinygl/test/host && make test_module && ./test_module`
Expected: `test_module: header parsing OK`

- [ ] **Step 6: Commit**

```bash
git -C neoos-tinygl add spv/spv_defs.h spv/spv_module.h spv/spv_module.c test/host/Makefile test/host/test_module.c
git -C neoos-tinygl commit -m "spv: parse and validate the SPIR-V module header"
```

---

## Task 2: Type and constant table

**Files:**
- Modify: `neoos-tinygl/spv/spv_module.c`
- Modify: `neoos-tinygl/spv/spv_module.h` (expose the type-kind enum for Task 6's VM to consume)
- Test: `neoos-tinygl/test/host/test_module.c` (add cases)

**Interfaces:**
- Consumes: `spv_opcode_t` from Task 1's `spv_defs.h`.
- Produces: an internal `spv_type_t`/`spv_const_t` table indexed by SPIR-V `<id>`, plus `spv_type_kind_t` (`SPV_TYPE_FLOAT`, `SPV_TYPE_VEC2/3/4`, `SPV_TYPE_MAT4`, `SPV_TYPE_ARRAY`, `SPV_TYPE_POINTER`) that Task 6's VM will read via `spv_module_type_of(mod, id)`.

- [ ] **Step 1: Add a failing test for a module with a real type + constant table**

```c
/* Append to test_module.c's main(), before the final return: */
{
    /* Hand-built minimal module: header + OpTypeFloat + OpTypeVector(vec4)
     * + OpConstant(float 1.0) -- still no entry point, so load must
     * still fail, but it must fail for the RIGHT reason (checked via
     * stderr in a real test harness would be brittle; instead assert
     * that a module with a type table + entry point, built in Step 2's
     * fixture below, DOES parse that far -- covered in Task 3). */
}
printf("test_module: type/constant table OK (validated fully in Task 3)\n");
```

Task 2 alone cannot observably differ from Task 1's behavior (no entry point = no accepted module either way), so its real test lands in Task 3 once a module can be fully accepted. This step just adds the parsing code with unit-level assertions inside `spv_module.c` via a `#ifdef SPV_SELFCHECK` block is overkill — skip a standalone Task 2 test file and fold verification into Task 3's end-to-end fixture, which is the honest place a type table's correctness becomes observable.

- [ ] **Step 2: Implement type/constant table parsing in `spv_module.c`**

```c
/* Added to struct TGLspvModule: */
typedef enum {
    SPV_TYPE_VOID, SPV_TYPE_FLOAT, SPV_TYPE_VEC2, SPV_TYPE_VEC3,
    SPV_TYPE_VEC4, SPV_TYPE_MAT4, SPV_TYPE_ARRAY, SPV_TYPE_POINTER,
    SPV_TYPE_IMAGE, SPV_TYPE_SAMPLED_IMAGE, SPV_TYPE_FUNCTION
} spv_type_kind_t;

typedef struct {
    spv_type_kind_t kind;
    uint32_t elem_type_id;   /* SPV_TYPE_ARRAY/POINTER: element type */
    uint32_t array_len;      /* SPV_TYPE_ARRAY only */
} spv_type_t;

typedef struct {
    uint32_t type_id;
    float f32[16];           /* up to a mat4's 16 floats; scalars use [0] */
    int is_composite;
} spv_const_t;

#define SPV_MAX_IDS 4096

struct TGLspvModule {
    spv_header_t header;
    spv_type_t types[SPV_MAX_IDS];
    int has_type[SPV_MAX_IDS];
    spv_const_t consts[SPV_MAX_IDS];
    int has_const[SPV_MAX_IDS];
};

/* Walks the instruction stream starting right after the header,
 * recording every OpType*/OpConstant* by result <id>. Returns 0 and
 * logs the offending opcode on anything outside the supported set
 * that isn't yet handled by a later task (Tasks 3-5 extend this same
 * switch) -- for Task 2, only type/constant opcodes are recognized;
 * everything else is skipped over by word-count, not rejected, since
 * rejection of the FULL unsupported set only becomes meaningful once
 * Task 4 walks function bodies too. */
static int parse_globals(TGLspvModule *mod, const uint32_t *words,
                          size_t nwords, size_t *cursor) {
    size_t i = *cursor;
    while (i < nwords) {
        uint32_t instr = words[i];
        uint16_t wc = (uint16_t)(instr >> 16);
        uint16_t op = (uint16_t)(instr & 0xFFFF);
        if (wc == 0) return 0; /* malformed: zero-length instruction */

        switch (op) {
        case SPV_OP_TYPE_VOID: {
            uint32_t id = words[i + 1];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = SPV_TYPE_VOID;
            mod->has_type[id] = 1;
            break;
        }
        case SPV_OP_TYPE_FLOAT: {
            uint32_t id = words[i + 1];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = SPV_TYPE_FLOAT;
            mod->has_type[id] = 1;
            break;
        }
        case SPV_OP_TYPE_VECTOR: {
            uint32_t id = words[i + 1];
            uint32_t comp_count = words[i + 3];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = comp_count == 2 ? SPV_TYPE_VEC2
                                 : comp_count == 3 ? SPV_TYPE_VEC3
                                 : comp_count == 4 ? SPV_TYPE_VEC4
                                 : (spv_type_kind_t)-1;
            if ((int)mod->types[id].kind == -1) {
                fprintf(stderr, "spv: rejected -- OpTypeVector with %u components\n", comp_count);
                return 0;
            }
            mod->has_type[id] = 1;
            break;
        }
        case SPV_OP_TYPE_MATRIX: {
            uint32_t id = words[i + 1];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = SPV_TYPE_MAT4; /* only mat4 supported */
            mod->has_type[id] = 1;
            break;
        }
        case SPV_OP_TYPE_ARRAY: {
            uint32_t id = words[i + 1];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = SPV_TYPE_ARRAY;
            mod->types[id].elem_type_id = words[i + 2];
            /* length is itself a constant <id>; Task 2 resolves it
             * once const parsing below has run for earlier ids. Real
             * SPIR-V always emits the length constant before the
             * array type that uses it, so a single forward pass is
             * sufficient. */
            {
                uint32_t len_id = words[i + 3];
                mod->types[id].array_len = mod->has_const[len_id]
                    ? (uint32_t)mod->consts[len_id].f32[0] : 0;
            }
            mod->has_type[id] = 1;
            break;
        }
        case SPV_OP_TYPE_POINTER: {
            uint32_t id = words[i + 1];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = SPV_TYPE_POINTER;
            mod->types[id].elem_type_id = words[i + 3];
            mod->has_type[id] = 1;
            break;
        }
        case SPV_OP_CONSTANT: {
            uint32_t type_id = words[i + 1];
            uint32_t id = words[i + 2];
            if (id >= SPV_MAX_IDS) return 0;
            mod->consts[id].type_id = type_id;
            memcpy(&mod->consts[id].f32[0], &words[i + 3], sizeof(uint32_t));
            mod->consts[id].is_composite = 0;
            mod->has_const[id] = 1;
            break;
        }
        case SPV_OP_CONSTANT_COMPOSITE: {
            uint32_t type_id = words[i + 1];
            uint32_t id = words[i + 2];
            if (id >= SPV_MAX_IDS) return 0;
            mod->consts[id].type_id = type_id;
            mod->consts[id].is_composite = 1;
            for (int k = 0; k < wc - 3 && k < 16; k++) {
                uint32_t comp_id = words[i + 3 + k];
                mod->consts[id].f32[k] = mod->has_const[comp_id]
                    ? mod->consts[comp_id].f32[0] : 0.0f;
            }
            mod->has_const[id] = 1;
            break;
        }
        case SPV_OP_FUNCTION:
            /* Function bodies start here -- Task 4 takes over from
             * this cursor position. */
            *cursor = i;
            return 1;
        default:
            /* Not yet handled -- Tasks 3-4 extend this switch. Skip
             * by word count rather than reject, since global-section
             * opcodes outside the type/const set (OpName, OpDecorate,
             * OpCapability, ...) are legal and simply uninteresting to
             * this table. */
            break;
        }
        i += wc;
    }
    *cursor = i;
    return 1;
}
```

- [ ] **Step 3: Build and confirm no regression**

Run: `cd neoos-tinygl/test/host && make test_module && ./test_module`
Expected: `test_module: header parsing OK` then `test_module: type/constant table OK (validated fully in Task 3)`

- [ ] **Step 4: Commit**

```bash
git -C neoos-tinygl add spv/spv_module.c test/host/test_module.c
git -C neoos-tinygl commit -m "spv: parse the type and constant tables"
```

---

## Task 3: Entry points and full-module acceptance

**Files:**
- Modify: `neoos-tinygl/spv/spv_module.c`, `spv_module.h`
- Test: `neoos-tinygl/test/host/test_module.c`, new fixture `neoos-tinygl/test/host/fixtures/minimal_vertex.spvasm`

**Interfaces:**
- Produces: `int tglSpvEntryModel(const TGLspvModule *mod)` returning `SPV_EXEC_MODEL_VERTEX`/`SPV_EXEC_MODEL_FRAGMENT`, used by Task 9's `tglUseSpvProgram` to check the right module was bound to the right stage.

- [ ] **Step 1: Write the minimal valid fixture in SPIR-V assembly**

```
; neoos-tinygl/test/host/fixtures/minimal_vertex.spvasm
; A vertex shader that outputs a constant clip-space position -- the
; smallest module with exactly one entry point, exercised end to end.
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %main "main" %gl_Position
               OpExecutionMode %main OriginUpperLeft
       %void = OpTypeVoid
       %fnty = OpTypeFunction %void
      %float = OpTypeFloat 32
       %vec4 = OpTypeVector %float 4
    %out_ptr = OpTypePointer Output %vec4
%gl_Position = OpVariable %out_ptr Output
       %one  = OpConstant %float 1.0
      %zero  = OpConstant %float 0.0
      %const4 = OpConstantComposite %vec4 %zero %zero %zero %one
       %main = OpFunction %void None %fnty
      %entry = OpLabel
               OpStore %gl_Position %const4
               OpReturn
               OpFunctionEnd
```

- [ ] **Step 2: Assemble it and add a failing test that expects acceptance**

```bash
spirv-as --target-env spv1.0 neoos-tinygl/test/host/fixtures/minimal_vertex.spvasm \
  -o neoos-tinygl/test/host/fixtures/minimal_vertex.spv
```

```c
/* Add to test_module.c */
#include <stdio.h>
static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(*len);
    assert(fread(buf, 1, *len, f) == *len);
    fclose(f);
    return buf;
}

/* In main(): */
{
    size_t len;
    void *bytes = read_file("fixtures/minimal_vertex.spv", &len);
    TGLspvModule *m = tglLoadSpvModule(bytes, len);
    assert(m != NULL);
    assert(tglSpvEntryModel(m) == SPV_EXEC_MODEL_VERTEX);
    tglFreeSpvModule(m);
    free(bytes);
    printf("test_module: minimal valid module accepted\n");
}
```

- [ ] **Step 3: Run and confirm it fails (no entry-point/function parsing yet)**

Run: `cd neoos-tinygl/test/host && ./test_module` (after `make`)
Expected: assertion failure at `assert(m != NULL)`.

- [ ] **Step 4: Implement `OpEntryPoint`/`OpFunction`...`OpFunctionEnd` parsing and acceptance**

```c
/* Added fields on TGLspvModule: */
    uint32_t entry_model;   /* SPV_EXEC_MODEL_* */
    int has_entry;
    uint32_t entry_func_id;

/* In the same forward pass as Task 2's parse_globals, add: */
        case SPV_OP_ENTRY_POINT: {
            if (mod->has_entry) {
                fprintf(stderr, "spv: rejected -- more than one entry point\n");
                return 0;
            }
            mod->entry_model = words[i + 1];
            mod->entry_func_id = words[i + 2];
            if (mod->entry_model != SPV_EXEC_MODEL_VERTEX &&
                mod->entry_model != SPV_EXEC_MODEL_FRAGMENT) {
                fprintf(stderr, "spv: rejected -- unsupported execution model %u\n", mod->entry_model);
                return 0;
            }
            mod->has_entry = 1;
            break;
        }

/* tglLoadSpvModule's tail, replacing Task 1's unconditional rejection: */
    size_t cursor = 5;
    TGLspvModule *mod = calloc(1, sizeof(TGLspvModule));
    mod->header = hdr;
    if (!parse_globals(mod, words, nwords, &cursor)) { free(mod); return NULL; }
    if (!mod->has_entry) {
        fprintf(stderr, "spv: rejected -- no OpEntryPoint\n");
        free(mod);
        return NULL;
    }
    /* Task 4 parses the function body starting at `cursor`; for this
     * task, a module with a valid header + exactly one recognized
     * entry point is accepted even before its body is decoded, since
     * Task 4 is what turns body-decode failures into rejections. */
    return mod;

int tglSpvEntryModel(const TGLspvModule *mod) { return (int)mod->entry_model; }
```

- [ ] **Step 5: Run and confirm the test passes**

Run: `cd neoos-tinygl/test/host && ./test_module`
Expected: all three lines print, exit 0.

- [ ] **Step 6: Commit**

```bash
git -C neoos-tinygl add spv/spv_module.c spv/spv_module.h test/host/test_module.c test/host/fixtures/minimal_vertex.spvasm test/host/fixtures/minimal_vertex.spv
git -C neoos-tinygl commit -m "spv: accept a module with exactly one valid entry point"
```

---

## Task 4: Function body decode into basic blocks, with opcode-subset rejection

**Files:**
- Modify: `neoos-tinygl/spv/spv_module.c`, `spv_module.h`
- Test: `neoos-tinygl/test/host/test_module.c`, new fixtures `fixtures/unsupported_opcode.spvasm`

**Interfaces:**
- Produces: `spv_block_t { uint32_t label_id; spv_instr_t *instrs; int ninstrs; uint32_t *succ; int nsucc; }` and `spv_func_t { spv_block_t *blocks; int nblocks; }`, stored on `TGLspvModule` and exposed via `const spv_func_t *spv_module_entry_func(const TGLspvModule *mod)` for Task 6's VM.

- [ ] **Step 1: Write the unsupported-opcode fixture and failing test**

```
; fixtures/unsupported_opcode.spvasm -- uses OpImageSampleExplicitLod,
; which is NOT in the supported subset (only ImplicitLod is).
               OpCapability Shader
               OpCapability ImageQuery
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main"
               OpExecutionMode %main OriginUpperLeft
       %void = OpTypeVoid
       %fnty = OpTypeFunction %void
      %float = OpTypeFloat 32
       %vec4 = OpTypeVector %float 4
      %img_t = OpTypeImage %float 2D 0 0 0 1 Unknown
     %simg_t = OpTypeSampledImage %img_t
   %ptr_simg = OpTypePointer UniformConstant %simg_t
        %tex = OpVariable %ptr_simg UniformConstant
       %vec2 = OpTypeVector %float 2
       %zero = OpConstant %float 0.0
     %coord  = OpConstantComposite %vec2 %zero %zero
       %main = OpFunction %void None %fnty
      %entry = OpLabel
        %s   = OpLoad %simg_t %tex
        %c   = OpImageSampleExplicitLod %vec4 %s %coord Lod %zero
               OpReturn
               OpFunctionEnd
```

```bash
spirv-as --target-env spv1.0 neoos-tinygl/test/host/fixtures/unsupported_opcode.spvasm \
  -o neoos-tinygl/test/host/fixtures/unsupported_opcode.spv
```

```c
/* Add to test_module.c main(): */
{
    size_t len;
    void *bytes = read_file("fixtures/unsupported_opcode.spv", &len);
    TGLspvModule *m = tglLoadSpvModule(bytes, len);
    assert(m == NULL); /* must be rejected, with the opcode named on stderr */
    free(bytes);
    printf("test_module: unsupported opcode rejected\n");
}
```

- [ ] **Step 2: Run and confirm it currently fails**

Run: `./test_module`
Expected: `assert(m == NULL)` fails, because Task 3 accepts any module with a valid entry point regardless of body content.

- [ ] **Step 3: Implement basic-block decode with a whitelist check**

```c
/* spv_module.h additions: */
typedef struct {
    uint16_t opcode;
    uint32_t result_id;   /* 0 if this opcode has no result */
    uint32_t type_id;      /* 0 if none */
    uint32_t operands[8];
    int noperands;
} spv_instr_t;

typedef struct {
    uint32_t label_id;
    spv_instr_t instrs[64];
    int ninstrs;
} spv_block_t;

typedef struct {
    spv_block_t blocks[32];
    int nblocks;
} spv_func_t;

const spv_func_t *spv_module_entry_func(const TGLspvModule *mod);
```

```c
/* spv_module.c: the whitelist used by both function-body decode and
 * (Task 5) loop-bound validation. */
static int opcode_supported(uint16_t op) {
    switch (op) {
    case SPV_OP_LOAD: case SPV_OP_STORE: case SPV_OP_ACCESS_CHAIN:
    case SPV_OP_VECTOR_SHUFFLE: case SPV_OP_COMPOSITE_CONSTRUCT:
    case SPV_OP_COMPOSITE_EXTRACT: case SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD:
    case SPV_OP_FNEGATE: case SPV_OP_FADD: case SPV_OP_FSUB:
    case SPV_OP_FMUL: case SPV_OP_FDIV: case SPV_OP_VECTOR_TIMES_SCALAR:
    case SPV_OP_VECTOR_TIMES_MATRIX: case SPV_OP_DOT: case SPV_OP_IEQUAL:
    case SPV_OP_SLESSTHAN: case SPV_OP_IADD: case SPV_OP_PHI:
    case SPV_OP_LOOP_MERGE: case SPV_OP_SELECTION_MERGE: case SPV_OP_LABEL:
    case SPV_OP_BRANCH: case SPV_OP_BRANCH_CONDITIONAL: case SPV_OP_RETURN:
    case SPV_OP_FUNCTION_END:
        return 1;
    default:
        return 0;
    }
}

static const char *opcode_name(uint16_t op) {
    /* Small lookup for error messages -- extend as opcodes are added. */
    switch (op) {
    case SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD: return "OpImageSampleImplicitLod";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "opcode %u", op);
        return buf;
    }
    }
}

static int decode_function_body(TGLspvModule *mod, const uint32_t *words,
                                 size_t nwords, size_t cursor) {
    spv_func_t *fn = &mod->func;
    fn->nblocks = 0;
    spv_block_t *cur = NULL;
    size_t i = cursor;

    /* Skip the OpFunction line itself -- already identified by
     * parse_globals's cursor handoff. */
    uint32_t wc0 = words[i] >> 16;
    i += wc0;

    while (i < nwords) {
        uint32_t instr = words[i];
        uint16_t wc = (uint16_t)(instr >> 16);
        uint16_t op = (uint16_t)(instr & 0xFFFF);
        if (wc == 0) return 0;

        if (op == SPV_OP_FUNCTION_END) { i += wc; break; }

        if (op == SPV_OP_LABEL) {
            if (fn->nblocks >= 32) { fprintf(stderr, "spv: rejected -- too many basic blocks\n"); return 0; }
            cur = &fn->blocks[fn->nblocks++];
            cur->label_id = words[i + 1];
            cur->ninstrs = 0;
            i += wc;
            continue;
        }

        if (!opcode_supported(op)) {
            fprintf(stderr, "spv: rejected -- unsupported %s\n", opcode_name(op));
            return 0;
        }

        if (!cur) { fprintf(stderr, "spv: rejected -- instruction outside any block\n"); return 0; }
        if (cur->ninstrs >= 64) { fprintf(stderr, "spv: rejected -- block too large\n"); return 0; }

        spv_instr_t *ins = &cur->instrs[cur->ninstrs++];
        ins->opcode = op;
        /* Result/type id positions vary by opcode form; a full decode
         * table is added incrementally as Tasks 6-7 need specific
         * operands. For now, capture the raw operand words verbatim
         * so later tasks can interpret them without a second parse
         * pass. */
        ins->noperands = wc - 1;
        for (int k = 0; k < ins->noperands && k < 8; k++)
            ins->operands[k] = words[i + 1 + k];

        i += wc;
    }
    return 1;
}
```

Wire it into `tglLoadSpvModule`'s tail (replacing Task 3's comment about deferring body decode):

```c
    if (!decode_function_body(mod, words, nwords, cursor)) { free(mod); return NULL; }
    return mod;
```

Add `spv_func_t func;` to `struct TGLspvModule`, and:

```c
const spv_func_t *spv_module_entry_func(const TGLspvModule *mod) { return &mod->func; }
```

- [ ] **Step 4: Run and confirm both the new and prior tests pass**

Run: `./test_module`
Expected: all four lines print, exit 0. Re-run `minimal_vertex.spv`'s test too — its `OpStore`/`OpReturn` body must now decode successfully (both opcodes are in the whitelist).

- [ ] **Step 5: Commit**

```bash
git -C neoos-tinygl add spv/spv_module.c spv/spv_module.h test/host/test_module.c test/host/fixtures/unsupported_opcode.spvasm test/host/fixtures/unsupported_opcode.spv
git -C neoos-tinygl commit -m "spv: decode function bodies into basic blocks, reject unsupported opcodes"
```

---

## Task 5: Bounded-loop validation

**Files:**
- Modify: `neoos-tinygl/spv/spv_module.c`
- Test: `neoos-tinygl/test/host/test_module.c`, new fixtures `fixtures/bounded_loop.spvasm`, `fixtures/unbounded_loop.spvasm`

**Interfaces:**
- Produces: on `spv_func_t`, each block with `SPV_OP_LOOP_MERGE` gets an entry in a new `int trip_count[32]` array on `TGLspvModule` (indexed by block index), populated only for loops that pass validation; Task 7's VM reads this to know how many times to execute the loop body without re-deriving it.

- [ ] **Step 1: Write the bounded-loop fixture (trip count 4) and failing acceptance test**

```
; fixtures/bounded_loop.spvasm -- for (int i = 0; i < 4; i++) {} with a
; statically constant bound and step, inside a Fragment entry point.
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main"
               OpExecutionMode %main OriginUpperLeft
        %void = OpTypeVoid
        %fnty = OpTypeFunction %void
         %int = OpTypeInt 32 1
        %zero = OpConstant %int 0
         %one = OpConstant %int 1
        %four = OpConstant %int 4
        %bool = OpTypeBool
        %main = OpFunction %void None %fnty
       %entry = OpLabel
                OpBranch %header
      %header = OpLabel
           %i = OpPhi %int %zero %entry %inext %continue
                OpLoopMerge %merge %continue None
                OpBranch %check
       %check = OpLabel
        %cond = OpSLessThan %bool %i %four
                OpBranchConditional %cond %continue %merge
    %continue = OpLabel
       %inext = OpIAdd %int %i %one
                OpBranch %header
       %merge = OpLabel
                OpReturn
                OpFunctionEnd
```

```
; fixtures/unbounded_loop.spvasm -- identical shape, but the bound
; %four is replaced by a value loaded from a variable, so no constant
; trip count exists.
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main"
               OpExecutionMode %main OriginUpperLeft
        %void = OpTypeVoid
        %fnty = OpTypeFunction %void
         %int = OpTypeInt 32 1
    %ptr_int  = OpTypePointer Input %int
      %bound_v = OpVariable %ptr_int Input
        %zero = OpConstant %int 0
         %one = OpConstant %int 1
        %bool = OpTypeBool
        %main = OpFunction %void None %fnty
       %entry = OpLabel
       %bound = OpLoad %int %bound_v
                OpBranch %header
      %header = OpLabel
           %i = OpPhi %int %zero %entry %inext %continue
                OpLoopMerge %merge %continue None
                OpBranch %check
       %check = OpLabel
        %cond = OpSLessThan %bool %i %bound
                OpBranchConditional %cond %continue %merge
    %continue = OpLabel
       %inext = OpIAdd %int %i %one
                OpBranch %header
       %merge = OpLabel
                OpReturn
                OpFunctionEnd
```

```bash
spirv-as --target-env spv1.0 neoos-tinygl/test/host/fixtures/bounded_loop.spvasm -o neoos-tinygl/test/host/fixtures/bounded_loop.spv
spirv-as --target-env spv1.0 neoos-tinygl/test/host/fixtures/unbounded_loop.spvasm -o neoos-tinygl/test/host/fixtures/unbounded_loop.spv
```

```c
/* Add to test_module.c main(): */
{
    size_t len;
    void *bytes = read_file("fixtures/bounded_loop.spv", &len);
    TGLspvModule *m = tglLoadSpvModule(bytes, len);
    assert(m != NULL);
    tglFreeSpvModule(m);
    free(bytes);
    printf("test_module: bounded loop accepted\n");
}
{
    size_t len;
    void *bytes = read_file("fixtures/unbounded_loop.spv", &len);
    TGLspvModule *m = tglLoadSpvModule(bytes, len);
    assert(m == NULL);
    free(bytes);
    printf("test_module: unbounded loop rejected\n");
}
```

Note both fixtures need `OpTypeInt`, `OpPhi`, and `OpIAdd` decode support in Task 4's `opcode_supported`/operand capture — `OpTypeInt`/`OpTypeBool` belong in Task 2's type-table `switch`; add them now since Task 2 only covered float/vector/matrix/array/pointer:

```c
/* Add to parse_globals's switch, alongside Task 2's other OpType* cases: */
        case SPV_OP_TYPE_INT: {
            uint32_t id = words[i + 1];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = SPV_TYPE_FLOAT; /* stored as float internally -- the VM (Task 6) treats int/float registers uniformly since every use here is arithmetic on small counters */
            mod->has_type[id] = 1;
            break;
        }
        case SPV_OP_TYPE_BOOL: {
            uint32_t id = words[i + 1];
            if (id >= SPV_MAX_IDS) return 0;
            mod->types[id].kind = SPV_TYPE_FLOAT;
            mod->has_type[id] = 1;
            break;
        }
```

- [ ] **Step 2: Run and confirm both new assertions currently fail**

Run: `./test_module`
Expected: fails at the bounded-loop `assert(m != NULL)` (loop validation doesn't exist, but decode may already succeed if only whitelisting matters — check by running; if it unexpectedly already passes, the unbounded case is the one that must fail, since nothing yet distinguishes them). Confirm empirically which assertion fails before writing Step 3, and adjust the written expectation to match reality rather than guessing.

- [ ] **Step 3: Implement loop-bound validation**

```c
/* Added to TGLspvModule: */
    int trip_count[32];      /* indexed by the LoopMerge block's index in func.blocks */
    int has_trip_count[32];

/* A loop header block ends in OpLoopMerge then OpBranch to a "check"
 * block that computes the exit condition via OpSLessThan/OpIEqual
 * against a constant, branching via OpBranchConditional into either
 * the continue block (loop body) or the merge block (exit). The
 * induction variable is defined by OpPhi in the header, incremented
 * by a constant OpIAdd in the continue block. This function finds
 * that shape and computes the trip count, or returns 0. */
static int validate_loop(const TGLspvModule *mod, const spv_func_t *fn,
                          int header_idx, int *out_trip_count) {
    const spv_block_t *header = &fn->blocks[header_idx];

    /* Find the Phi's initial constant and increment constant. */
    int32_t init_val = 0, step_val = 0;
    int have_init = 0, have_step = 0;
    for (int k = 0; k < header->ninstrs; k++) {
        if (header->instrs[k].opcode == SPV_OP_PHI) {
            uint32_t init_id = header->instrs[k].operands[0];
            if (mod->has_const[init_id]) {
                init_val = (int32_t)mod->consts[init_id].f32[0];
                have_init = 1;
            }
        }
    }
    /* Find the increment: an OpIAdd whose one operand is a constant,
     * anywhere in the function (the continue block, in the fixtures'
     * shape). */
    for (int b = 0; b < fn->nblocks && !have_step; b++) {
        for (int k = 0; k < fn->blocks[b].ninstrs; k++) {
            if (fn->blocks[b].instrs[k].opcode == SPV_OP_IADD) {
                uint32_t rhs = fn->blocks[b].instrs[k].operands[1];
                if (mod->has_const[rhs]) {
                    step_val = (int32_t)mod->consts[rhs].f32[0];
                    have_step = 1;
                }
            }
        }
    }
    /* Find the bound: an OpSLessThan whose one operand is a constant,
     * anywhere in the function. */
    int32_t bound_val = 0;
    int have_bound = 0;
    for (int b = 0; b < fn->nblocks && !have_bound; b++) {
        for (int k = 0; k < fn->blocks[b].ninstrs; k++) {
            if (fn->blocks[b].instrs[k].opcode == SPV_OP_SLESSTHAN) {
                uint32_t rhs = fn->blocks[b].instrs[k].operands[1];
                if (mod->has_const[rhs]) {
                    bound_val = (int32_t)mod->consts[rhs].f32[0];
                    have_bound = 1;
                }
            }
        }
    }

    if (!have_init || !have_step || !have_bound || step_val == 0) return 0;

    long long count = ((long long)bound_val - (long long)init_val) / step_val;
    if (count <= 0 || count > 4096) return 0;

    *out_trip_count = (int)count;
    return 1;
}

/* Called once after decode_function_body succeeds, before
 * tglLoadSpvModule returns success: */
static int validate_all_loops(TGLspvModule *mod) {
    spv_func_t *fn = &mod->func;
    for (int b = 0; b < fn->nblocks; b++) {
        for (int k = 0; k < fn->blocks[b].ninstrs; k++) {
            if (fn->blocks[b].instrs[k].opcode == SPV_OP_LOOP_MERGE) {
                int trip;
                if (!validate_loop(mod, fn, b, &trip)) {
                    fprintf(stderr, "spv: rejected -- loop at block %d has no "
                            "statically determinable, bounded trip count\n", b);
                    return 0;
                }
                mod->trip_count[b] = trip;
                mod->has_trip_count[b] = 1;
            }
        }
    }
    return 1;
}
```

Wire into `tglLoadSpvModule`, after `decode_function_body` succeeds:

```c
    if (!decode_function_body(mod, words, nwords, cursor)) { free(mod); return NULL; }
    if (!validate_all_loops(mod)) { free(mod); return NULL; }
    return mod;
```

- [ ] **Step 4: Run and confirm both assertions now pass, with no regression on Tasks 1-4's tests**

Run: `./test_module`
Expected: every printed line from Tasks 1-5 appears, exit 0.

- [ ] **Step 5: Commit**

```bash
git -C neoos-tinygl add spv/spv_module.c test/host/test_module.c test/host/fixtures/bounded_loop.spvasm test/host/fixtures/bounded_loop.spv test/host/fixtures/unbounded_loop.spvasm test/host/fixtures/unbounded_loop.spv
git -C neoos-tinygl commit -m "spv: validate loop trip counts at load time, reject unbounded loops"
```

---

## Task 6: VM register file and straight-line arithmetic execution

**Files:**
- Create: `neoos-tinygl/spv/spv_vm.h`
- Create: `neoos-tinygl/spv/spv_vm.c`
- Test: `neoos-tinygl/test/host/test_vm.c`, fixture `fixtures/straight_line_arith.spvasm`

**Interfaces:**
- Consumes: `spv_func_t`/`spv_instr_t` from Task 4, `spv_const_t`/`spv_type_t` tables from Task 2.
- Produces: `typedef struct { float v[16]; } spv_value_t;` and `void spv_vm_run_fragment(const TGLspvModule *mod, const spv_value_t *inputs, int ninputs, spv_value_t *output)`, which Task 7 extends with control flow and Task 8 extends with texture sampling; Task 9's `spv_gl.c` calls this directly.

- [ ] **Step 1: Write the fixture and failing test — a fragment shader computing `dot(vec4(1,2,3,4), vec4(1,1,1,1))` (=10) via straight-line arithmetic**

```
; fixtures/straight_line_arith.spvasm
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %out_color
               OpExecutionMode %main OriginUpperLeft
        %void = OpTypeVoid
        %fnty = OpTypeFunction %void
       %float = OpTypeFloat 32
        %vec4 = OpTypeVector %float 4
     %out_ptr = OpTypePointer Output %vec4
  %out_color = OpVariable %out_ptr Output
          %a  = OpConstantComposite %vec4 %f1 %f2 %f3 %f4
          %b  = OpConstantComposite %vec4 %f1b %f1b %f1b %f1b
          %f1 = OpConstant %float 1.0
          %f2 = OpConstant %float 2.0
          %f3 = OpConstant %float 3.0
          %f4 = OpConstant %float 4.0
         %f1b = OpConstant %float 1.0
        %main = OpFunction %void None %fnty
       %entry = OpLabel
           %d = OpDot %float %a %b
        %res  = OpCompositeConstruct %vec4 %d %d %d %d
                OpStore %out_color %res
                OpReturn
                OpFunctionEnd
```

```bash
spirv-as --target-env spv1.0 neoos-tinygl/test/host/fixtures/straight_line_arith.spvasm \
  -o neoos-tinygl/test/host/fixtures/straight_line_arith.spv
```

```c
/* neoos-tinygl/test/host/test_vm.c */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../../spv/spv_module.h"
#include "../../spv/spv_vm.h"

static void *read_file(const char *path, size_t *len); /* same as test_module.c's helper -- duplicated here since host tests don't share a build unit */

int main(void) {
    size_t len;
    void *bytes = read_file("fixtures/straight_line_arith.spv", &len);
    TGLspvModule *m = tglLoadSpvModule(bytes, len);
    assert(m != NULL);

    spv_value_t output;
    spv_vm_run_fragment(m, NULL, 0, &output);
    /* dot((1,2,3,4),(1,1,1,1)) == 10, broadcast into all 4 components */
    assert(fabsf(output.v[0] - 10.0f) < 1e-4f);
    assert(fabsf(output.v[3] - 10.0f) < 1e-4f);

    tglFreeSpvModule(m);
    free(bytes);
    printf("test_vm: straight-line arithmetic OK\n");
    return 0;
}
```

- [ ] **Step 2: Run and confirm it fails to build (`spv_vm.h`/`spv_vm.c` don't exist)**

Run: `cd neoos-tinygl/test/host && make test_vm`
Expected: FAIL — missing `spv_vm.h`.

- [ ] **Step 3: Implement the register file and arithmetic execution**

```c
/* spv/spv_vm.h */
#ifndef NEOOS_SPV_VM_H
#define NEOOS_SPV_VM_H
#include "spv_module.h"

typedef struct { float v[16]; } spv_value_t; /* [0..3]=vec4 or [0..15]=mat4 */

void spv_vm_run_fragment(const TGLspvModule *mod, const spv_value_t *inputs,
                          int ninputs, spv_value_t *output);
void spv_vm_run_vertex(const TGLspvModule *mod, const spv_value_t *inputs,
                        int ninputs, spv_value_t *output);

#endif
```

```c
/* spv/spv_vm.c */
#include <string.h>
#include <math.h>
#include "spv_vm.h"

#define SPV_MAX_REGS 4096

typedef struct {
    spv_value_t regs[SPV_MAX_REGS];
    int has_reg[SPV_MAX_REGS];
} spv_regfile_t;

static spv_value_t reg_read(const TGLspvModule *mod, spv_regfile_t *rf, uint32_t id) {
    if (rf->has_reg[id]) return rf->regs[id];
    if (mod->has_const[id]) {
        spv_value_t v;
        memcpy(v.v, mod->consts[id].f32, sizeof(v.v));
        return v;
    }
    spv_value_t zero; memset(&zero, 0, sizeof(zero));
    return zero;
}

static void reg_write(spv_regfile_t *rf, uint32_t id, spv_value_t v) {
    rf->regs[id] = v;
    rf->has_reg[id] = 1;
}

/* Executes one basic block's straight-line instructions (Task 7 adds
 * branch handling around calls to this). Store targets are recorded
 * into rf under the pointer <id> itself -- this VM has no separate
 * memory model; OpVariable/OpStore/OpLoad all operate directly on the
 * register file, which is sufficient for the supported subset (no
 * aliasing, no pointer arithmetic). */
static void exec_block(const TGLspvModule *mod, const spv_block_t *blk,
                        spv_regfile_t *rf, uint32_t *out_var_id) {
    for (int k = 0; k < blk->ninstrs; k++) {
        const spv_instr_t *ins = &blk->instrs[k];
        switch (ins->opcode) {
        case SPV_OP_DOT: {
            /* operands: [0]=vec1_id [1]=vec2_id (result id/type handled below) */
            spv_value_t a = reg_read(mod, rf, ins->operands[0]);
            spv_value_t b = reg_read(mod, rf, ins->operands[1]);
            float d = a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2] + a.v[3]*b.v[3];
            spv_value_t r; memset(&r, 0, sizeof(r)); r.v[0] = d;
            reg_write(rf, ins->result_id, r);
            break;
        }
        case SPV_OP_FADD: case SPV_OP_FSUB: case SPV_OP_FMUL: case SPV_OP_FDIV: {
            spv_value_t a = reg_read(mod, rf, ins->operands[0]);
            spv_value_t b = reg_read(mod, rf, ins->operands[1]);
            spv_value_t r;
            for (int c = 0; c < 4; c++) {
                r.v[c] = ins->opcode == SPV_OP_FADD ? a.v[c] + b.v[c]
                        : ins->opcode == SPV_OP_FSUB ? a.v[c] - b.v[c]
                        : ins->opcode == SPV_OP_FMUL ? a.v[c] * b.v[c]
                        : (b.v[c] != 0.0f ? a.v[c] / b.v[c] : 0.0f);
            }
            reg_write(rf, ins->result_id, r);
            break;
        }
        case SPV_OP_FNEGATE: {
            spv_value_t a = reg_read(mod, rf, ins->operands[0]);
            spv_value_t r;
            for (int c = 0; c < 4; c++) r.v[c] = -a.v[c];
            reg_write(rf, ins->result_id, r);
            break;
        }
        case SPV_OP_COMPOSITE_CONSTRUCT: {
            spv_value_t r; memset(&r, 0, sizeof(r));
            for (int c = 0; c < ins->noperands - 1 && c < 4; c++)
                r.v[c] = reg_read(mod, rf, ins->operands[c]).v[0];
            reg_write(rf, ins->result_id, r);
            break;
        }
        case SPV_OP_STORE: {
            uint32_t ptr_id = ins->operands[0];
            uint32_t val_id = ins->operands[1];
            reg_write(rf, ptr_id, reg_read(mod, rf, val_id));
            if (out_var_id) *out_var_id = ptr_id;
            break;
        }
        case SPV_OP_LOAD: {
            uint32_t ptr_id = ins->operands[0];
            reg_write(rf, ins->result_id, reg_read(mod, rf, ptr_id));
            break;
        }
        case SPV_OP_RETURN: case SPV_OP_LABEL: case SPV_OP_FUNCTION_END:
            break;
        default:
            /* Control-flow opcodes (Phi/Branch/BranchConditional/
             * LoopMerge/SelectionMerge) and ImageSampleImplicitLod are
             * handled by Tasks 7-8's block-walking driver, not here --
             * this function only executes ONE block's straight-line
             * body. */
            break;
        }
    }
}

void spv_vm_run_fragment(const TGLspvModule *mod, const spv_value_t *inputs,
                          int ninputs, spv_value_t *output) {
    spv_regfile_t rf;
    memset(&rf, 0, sizeof(rf));
    /* Task 8 binds the varying inputs to fixed register ids matching
     * the module's declared Input variables; Task 6 has no varyings
     * yet, so this loop is a no-op until then. */
    (void)inputs; (void)ninputs;

    const spv_func_t *fn = spv_module_entry_func(mod);
    uint32_t out_var = 0;
    for (int b = 0; b < fn->nblocks; b++)
        exec_block(mod, &fn->blocks[b], &rf, &out_var);

    *output = out_var ? rf.regs[out_var] : (spv_value_t){{0}};
}

void spv_vm_run_vertex(const TGLspvModule *mod, const spv_value_t *inputs,
                        int ninputs, spv_value_t *output) {
    spv_vm_run_fragment(mod, inputs, ninputs, output); /* identical driver until Task 9 differentiates by entry model if ever needed */
}
```

- [ ] **Step 4: Run and confirm it passes**

Run: `cd neoos-tinygl/test/host && make test_vm && ./test_vm`
Expected: `test_vm: straight-line arithmetic OK`

- [ ] **Step 5: Commit**

```bash
git -C neoos-tinygl add spv/spv_vm.h spv/spv_vm.c test/host/test_vm.c test/host/fixtures/straight_line_arith.spvasm test/host/fixtures/straight_line_arith.spv
git -C neoos-tinygl commit -m "spv: VM register file and straight-line arithmetic execution"
```

---

## Task 7: Control flow — if/else and bounded loops

**Files:**
- Modify: `neoos-tinygl/spv/spv_vm.c`
- Test: `neoos-tinygl/test/host/test_vm.c` (reuse Task 5's `bounded_loop.spv` fixture, extended with an output store)

**Interfaces:**
- Produces: `exec_block`'s driver becomes a real block-walking interpreter (`run_function` replacing Task 6's flat `for` loop over `fn->blocks`), handling `OpBranch`/`OpBranchConditional`/`OpLoopMerge`/`OpPhi` using Task 5's precomputed `trip_count`.

- [ ] **Step 1: Extend `bounded_loop.spvasm` to store its final induction value, and write a failing test expecting output `4`**

```
; fixtures/bounded_loop_output.spvasm -- same shape as Task 5's
; bounded_loop.spvasm, but stores the final %i into an Output variable
; so the VM's result is observable.
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %out_color
               OpExecutionMode %main OriginUpperLeft
        %void = OpTypeVoid
        %fnty = OpTypeFunction %void
         %int = OpTypeInt 32 1
       %float = OpTypeFloat 32
     %out_ptr = OpTypePointer Output %float
  %out_color = OpVariable %out_ptr Output
        %zero = OpConstant %int 0
         %one = OpConstant %int 1
        %four = OpConstant %int 4
        %bool = OpTypeBool
        %main = OpFunction %void None %fnty
       %entry = OpLabel
                OpBranch %header
      %header = OpLabel
           %i = OpPhi %int %zero %entry %inext %continue
                OpLoopMerge %merge %continue None
                OpBranch %check
       %check = OpLabel
        %cond = OpSLessThan %bool %i %four
                OpBranchConditional %cond %continue %merge
    %continue = OpLabel
       %inext = OpIAdd %int %i %one
                OpBranch %header
       %merge = OpLabel
        %ffin = OpConvertSToF %float %i
                OpStore %out_color %ffin
                OpReturn
                OpFunctionEnd
```

`OpConvertSToF` is new — add it to Task 4's `opcode_supported` whitelist and give it a trivial VM case (copy `.v[0]`, since this VM stores everything as float already per Task 2's int-as-float decision):

```c
/* spv_defs.h: add */
    SPV_OP_CONVERT_S_TO_F = 111,
/* spv_module.c opcode_supported: add case SPV_OP_CONVERT_S_TO_F: */
/* spv_vm.c exec_block: add */
        case SPV_OP_CONVERT_S_TO_F: {
            spv_value_t a = reg_read(mod, rf, ins->operands[0]);
            reg_write(rf, ins->result_id, a); /* already float-valued internally */
            break;
        }
```

```bash
spirv-as --target-env spv1.0 neoos-tinygl/test/host/fixtures/bounded_loop_output.spvasm \
  -o neoos-tinygl/test/host/fixtures/bounded_loop_output.spv
```

```c
/* Add to test_vm.c main(): */
{
    size_t len;
    void *bytes = read_file("fixtures/bounded_loop_output.spv", &len);
    TGLspvModule *m = tglLoadSpvModule(bytes, len);
    assert(m != NULL);
    spv_value_t output;
    spv_vm_run_fragment(m, NULL, 0, &output);
    assert(fabsf(output.v[0] - 4.0f) < 1e-4f); /* loop ran exactly 4 times */
    tglFreeSpvModule(m);
    free(bytes);
    printf("test_vm: bounded loop executes the right number of iterations\n");
}
```

- [ ] **Step 2: Run and confirm it fails**

Run: `./test_vm`
Expected: fails, since Task 6's driver runs every block exactly once in file order, ignoring branches/loop iteration entirely — the stored value will be wrong (likely `0` or `1`, not `4`).

- [ ] **Step 3: Implement the block-walking driver with loop and branch handling**

```c
/* Replaces the flat "for (b...) exec_block(...)" loop in both
 * spv_vm_run_fragment and spv_vm_run_vertex. */
static const spv_block_t *find_block(const spv_func_t *fn, uint32_t label_id) {
    for (int b = 0; b < fn->nblocks; b++)
        if (fn->blocks[b].label_id == label_id) return &fn->blocks[b];
    return NULL;
}

static int block_index(const spv_func_t *fn, uint32_t label_id) {
    for (int b = 0; b < fn->nblocks; b++)
        if (fn->blocks[b].label_id == label_id) return b;
    return -1;
}

/* Returns the label id this block branches to (its last instruction's
 * target), or 0 if the block ends in OpReturn. For
 * OpBranchConditional, `cond_true`/`cond_false` are filled in and the
 * function returns 0xFFFFFFFF as a sentinel meaning "caller must
 * choose". Phi results in the TARGET block are resolved by the caller
 * using which predecessor it came from. */
#define SPV_BRANCH_COND 0xFFFFFFFFu

static uint32_t block_terminator(const TGLspvModule *mod, const spv_block_t *blk,
                                  spv_regfile_t *rf, uint32_t *cond_true, uint32_t *cond_false) {
    for (int k = blk->ninstrs - 1; k >= 0; k--) {
        const spv_instr_t *ins = &blk->instrs[k];
        if (ins->opcode == SPV_OP_RETURN) return 0;
        if (ins->opcode == SPV_OP_BRANCH) return ins->operands[0];
        if (ins->opcode == SPV_OP_BRANCH_CONDITIONAL) {
            spv_value_t c = reg_read(mod, rf, ins->operands[0]);
            *cond_true = ins->operands[1];
            *cond_false = ins->operands[2];
            return c.v[0] != 0.0f ? *cond_true : *cond_false;
        }
    }
    return 0;
}

/* Resolves every OpPhi at the top of `blk`, given the label of the
 * block control arrived FROM. */
static void resolve_phis(const TGLspvModule *mod, const spv_block_t *blk,
                          uint32_t from_label, spv_regfile_t *rf) {
    for (int k = 0; k < blk->ninstrs; k++) {
        const spv_instr_t *ins = &blk->instrs[k];
        if (ins->opcode != SPV_OP_PHI) continue;
        for (int p = 0; p + 1 < ins->noperands; p += 2) {
            uint32_t val_id = ins->operands[p];
            uint32_t pred_label = ins->operands[p + 1];
            if (pred_label == from_label) {
                reg_write(rf, ins->result_id, reg_read(mod, rf, val_id));
                break;
            }
        }
    }
}

static void run_function(const TGLspvModule *mod, spv_regfile_t *rf, uint32_t *out_var_id) {
    const spv_func_t *fn = spv_module_entry_func(mod);
    if (fn->nblocks == 0) return;

    uint32_t cur_label = fn->blocks[0].label_id;
    uint32_t prev_label = 0;
    int guard = 0; /* Task 5 already bounded every loop at load time, but a
                    * runtime guard against a validator bug staying safe
                    * costs nothing and never fires on valid input. */

    while (guard++ < 100000) {
        int idx = block_index(fn, cur_label);
        if (idx < 0) return;
        const spv_block_t *blk = &fn->blocks[idx];

        if (prev_label) resolve_phis(mod, blk, prev_label, rf);
        exec_block(mod, blk, rf, out_var_id);

        uint32_t ct = 0, cf = 0;
        uint32_t next = block_terminator(mod, blk, rf, &ct, &cf);
        if (next == 0) return; /* OpReturn */

        prev_label = cur_label;
        cur_label = next;
    }
}
```

Replace both `spv_vm_run_fragment`/`spv_vm_run_vertex` bodies' block loop with:

```c
    uint32_t out_var = 0;
    run_function(mod, &rf, &out_var);
    *output = out_var ? rf.regs[out_var] : (spv_value_t){{0}};
```

Note this driver handles `OpLoopMerge` implicitly: the loop header block re-executes each time control branches back to it via the continue block's `OpBranch %header`, and Task 5's load-time validation already guarantees this terminates within 4096 iterations — `run_function`'s `guard` is a defense-in-depth backstop, not the mechanism that bounds the loop.

- [ ] **Step 4: Run and confirm the new test passes with no regression**

Run: `./test_vm`
Expected: both Task 6 and Task 7 lines print, exit 0.

- [ ] **Step 5: Commit**

```bash
git -C neoos-tinygl add spv/spv_vm.c spv/spv_defs.h spv/spv_module.c test/host/test_vm.c test/host/fixtures/bounded_loop_output.spvasm test/host/fixtures/bounded_loop_output.spv
git -C neoos-tinygl commit -m "spv: execute branches, loops, and Phi nodes"
```

---

## Task 8: Texture sampling

**Files:**
- Modify: `neoos-tinygl/spv/spv_vm.h`, `spv_vm.c`
- Test: `neoos-tinygl/test/host/test_sampling.c`, fixture `fixtures/sample_texture.spvasm`

**Interfaces:**
- Produces: `void spv_vm_bind_texture(const uint8_t *rgba, int w, int h)` (module-global for this VM instance — one bound texture unit, matching the spec's "exactly one bound 2D texture per fragment invocation") and the `OpImageSampleImplicitLod` VM case that reads coordinates from a register and nearest-samples the bound buffer.

- [ ] **Step 1: Write the fixture and failing test — sample a 2x2 texture at (0.75, 0.75), expecting the bottom-right pixel**

```
; fixtures/sample_texture.spvasm
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %out_color
               OpExecutionMode %main OriginUpperLeft
        %void = OpTypeVoid
        %fnty = OpTypeFunction %void
       %float = OpTypeFloat 32
        %vec2 = OpTypeVector %float 2
        %vec4 = OpTypeVector %float 4
       %img_t = OpTypeImage %float 2D 0 0 0 1 Unknown
      %simg_t = OpTypeSampledImage %img_t
    %ptr_simg = OpTypePointer UniformConstant %simg_t
         %tex = OpVariable %ptr_simg UniformConstant
     %out_ptr = OpTypePointer Output %vec4
   %out_color = OpVariable %out_ptr Output
       %c0 = OpConstant %float 0.75
      %coord = OpConstantComposite %vec2 %c0 %c0
        %main = OpFunction %void None %fnty
       %entry = OpLabel
           %s = OpLoad %simg_t %tex
           %c = OpImageSampleImplicitLod %vec4 %s %coord
                OpStore %out_color %c
                OpReturn
                OpFunctionEnd
```

```bash
spirv-as --target-env spv1.0 neoos-tinygl/test/host/fixtures/sample_texture.spvasm \
  -o neoos-tinygl/test/host/fixtures/sample_texture.spv
```

```c
/* neoos-tinygl/test/host/test_sampling.c */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../../spv/spv_module.h"
#include "../../spv/spv_vm.h"

static void *read_file(const char *path, size_t *len);

int main(void) {
    size_t len;
    void *bytes = read_file("fixtures/sample_texture.spv", &len);
    TGLspvModule *m = tglLoadSpvModule(bytes, len);
    assert(m != NULL);

    /* 2x2 RGBA texture: TL=red, TR=green, BL=blue, BR=white. */
    uint8_t tex[2 * 2 * 4] = {
        255,0,0,255,   0,255,0,255,
        0,0,255,255,   255,255,255,255,
    };
    spv_vm_bind_texture(tex, 2, 2);

    spv_value_t output;
    spv_vm_run_fragment(m, NULL, 0, &output);
    /* (0.75, 0.75) lands in the bottom-right texel -> white */
    assert(fabsf(output.v[0] - 1.0f) < 1e-2f);
    assert(fabsf(output.v[1] - 1.0f) < 1e-2f);
    assert(fabsf(output.v[2] - 1.0f) < 1e-2f);

    tglFreeSpvModule(m);
    free(bytes);
    printf("test_sampling: nearest-sample texture fetch OK\n");
    return 0;
}
```

- [ ] **Step 2: Run and confirm it fails to build**

Run: `cd neoos-tinygl/test/host && make test_sampling`
Expected: FAIL — `spv_vm_bind_texture` undeclared, and `OpImageSampleImplicitLod`/`OpTypeImage`/`OpTypeSampledImage`/`OpLoad` (of a sampled-image type) aren't fully wired yet.

- [ ] **Step 3: Implement texture binding and sampling**

```c
/* spv_vm.h: add */
void spv_vm_bind_texture(const uint8_t *rgba, int w, int h);
```

```c
/* spv_vm.c: module-level state -- one bound texture unit, matching the
 * spec's "exactly one bound 2D texture per fragment invocation". */
static const uint8_t *g_tex_rgba = NULL;
static int g_tex_w = 0, g_tex_h = 0;

void spv_vm_bind_texture(const uint8_t *rgba, int w, int h) {
    g_tex_rgba = rgba; g_tex_w = w; g_tex_h = h;
}

/* In exec_block's switch, add: */
        case SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD: {
            spv_value_t coord = reg_read(mod, rf, ins->operands[1]);
            spv_value_t r; memset(&r, 0, sizeof(r));
            if (g_tex_rgba && g_tex_w > 0 && g_tex_h > 0) {
                int x = (int)(coord.v[0] * g_tex_w);
                int y = (int)(coord.v[1] * g_tex_h);
                if (x < 0) x = 0; if (x >= g_tex_w) x = g_tex_w - 1;
                if (y < 0) y = 0; if (y >= g_tex_h) y = g_tex_h - 1;
                const uint8_t *px = &g_tex_rgba[(y * g_tex_w + x) * 4];
                r.v[0] = px[0] / 255.0f; r.v[1] = px[1] / 255.0f;
                r.v[2] = px[2] / 255.0f; r.v[3] = px[3] / 255.0f;
            }
            reg_write(rf, ins->result_id, r);
            break;
        }
```

`OpTypeImage`/`OpTypeSampledImage`/`OpTypePointer(UniformConstant)`/`OpVariable(UniformConstant)` need to parse without error in Task 2/3's `parse_globals` (they're legal but were previously falling into the `default: break` no-op case, which already tolerates them — confirm this by running the test before adding any new parsing code; only add cases if the test reveals a specific rejection). `OpLoad` of a sampled-image just needs to not crash: since Task 6's `SPV_OP_LOAD` case reads `reg_read(mod, rf, ptr_id)` generically, loading an opaque handle behaves as a no-op passthrough, which is sufficient — `OpImageSampleImplicitLod`'s `operands[0]` (the loaded sampled-image register) is never dereferenced, only `operands[1]` (the coordinate) is used, since this VM has exactly one global bound texture rather than modeling descriptor sets.

Also add `SPV_OP_TYPE_IMAGE`, `SPV_OP_TYPE_SAMPLED_IMAGE` to `opcode_supported`'s equivalent for globals (they're already tolerated by the `default` no-op in `parse_globals`, so no change needed there) — but they DO need to be added to `decode_function_body`'s `opcode_supported` whitelist if any instruction inside a function references them directly (it doesn't in this fixture: only `OpLoad`/`OpImageSampleImplicitLod`/`OpStore`/`OpReturn` appear in the body, all already whitelisted).

- [ ] **Step 4: Run and confirm it passes**

Run: `cd neoos-tinygl/test/host && make test_sampling && ./test_sampling`
Expected: `test_sampling: nearest-sample texture fetch OK`. If parsing fails first, run with `2>&1` to see which opcode's rejection fires and add exactly that case to `parse_globals` before re-running — don't pre-guess beyond what's listed above.

- [ ] **Step 5: Commit**

```bash
git -C neoos-tinygl add spv/spv_vm.h spv/spv_vm.c test/host/test_sampling.c test/host/fixtures/sample_texture.spvasm test/host/fixtures/sample_texture.spv test/host/Makefile
git -C neoos-tinygl commit -m "spv: nearest-neighbor 2D texture sampling"
```

---

## Task 9: TinyGL integration — the two rasterizer hooks and public API

**Files:**
- Create: `neoos-tinygl/spv/spv_gl.h`
- Create: `neoos-tinygl/spv/spv_gl.c`
- Modify: `neoos-tinygl/upstream/src/ztriangle.c` (hook, ~3 lines)
- Modify: `neoos-tinygl/upstream/src/zraster.c` (hook, ~3 lines)
- Modify: `neoos-tinygl/Makefile` (build `spv/*.c` into `libTinyGL.a`)

**Interfaces:**
- Consumes: `spv_vm_run_vertex`/`spv_vm_run_fragment`/`spv_vm_bind_texture` from Task 8, `tglLoadSpvModule`/`tglFreeSpvModule` from Task 1.
- Produces: `void tglUseSpvProgram(TGLspvModule *vs, TGLspvModule *fs)` — the only new public symbol an application calls.

- [ ] **Step 1: Locate the exact hook points**

```bash
grep -n "ZB_fillTriangleFlat\|ZB_fillTriangleSmooth\|ZB_fillTriangleMapping" neoos-tinygl/upstream/src/ztriangle.c | head -5
grep -n "^void\|zb_fill_tri" neoos-tinygl/upstream/src/zraster.c | head -20
```

Read the ~20 lines around each match found before writing the hooks below, to confirm the exact function names/signatures in this vendored version — TinyGL forks vary slightly, and the plan's hook code must match what's actually there rather than an assumed signature.

- [ ] **Step 2: Write `spv_gl.h`/`spv_gl.c`**

```c
/* spv/spv_gl.h */
#ifndef NEOOS_SPV_GL_H
#define NEOOS_SPV_GL_H
#include "spv_module.h"

void tglUseSpvProgram(TGLspvModule *vs, TGLspvModule *fs);

/* Called from the two upstream hooks -- not part of the public API. */
int spv_gl_has_fragment_program(void);
int spv_gl_has_vertex_program(void);
void spv_gl_run_fragment(const float *varyings, int nvaryings, float *out_rgba);
void spv_gl_run_vertex(const float *attribs, int nattribs, float *out_clip);

#endif
```

```c
/* spv/spv_gl.c */
#include "spv_gl.h"
#include "spv_vm.h"

static TGLspvModule *g_vs = NULL;
static TGLspvModule *g_fs = NULL;

void tglUseSpvProgram(TGLspvModule *vs, TGLspvModule *fs) {
    if (vs) { /* Fail loudly rather than silently accepting a mis-bound stage -- see spec's "translation not emulation" rule. */
        if (tglSpvEntryModel(vs) != SPV_EXEC_MODEL_VERTEX) {
            fprintf(stderr, "spv: rejected -- module bound as vertex program has a non-Vertex entry point\n");
            return;
        }
    }
    if (fs) {
        if (tglSpvEntryModel(fs) != SPV_EXEC_MODEL_FRAGMENT) {
            fprintf(stderr, "spv: rejected -- module bound as fragment program has a non-Fragment entry point\n");
            return;
        }
    }
    g_vs = vs; g_fs = fs;
}

int spv_gl_has_fragment_program(void) { return g_fs != NULL; }
int spv_gl_has_vertex_program(void) { return g_vs != NULL; }

void spv_gl_run_fragment(const float *varyings, int nvaryings, float *out_rgba) {
    spv_value_t in, out;
    memset(in.v, 0, sizeof(in.v));
    for (int i = 0; i < nvaryings && i < 16; i++) in.v[i] = varyings[i];
    spv_vm_run_fragment(g_fs, &in, 1, &out);
    for (int i = 0; i < 4; i++) out_rgba[i] = out.v[i];
}

void spv_gl_run_vertex(const float *attribs, int nattribs, float *out_clip) {
    spv_value_t in, out;
    memset(in.v, 0, sizeof(in.v));
    for (int i = 0; i < nattribs && i < 16; i++) in.v[i] = attribs[i];
    spv_vm_run_vertex(g_vs, &in, 1, &out);
    for (int i = 0; i < 4; i++) out_clip[i] = out.v[i];
}
```

`#include <string.h>` and `<stdio.h>` at the top of `spv_gl.c` for `memset`/`fprintf` (freestanding musl provides both).

- [ ] **Step 3: Add the fragment hook in `zraster.c`**

At the point identified in Step 1 where a fragment's final RGBA is about to be written to the destination buffer, add:

```c
/* NEOOS-SPV-HOOK: run the bound fragment program instead of the
 * fixed-function color/texture path when one is set. varyings[] here
 * must be whatever this rasterizer already interpolates per-pixel
 * (color and/or texcoord) -- confirm the exact local variable names
 * at the hook site in Step 1 and pass those, don't invent new state. */
if (spv_gl_has_fragment_program()) {
    float out_rgba[4];
    spv_gl_run_fragment(varyings, nvaryings, out_rgba);
    /* write out_rgba into the destination pixel using this
     * function's existing pixel-write path/format conversion --
     * reuse it, don't duplicate it. */
} else {
    /* existing fixed-function body, unchanged */
}
```

- [ ] **Step 4: Add the vertex hook in `ztriangle.c`**

At the point identified in Step 1 where a vertex's clip-space position is computed from the fixed-function transform, add the equivalent `if (spv_gl_has_vertex_program()) { ... } else { /* existing code */ }` structure, calling `spv_gl_run_vertex`.

- [ ] **Step 5: Wire `spv/*.c` into the Makefile**

```makefile
# neoos-tinygl/Makefile: extend TGL_SRCS and the include path
SPV_SRCS := $(wildcard spv/*.c)
TGL_SRCS := $(wildcard $(UPSTREAM_DIR)/src/*.c) $(SPV_SRCS)
TGL_CFLAGS += -Ispv
```

(This is the only Makefile change — `$(BUILD_DIR)/libTinyGL.a`'s existing rule already compiles every file in `$(TGL_SRCS)`, so adding `spv/*.c` to that variable is sufficient.)

- [ ] **Step 6: Build for the target and confirm no regression**

Run: `cd neoos-tinygl && make lib MUSL_DIR=../neoos-musl/build-output`
Expected: builds cleanly; `spv_gl.c`'s `fprintf`/`memset` resolve against musl's headers via the existing `-isystem $(MUSL_DIR)/include` flag.

- [ ] **Step 7: Commit**

```bash
git -C neoos-tinygl add spv/spv_gl.h spv/spv_gl.c upstream/src/ztriangle.c upstream/src/zraster.c Makefile
git -C neoos-tinygl commit -m "tinygl: hook the SPIR-V vertex/fragment stage into the rasterizer"
```

---

## Task 10: End-to-end target test — `spvtest.nex`

**Files:**
- Create: `neoos-tinygl/test/target/spvtest.c`
- Create: `neoos-tinygl/test/target/spvtest.test.json`
- Modify: `neoos-tinygl/Makefile` (build `spvtest.nex`)
- Modify: `NeoOS/Makefile` (new `spvtest` target, modeled on the existing `uxtest` target)

**Interfaces:**
- Consumes: `tglLoadSpvModule`, `tglUseSpvProgram` (public API from Tasks 1 and 9), TinyGL's existing `tglfb_open`/`glBegin`/`glEnd`/etc (already used by `glgears.c`).

- [ ] **Step 1: Generate the golden fragment-shader fixture used at runtime**

Reuse `fixtures/sample_texture.spvasm`'s shape (Task 8), but embed its `.spv` bytes directly into `spvtest.c` as a byte array, since the target binary can't read a host filesystem path at boot. Generate the array once, at development time:

```bash
cd neoos-tinygl
xxd -i test/host/fixtures/sample_texture.spv > test/target/sample_texture_spv.h
# produces: unsigned char test_host_fixtures_sample_texture_spv[] = {...}; unsigned int ..._len = N;
```

- [ ] **Step 2: Write `spvtest.c`, following `glgears.c`'s self-check pattern exactly**

```c
/* neoos-tinygl/test/target/spvtest.c -- self-checking SPIR-V shader
 * stage test, in the same style as glgears.c: render, read back,
 * assert, print ALL PASSED, exit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "GL/gl.h"
#include "zbuffer.h"
#include "tglfb.h"
#include "spv_module.h"
#include "spv_gl.h"
#include "sample_texture_spv.h"

int main(void) {
    if (tglfb_open() != 0) {
        printf("[spvtest] FAIL could not bring up /dev/fb0\n");
        return 1;
    }
    printf("[spvtest] start\n");

    TGLspvModule *fs = tglLoadSpvModule(test_host_fixtures_sample_texture_spv,
                                        test_host_fixtures_sample_texture_spv_len);
    if (!fs) {
        printf("[spvtest] FAIL golden fragment shader failed to load\n");
        return 1;
    }

    uint8_t tex[2 * 2 * 4] = {
        255,0,0,255,   0,255,0,255,
        0,0,255,255,   255,255,255,255,
    };
    spv_vm_bind_texture(tex, 2, 2);
    tglUseSpvProgram(NULL, fs);

    /* Draw one full-screen quad -- enough to exercise the fragment
     * hook across many pixels, matching how glgears exercises the
     * fixed-function path across many pixels. */
    tglfb_clear_screen(0x00101018);
    glBegin(GL_QUADS);
    glVertex3f(-1, -1, 0); glVertex3f(1, -1, 0);
    glVertex3f(1, 1, 0);   glVertex3f(-1, 1, 0);
    glEnd();
    tglfb_swap();

    struct tglfb_probe_result pr;
    tglfb_probe(&pr);
    printf("[spvtest] %lu distinct colours, %lu non-black\n", pr.distinct, pr.nonblack);

    if (pr.nonblack == 0) {
        printf("[spvtest] FAIL fragment shader produced an all-black frame\n");
        tglFreeSpvModule(fs);
        tglfb_close();
        return 1;
    }

    printf("[spvtest] ALL PASSED\n");
    tglFreeSpvModule(fs);
    tglfb_close();
    return 0;
}
```

```json
{"category":"bin"}
```
(save as `spvtest.test.json`, matching `glgears.test.json`'s exact format)

- [ ] **Step 3: Add the build rule to `neoos-tinygl/Makefile`**

```makefile
$(BUILD_DIR)/spvtest.nex: $(BUILD_DIR)/libTinyGL.a test/target/spvtest.c user.ld
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TGL_CFLAGS) -Itest/target -T user.ld -z noexecstack \
		-o $@ $(MUSL_DIR)/lib/crt1.o test/target/spvtest.c $(FB_SRCS) \
		$(BUILD_DIR)/libTinyGL.a \
		-L$(MUSL_DIR)/lib -lc -lgcc -lm
	cp test/target/spvtest.test.json $(BUILD_DIR)/spvtest.test.json

all: $(BUILD_DIR)/spvtest.nex
```

Run: `cd neoos-tinygl && make MUSL_DIR=../neoos-musl/build-output`
Expected: `build/spvtest.nex` produced alongside `build/glgears.nex`.

- [ ] **Step 4: Add the `spvtest` target to `NeoOS/Makefile`, modeled directly on the existing `uxtest` target**

```makefile
# ---- spvtest: boots neoos-tinygl's SPIR-V shader-stage self-test ----
TINYGL_DIR ?= ../neoos-tinygl

.PHONY: spvtest
spvtest: iso disk-image
	@[ -f $(TINYGL_DIR)/build/spvtest.nex ] || { echo "error: $(TINYGL_DIR)/build/spvtest.nex missing; build neoos-tinygl first" >&2; exit 1; }
	mcopy -o -i $(DISK_IMG) $(TINYGL_DIR)/build/spvtest.nex ::spvtest.nex
	@printf '%s\n' \
	  '# generated by `make spvtest` -- this test runs alone' \
	  'wait /spvtest.nex' \
	  > $(BUILD_DIR)/disk-src/INITTAB.solo
	mcopy -o -i $(DISK_IMG) $(BUILD_DIR)/disk-src/INITTAB.solo ::etc/inittab
	-@timeout $(BOOT_TIMEOUT) qemu-system-x86_64 $(QEMU_COMMON) \
		-display none -serial file:$(BUILD_DIR)/spvtest.log > /dev/null 2>&1
	@grep -E '^\[spvtest\]' $(BUILD_DIR)/spvtest.log || true
	@if grep -qE 'PANIC|\[exception\]' $(BUILD_DIR)/spvtest.log; then \
		echo "SPVTEST: the KERNEL did not survive"; \
		grep -E 'PANIC|\[exception\]' $(BUILD_DIR)/spvtest.log; exit 1; fi
	@if ! grep -q '\[spvtest\] ALL PASSED' $(BUILD_DIR)/spvtest.log; then \
		echo "SPVTEST FAILED: no ALL PASSED marker"; tail -12 $(BUILD_DIR)/spvtest.log; exit 1; fi
```

- [ ] **Step 5: Run and confirm it passes**

Run: `cd NeoOS && make spvtest TINYGL_DIR=../neoos-tinygl`
Expected: `[spvtest] ALL PASSED` printed, target exits 0.

- [ ] **Step 6: Commit both repos**

```bash
git -C neoos-tinygl add test/target/spvtest.c test/target/spvtest.test.json test/target/sample_texture_spv.h Makefile
git -C neoos-tinygl commit -m "spv: end-to-end target self-test for the shader stage"

git -C NeoOS add Makefile
git -C NeoOS commit -m "$(cat <<'EOF'
build: add spvtest -- boots neoos-tinygl's SPIR-V shader-stage self-test

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Load-time rejection and fixed-function regression, verified on target

**Files:**
- Modify: `neoos-tinygl/test/target/spvtest.c`

**Interfaces:**
- None new — this task only adds more assertions to the existing self-test.

- [ ] **Step 1: Embed the two rejection fixtures and extend `spvtest.c`**

```bash
cd neoos-tinygl
xxd -i test/host/fixtures/unsupported_opcode.spv > test/target/unsupported_opcode_spv.h
xxd -i test/host/fixtures/unbounded_loop.spv > test/target/unbounded_loop_spv.h
```

```c
/* Add to spvtest.c, after the golden-shader check and before "ALL PASSED": */
#include "unsupported_opcode_spv.h"
#include "unbounded_loop_spv.h"

    TGLspvModule *bad1 = tglLoadSpvModule(test_host_fixtures_unsupported_opcode_spv,
                                          test_host_fixtures_unsupported_opcode_spv_len);
    if (bad1 != NULL) {
        printf("[spvtest] FAIL unsupported-opcode module was accepted\n");
        tglFreeSpvModule(bad1);
        tglfb_close();
        return 1;
    }
    printf("[spvtest] unsupported opcode correctly rejected\n");

    TGLspvModule *bad2 = tglLoadSpvModule(test_host_fixtures_unbounded_loop_spv,
                                          test_host_fixtures_unbounded_loop_spv_len);
    if (bad2 != NULL) {
        printf("[spvtest] FAIL unbounded-loop module was accepted\n");
        tglFreeSpvModule(bad2);
        tglfb_close();
        return 1;
    }
    printf("[spvtest] unbounded loop correctly rejected\n");
```

- [ ] **Step 2: Add a fixed-function regression check — draw the same quad with no program bound, before the shader checks, and confirm distinct output**

```c
/* Add near the top of main(), before tglUseSpvProgram is ever called: */
    tglfb_clear_screen(0x00101018);
    glBegin(GL_QUADS);
    glVertex3f(-0.5f, -0.5f, 0); glVertex3f(0.5f, -0.5f, 0);
    glVertex3f(0.5f, 0.5f, 0);   glVertex3f(-0.5f, -0.5f, 0);
    glEnd();
    tglfb_swap();
    {
        struct tglfb_probe_result pr0;
        tglfb_probe(&pr0);
        if (pr0.nonblack == 0) {
            printf("[spvtest] FAIL fixed-function path (no program bound) produced nothing\n");
            tglfb_close();
            return 1;
        }
        printf("[spvtest] fixed-function path unaffected by the shader stage\n");
    }
```

- [ ] **Step 3: Rebuild and run `make spvtest` again**

Run: `cd neoos-tinygl && make MUSL_DIR=../neoos-musl/build-output && cd ../NeoOS && make spvtest TINYGL_DIR=../neoos-tinygl`
Expected: all four `[spvtest]` lines print in order, ending in `ALL PASSED`, exit 0.

- [ ] **Step 4: Commit**

```bash
git -C neoos-tinygl add test/target/spvtest.c test/target/unsupported_opcode_spv.h test/target/unbounded_loop_spv.h
git -C neoos-tinygl commit -m "spv: verify load-time rejection and fixed-function regression on target"
```

---

## Summary

Eleven tasks, in dependency order: module-header parsing → type/constant tables → entry-point acceptance → function-body decode with opcode whitelisting → bounded-loop validation → straight-line VM execution → control-flow VM execution → texture sampling → TinyGL rasterizer hooks → end-to-end target self-test → rejection/regression coverage on target. Tasks 1-8 iterate fast on the host (`gcc`, no QEMU); Tasks 9-11 verify the real integration the only way NeoOS verifies anything — a headless boot and a serial log.

This closes out sub-project 1 of the four-part Liquid-Glass roadmap from the design spec. Sub-project 2 (compositor-level lensing in `neoos-wm`) gets its own spec once this lands and is confirmed working end to end.
