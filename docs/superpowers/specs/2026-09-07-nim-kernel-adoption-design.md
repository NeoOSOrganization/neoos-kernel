# Nim as the NeoOS Kernel Language — Adoption Design

## Goal

Make **Nim** the primary implementation language for the NeoOS kernel.
A small, well-defined **C + assembly core** stays C — the boot path,
the language-runtime bring-up, and the handful of routines that are
inherently asm-adjacent. Everything above that line migrates to Nim,
module by module, leaf-first, with a **mixed C/Nim build as the steady
state for a long time**. The stated long-term direction is a
Nim-only kernel; this spec does not commit to eliminating C, it
commits to shrinking it to a deliberate, documented minimum.

Nim compiles to C, so the generated code still goes through
`x86_64-elf-gcc` with the exact kernel flags NeoOS uses today
(`-ffreestanding -mcmodel=kernel -mno-sse -mno-red-zone -nostdlib`).
Nothing about the final artefact changes: one `kernel.elf`, one
linker script, the same GRUB multiboot2 entry.

### Why

- **Expressiveness where it pays**: the data-structure-heavy
  subsystems (the scheduler's EEVDF trees and sched-domain topology,
  the VFS, the network stack's state machines, the syscall
  demux) get real generics, sum types (`object` variants with
  exhaustive `case`), compile-time evaluation (`const` tables,
  `static:` blocks generating lookup data), `distinct` types
  (virtual-time vs real-time, physical vs virtual addresses as
  distinct integers the compiler won't let you mix), and
  `{.raises: [].}` effect tracking (a function provably cannot
  raise — important in interrupt context).
- **Safety without a runtime tax**: bounds/nil/overflow checks are
  opt-in per scope (`{.push checks: on.}` in a driver init path,
  `{.push checks: off.}` in the context-switch neighbourhood), not
  all-or-nothing. `--mm:none` means no GC, no hidden allocation.
- **It stays C underneath**: the toolchain, the debugging workflow
  (with the mitigations in "Debuggability" below), and every
  existing C module keep working unchanged during the multi-year
  migration.
- **Prior art**: freestanding Nim kernels are a trodden path —
  Khaled Hammouda's *Fusion OS* / "Writing an OS in Nim" series
  (`--os:any`, a custom allocator, `--exceptions:quirky`, UEFI),
  `dom96/nimkernel`. NeoOS is not inventing the integration, only
  applying it.

### Non-goals

- Not a rewrite. No module is rewritten "for cleanliness"; a module
  moves to Nim only when it is being changed substantially anyway, or
  as a scheduled migration milestone with its selftest as the gate.
- Not the scheduler first. SCH-1 (EEVDF) has a C plan and lands in C;
  `kernel/lib/rbtree` is C and stays the interop boundary. The
  scheduler is a *late* migration candidate, if ever — see the
  advanced-scheduler spec's own note.
- No new userland story. Nim-on-NeoOS *userland* already works via
  the musl path (`scratchpad/nim-probe`); that is unrelated and
  unchanged.

---

## The permanent C + assembly core

The line below which everything is C, and above which everything can
be Nim. Kept as small as the runtime allows; each entry has a reason
it cannot practically be Nim.

| stays C/asm | why |
|---|---|
| `boot/boot.asm` | 32→64-bit trampoline, early page tables, the jump to long mode. Pure asm; runs before any C or Nim runtime exists. |
| `kernel/arch/*.asm` (`gdt_flush`, `isr_stubs`, `context_switch`, `syscall_entry`, `fork_trampoline`, `sigframe`, `ap_trampoline`) | Hand-written asm — register save/restore, `iretq`/`sysretq` frames, the stack switch itself. Nim `{.emit.}` could hold these but there is no benefit and real risk. |
| `kernel/kernel.c`'s **first N lines** — a thin `kmain` that: sets up the earliest serial output, calls the Nim runtime init (`NimMain` or a custom `nim_kernel_init`), then hands off to `nim_kmain()`. | The Nim runtime must be initialised from C-linkage code that itself needs no runtime. Shrinks to ~30 lines. |
| `kernel/lib/panicoverride` (Nim-visible but C-implemented, or a tiny Nim module with `{.emit.}`) | Nim's `--os:standalone` requires the program to provide `rawoutput`, `panic`, `sysFatal`. These route to `serial_write_string` + the existing `lock_panic` machinery. |
| `kernel/lib/nim_rt_shims.c` | The freestanding-runtime glue: `memcpy`/`memset`/`memmove`/`memcmp` (Nim's codegen emits calls to these; NeoOS already has them), `__stack_chk_fail` (already `-fno-stack-protector`, but Nim may still reference), any `nimbase.h` symbol the kernel flags leave undefined. |
| `kernel/lib/rbtree.{c,h}` | Already landed, proven, and the natural C↔Nim data-structure boundary. Nim code embeds `RbNode` (`{.importc: "struct rb_node", header: "lib/rbtree.h".}`) and calls `rbInsertColor` etc. Migrating it to Nim generics is a *possible* later NIM-n milestone, not a goal. |
| `kernel/mm/pmm.c` + the lowest layer of `paging.c` — the physical frame allocator and the raw page-table walk/map | The Nim allocator (`--mm:none`'s `alloc`, later `--mm:arc`'s `allocShared`) is *bound to* `kmalloc`, which is bound to `pmm`. Circular-dependency floor: something must allocate physical memory with zero allocation of its own. Could be Nim with `{.checks: off, raises: [].}` eventually; C is the safe floor. |

**Everything else is fair game for Nim**: `kernel/drivers/*`,
`kernel/fs/*`, `kernel/net/*`, `kernel/tty/*`, `kernel/ipc/*`,
`kernel/sync/*` (above the raw spinlock asm), `kernel/syscall/*`,
`kernel/mm/*` above pmm (heap, vma, uaccess), `kernel/sched/*` (late),
`kernel/smp/*` above the IPI asm, `kernel/lib/*`.

---

## Nim kernel configuration

A `kernel/config.nims` (or `nim.cfg`) checked in, with a matching
build-time invocation. Settled once in NIM-1, then stable.

```nim
# kernel/config.nims  (applies to every module compiled for the kernel)
switch("os", "any")            # no OS assumptions; we provide the runtime
switch("mm", "none")           # phase 1: NO allocation. Phase 2: "arc".
switch("threads", "off")       # the kernel is its own scheduler
switch("panics", "on")         # defect -> panic (our panicoverride), not exception
switch("exceptions", "goto")   # phase 1. Consider "quirky" if goto codegen bloats.
switch("cc", "gcc")
switch("gcc.exe", "x86_64-elf-gcc")
switch("gcc.linkerexe", "x86_64-elf-gcc")
switch("passC", "-ffreestanding -fno-stack-protector -mno-red-zone " &
                "-mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -std=gnu11 -O2 " &
                "-Ikernel -Ishared -I" & nimcacheDir())
switch("stackTrace", "off")
switch("lineTrace", "off")
switch("lineDir", "on")        # keep #line back to the .nim source for gdb
switch("debugger", "native")   # emit enough for addr2line on the .nim
switch("define", "release")    # not "danger" -- keep the checks we opt into meaningful
switch("noMain", "on")         # we call the entry ourselves
switch("nimMainPrefix", "nim_kernel_")
--define:useMalloc             # phase 1: route the (unused) alloc to our stub;
                               # phase 2: our stub IS kmalloc
```

Deliberate settings, with the reasoning:

- **`--os:any`** not `standalone`: `standalone` is stricter and older;
  `any` + `--mm:none` gives a clean "no libc, no OS" target and is
  what Fusion OS uses.
- **`--mm:none` first, `--mm:arc` later**: phase 1 forbids `seq`,
  `string`, `ref`, `new`, closures-that-capture, `@[]` — you write
  `ptr T`, `array[N, T]`, `UncheckedArray[T]`, `openArray[T]`, and
  manual `alloc`/`dealloc`. This matches how the C kernel already
  works and keeps the first milestones honest. Phase 2 turns on
  `--mm:arc` (deterministic, no stop-the-world, no cycle collector)
  with `allocShared0`/`deallocShared` bound to the kernel heap,
  unlocking `seq`/`string`/`ref` in **non-hot-path** code
  (driver config parsing, fs path handling, `/proc` string building).
  `--mm:orc` (adds a cycle collector) is very likely **never**
  in-kernel.
- **`--panics:on` + `--exceptions:goto`**: an unhandled `Defect`
  (index error, nil deref, overflow) becomes a call to our `panic`,
  which routes to `lock_panic` / `exception_dump_and_halt`. `goto`
  exceptions are `setjmp`-free (a hidden error-code return + gotos);
  if the codegen bloat or the `NIM_RAISES` plumbing is a problem,
  fall back to `--exceptions:quirky` (no unwinding at all — a raise
  is UB, so every kernel proc is `{.raises: [].}` and the compiler
  enforces it). Quirky is the more honest kernel choice long-term.
- **`{.raises: [].}` everywhere reachable from interrupt context** —
  enforced by a `push`d pragma in the relevant modules.

### The freestanding runtime shims

`kernel/lib/panicoverride.nim` (Nim, tiny, `{.emit.}` or `importc`
into the C serial/panic paths):

```nim
proc serialWriteString(s: cstring) {.importc: "serial_write_string",
                                     header: "drivers/char/serial.h".}
proc lockPanic(msg, where, holding: cstring) {.importc: "lock_panic",
                                     header: "sync/lock.h", noreturn.}

{.push stackTrace: off, profiler: off.}
proc rawoutput(s: string) =
  serialWriteString(s.cstring)          # phase 1: s is a fixed cstring-backed value
proc panic(s: string) {.noreturn.} =
  serialWriteString("[nim] panic: ")
  serialWriteString(s.cstring)
  serialWriteString("\n")
  lockPanic("nim defect".cstring, "nim/panic".cstring, nil)
  while true: discard
{.pop.}
```

`kernel/lib/nim_rt_shims.c` — provide whatever `nimbase.h` /
codegen references that `-nostdlib` leaves undefined. Expected list
(confirm empirically in NIM-0): `memcpy/memset/memmove/memcmp`
(exist), `strlen` (exists), possibly `__mulodi4`/`__udivti3` (libgcc
covers), `nimGCvisit`/`nimRegisterGlobalMarker` (only with a GC —
none), `nimFrame`/`popFrame` (only with `--stackTrace:on` — off).
Kernel is `-mno-sse`: **never** let Nim float code in — `-d:release`
+ no `float`/`echo`/`$` of floats. If a module needs a float, it's
wrong for the kernel.

### Entry / init

`kmain` (still C, ~30 lines) after the earliest serial setup:

```c
extern void nim_kernel_PreMain(void);   // Nim: init const globals (no alloc)
extern void nim_kernel_NimMain(void);   // Nim: run module-level code
extern void nim_kmain(void *mb_info);   // Nim: the real kmain

void kmain(void *multiboot_info) {
    serial_init();
    nim_kernel_PreMain();
    nim_kernel_NimMain();               // must not allocate under --mm:none
    nim_kmain(multiboot_info);          // never returns
}
```

`kernel/main.nim` becomes the real kernel entry, and the current
`kmain` body (the long selftest + init sequence) migrates into it
incrementally — early on it just calls the remaining C init functions
via `importc`.

---

## Build integration

`Makefile` changes (NIM-1):

```make
NIM     ?= nim
NIMFLAGS := --nimcache:$(BUILD_DIR)/nimcache --compileOnly --noLinking \
            --header:nimkernel.h $(if $(V),--verbosity:2,--hints:off)

# One `nim c` compiles the whole Nim module graph rooted at main.nim and
# EMITS C into build/nimcache/ -- it does not link. The emitted .c files
# are then compiled by the SAME rule as hand-written kernel .c.
$(BUILD_DIR)/nimcache/.stamp: $(shell find kernel -name '*.nim') kernel/config.nims
	$(NIM) c $(NIMFLAGS) kernel/main.nim
	touch $@

NIM_C_SOURCES := $(wildcard $(BUILD_DIR)/nimcache/*.c)   # after the stamp exists
NIM_OBJECTS   := $(patsubst %.c,%.o,$(NIM_C_SOURCES))

$(BUILD_DIR)/nimcache/%.o: $(BUILD_DIR)/nimcache/%.c $(BUILD_DIR)/nimcache/.stamp
	$(CC) $(CFLAGS) -I$(BUILD_DIR)/nimcache -c $< -o $@

$(BUILD_DIR)/kernel.elf: $(ASM_OBJECTS) $(C_OBJECTS) $(NIM_OBJECTS) ...
```

(The `.stamp` + a second `make` pass, or a recursive `$(MAKE)`, is the
standard "generated sources" pattern; refine in NIM-1 — `nim
--genscript` emits a compile list that can be `include`d.)

- **Nim as a build dependency**: pin an exact version
  (Nim 2.2.x — matches the userland probe). Options, decided in
  NIM-1: (a) vendor the Nim compiler source + a bootstrap step in the
  repo, matching how `x86_64-elf-gcc` is "installed once and reused";
  (b) document a `choosenim`/manual install and check the version in
  the Makefile (`nim --version | grep 2.2` or fail). (a) is more in
  keeping with NeoOS's "reproducible toolchain" stance.
- **`neoos-os-builder` sync** (`CLAUDE.md`): the *kernel* build is
  not what os-builder assembles (it clones `neoos-kernel` +
  `neoos-musl` + ports and runs their Makefiles) — but
  `neoos-kernel`'s own build now needs Nim, so os-builder's kernel
  build step and `docs/BUILD_ORDER.md` must state the Nim
  dependency.
- **Gauntlet / CI**: `tools/gauntlet.sh` runs `make` — as long as
  Nim is on `PATH` in the build env, no gauntlet change. The
  `[nim] runtime selftest passed` marker (NIM-1) joins
  `CORE_REQUIRED_MARKERS`.
- **Compile time**: `nim c` of the whole kernel graph is one process;
  incremental is via the nimcache. Expect the Nim compile to add a
  few seconds initially; `make clean-kernel` blows the nimcache too.

---

## Interop patterns (the contract)

### Nim calls existing C

```nim
type Thread {.importc: "struct thread", header: "sched/proc.h",
              incompleteStruct.} = object
proc currentThread(): ptr Thread {.importc: "current_thread",
                                   header: "sched/proc.h".}
proc kmalloc(n: csize_t): pointer {.importc, header: "mm/heap.h".}
```

`incompleteStruct` = "I only pass pointers, never `sizeof` it" — the
default and the safe one for big C structs. When Nim must know the
layout (to embed a field, or `sizeof`), re-declare the fields
explicitly and add a `_Static_assert(sizeof + offsetof)` on the C
side (the pattern `cpu_local.h` already uses for asm offsets).

### C calls Nim

```nim
proc schedTick*(rq: ptr Rq) {.exportc: "sched_tick", cdecl.} = ...
```

`{.exportc.}` fixes the symbol name; `{.cdecl.}` fixes the ABI. A
migrated module keeps the **exact** C symbol names its callers use, so
callers don't change. The module's `.h` (hand-written, or
`--header` generated) declares them for the C side.

### Structs shared both ways

The `rb_node`-embedded-in-`sched_entity` pattern: define the Nim
`object` with `{.exportc, packed.}` and a `_Static_assert` generated
alongside (via `{.emit.}` at module scope) that its size/offsets match
what any remaining C expects. Or: keep the shared struct in a C
header and `importc` it into Nim. **Prefer the latter** during
migration — one source of truth, in C, until the last C caller is
gone.

### Atomics

`std/atomics` works freestanding (`Atomic[T]`, `load`/`store`/
`fetchAdd` with `moRelaxed`/`moAcquire`/`moRelease`/`moSeqCst`). Maps
to the same `__atomic_*` builtins the C uses. For the few places C
and Nim touch the *same* atomic word (`thread.on_cpu`,
`thread.state`), keep it a C field and `importc` it, accessed from
Nim via `atomicLoadN`/`atomicStoreN` `{.importc.}` wrappers, so both
languages hit identical codegen.

### `volatile`

`var x {.volatile.}: T` — the pragma exists. MMIO register access
(LAPIC, IOAPIC, framebuffer) uses it; matches the C `volatile`.

### Inline asm

`{.emit: """ __asm__ volatile ("..." ::: "memory"); """.}` — for
`cli`/`sti`/`hlt`/`rdmsr`/`wrmsr`/`invlpg`/`rdtsc`. Or wrap them once
in a C `arch/asm.h` and `importc` — cleaner, and most already exist.

### Function pointers (the `sched_class` vtable, `file_ops`, `rb_augment_callbacks`)

Nim `proc` types with `{.cdecl.}` are C function pointers. A vtable is
a Nim `object` of `proc {.cdecl.}` fields, `{.exportc.}`'d, matching
the C struct. This is exactly `file_ops` / `sched_class` /
`rb_augment_callbacks` — they interop cleanly.

---

## Debuggability

The one real regression, and its mitigations:

- **Name mangling**: `fairPick` becomes `fairPick__fair_u42` in the
  ELF symtab. Mitigation: `nim` emits a `*.json` per module in the
  nimcache with the mangling map; add `tools/nim-demangle.py` that
  post-processes `nm`/`addr2line`/`objdump` output. Or use
  `{.exportc.}` on every proc that could appear in a backtrace
  (verbose but explicit).
- **`#line` directives**: `--lineDir:on` makes the generated C carry
  `#line N "kernel/fair.nim"`, so `addr2line` and gdb-over-QEMU-gdbstub
  point at the **`.nim`** source, not the generated C. Keep this on
  for the kernel always.
- **The generated C is readable-ish**: Nim's C output is
  human-followable (not like a template-heavy C++ blob). When a fault
  RIP resolves into a Nim proc, reading the one generated `.c`
  function is a viable fallback, as it was for the .NET NativeAOT
  investigation this session.
- **Selftest-first culture unchanged**: every migrated module keeps
  its `[x] selftest passed` marker; a regression is a failed marker
  or a hang, caught by the gauntlet exactly as today.

---

## Memory model transition

**Phase 1 (`--mm:none`)** — NIM-0 through the first several migrations.
No `new`, `seq`, `string`, `ref`, capturing closures, `@[]`,
`newSeq`. The vocabulary: `ptr T`, `array[N, T]`,
`UncheckedArray[T]`, `openArray[T]` (a `(ptr, len)` pair — pass
buffers this way), `var T` / `sink T` / `lent T` for move/borrow
semantics without a runtime. `alloc`/`dealloc` are `{.error.}`-shadowed
so an accidental heap use is a compile error, except in the two or
three modules that genuinely manage memory (heap, vma).

**Phase 2 (`--mm:arc`)** — once `kernel/mm/heap` is stable enough to
back it. `allocShared0`/`deallocShared` (the "shared heap" hooks Nim
uses when `--threads:off` but you still want a global heap) bound to
`kmalloc0`/`kfree`. Unlocks `seq[T]`, `string`, `ref object`, and
non-capturing-or-heap-closures in **cold paths only** — a
`{.push checks: on, raises: [].}` block in a driver's `init`, a
`/proc` file's content builder, a config parser. Hot paths
(`schedule`, `timer_handler`, the recv/xmit fast path, `copy_to_user`)
stay `--mm:none`-discipline even under `--mm:arc`: `{.push
mm: none.}` is not a thing, but a lint (`{.raises: [], tags: [].}` +
a grep for `newSeq`/`@[`/`new(` in the hot modules, enforced in the
selftest or a `tools/` check) keeps them clean.

ARC's destructors (`=destroy`, `=copy`, `=sink`, `=wasMoved`) become
usable for RAII-style resource management — a `File` object that
closes on scope exit, a `Locked[T]` that releases — which is a real
ergonomic win for the fs/net layers.

---

## Migration order

Leaf-first, each gated by its selftest + the gauntlet. The steady
state is a **mixed build** — plan for it to last years, not months.

1. **NIM-0 — Spike**: one trivial `--mm:none` Nim module compiled
   into the kernel, `{.exportc.}`ing one function that C calls, and a
   `[nim] hello selftest passed` marker. Reimplement `rand_selftest`
   *or* a leaf helper (e.g. a checksum, a hex formatter) in Nim.
   Proves: `nim c` in the build, the runtime shims, panicoverride,
   the C↔Nim call both ways, `--mm:none` reality, gdb `#line` back to
   `.nim`, gauntlet still green. **If this is painful, stop and
   reassess.**
2. **NIM-1 — Runtime + build solidified**: `config.nims` finalised,
   Nim version pinned/vendored, the Makefile generated-sources
   pattern clean, `panicoverride` routing verified (deliberately
   trip a Nim `Defect`, confirm it hits `lock_panic` with a useful
   message), the demangle tooling. `docs/` written up. `[nim]
   runtime selftest passed` in `CORE_REQUIRED_MARKERS`.
3. **NIM-2 — First real subsystem: a driver**. `kernel/drivers/char/
   rtc.c` or `serial.c` or the `pit`/`timer` (small, self-contained,
   has a selftest, MMIO + a little logic). Rewrite in Nim keeping the
   C symbol names. Prove `volatile` MMIO, the interop with the IRQ
   layer, that a real module can be Nim.
4. **NIM-3 — `kernel/lib/*`** (`rand`, and consider `rbtree` as
   generics — but only if it doesn't disturb the SCH-1 work).
5. **NIM-4 — `kernel/sync/*`** above the spinlock asm: `waitq`,
   `poll_head`, `epoll`, `inotify`. State machines + lists — good
   Nim fit.
6. **NIM-5 — `kernel/tty/*`, `kernel/ipc/*`**.
7. **NIM-6 — `kernel/fs/*`**: VFS, ramfs, procfs, fatfs, blkcache.
   Large; probably its own multi-milestone spec. Phase-2 `--mm:arc`
   likely lands here (path strings, dirent buffers).
8. **NIM-7 — `kernel/net/*`**: the state machines (`tcp.c` especially)
   are the strongest Nim case in the whole kernel.
9. **NIM-8 — `kernel/syscall/*`**: the demux + the thin wrappers.
   `sched_class`-style dispatch tables in Nim.
10. **NIM-9 — `kernel/mm/*` above pmm**: heap, vma, uaccess. Careful —
    this is where `--mm:arc`'s backing allocator lives; bootstrapping
    order matters.
11. **NIM-10+ — `kernel/sched/*`, `kernel/smp/*`**: last, and only if
    NIM-2..9 proved the model under load. The context-switch
    neighbourhood may stay C permanently — that's fine and expected.
12. **NIM-∞ — Define the frozen C core**: once the migration front
    reaches the "permanent C core" table above, write the final
    statement of what is C and why, and whether "completely switch to
    Nim" means rewriting even that (boot asm can't be; `kmain`'s 30
    lines could).

Each migration milestone's Definition of Done:
- module rewritten in Nim, same C-visible symbols
- its existing selftest marker still fires
- `tools/gauntlet.sh 15 3` at zero retries
- the C file deleted, `KERNEL_DIRS`/nimcache wiring updated
- `docs/abi-compatibility.md` untouched (internal change) unless a
  syscall's observable behaviour shifted (it must not)

---

## Risks

- **Nim compiler as a build dependency**: version drift breaks the
  build. Mitigation: pin exactly, vendor if feasible, check in the
  Makefile. Nim 2.2's codegen for `--mm:none`/`--os:any` is stable
  and used by Fusion OS; not bleeding edge.
- **Generated-C debuggability**: covered above; `--lineDir:on` +
  a demangle script + `{.exportc.}` on backtrace-relevant procs.
  Accept a modest regression here.
- **`-mno-sse` + Nim floats**: Nim's `system` has float helpers
  (`$` of float, `nimFloatToStr`). If any reach the kernel codegen
  the SSE-less build breaks. Mitigation: no `float` in kernel Nim
  (a `{.error.}` shadow on `float`/`float64` in the kernel prelude),
  and a `tools/` grep in the selftest.
- **Exception codegen**: `--exceptions:goto` can bloat and can make
  `{.raises: [].}` proofs noisy. Fallback `--exceptions:quirky`
  decided empirically in NIM-1.
- **`--mm:arc` bootstrapping**: ARC needs a heap; the heap is being
  migrated in NIM-9. Order carefully — `--mm:arc` may not turn on
  until NIM-9, and everything before it lives under `--mm:none`.
- **Two-language cognitive load / contributor pool**: a Nim+C+asm
  kernel is a smaller audience than a C+asm one. Accepted trade-off;
  the mixed-build steady state means a C contributor can still work
  on C modules.
- **Stack usage**: Nim value types + `--mm:none` can be
  stack-hungry; the kernel stack is 16 KiB. `--panics:on` catches an
  overflow as a guard-page fault, not silent corruption, but budget
  it: `{.byaddr.}`, `sink`/`lent`, and `ptr` for anything large.
- **`static:` / compile-time in the kernel**: fine — it runs on the
  *host* Nim VM at compile time and emits data. This is a *strength*
  (generate `sched_prio_to_weight` from a formula, generate the
  syscall table from a declarative list) with no runtime cost.

---

## Testing

- Every phase and every migrated module: its `[x] selftest passed`
  marker, plus `tools/gauntlet.sh 15 3` at zero retries. No new test
  *infrastructure* — the boot-selftest + gauntlet model is
  language-agnostic.
- NIM-0/NIM-1 additionally: a **negative** test — a Nim module that
  deliberately does `let a = [1,2,3]; discard a[5]` under
  `{.push checks: on.}`, confirming it lands in `panic` → `lock_panic`
  with a `[nim] panic:` line and the machine halts cleanly (not a
  silent wrong answer).
- A `tools/nim-hotpath-lint.sh` (grep-based) that fails if
  `newSeq`/`@[`/`new(`/`alloc(` appears in the modules tagged
  hot-path, run from the gauntlet build.
- Boot-time delta tracked per migration (Nim codegen at `-O2` should
  be within noise of the C it replaced; a regression means a check
  left on in a hot path or an exception-plumbing cost).

## First concrete step

Write `docs/superpowers/plans/2026-09-07-nim-kernel-nim0-spike.md` —
the NIM-0 plan — and execute it. Everything else in this spec is
sequenced behind a clean NIM-0.
