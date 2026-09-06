# A Hosted `x86_64-neoos-linux-musl` GCC/G++ Toolchain — Design

## Goal

Build a genuinely **hosted** cross-toolchain targeting NeoOS — real
libc (NeoOS's own patched musl), real CRT startup, real C++ exception
unwinding (`crtbeginT.o`, `libgcc_eh.a`) — as a persistent, reusable
piece of host infrastructure, the same way `x86_64-elf-gcc`
(`~/opt/cross-x86_64-elf`) already is.

This exists to unblock a retry of statically linking a dotnet
NativeAOT-produced binary against NeoOS (the immediate trigger: that
link failed needing `crtbeginT.o`/`libgcc_eh.a`, which the existing
`x86_64-elf-gcc` cannot provide — it was deliberately built freestanding,
bare-metal, no libc, no hosted C++ runtime). It is also a generically
useful asset beyond that one use case: any future NeoOS port needing
real C++ (exceptions, RTTI) gets a real toolchain instead of working
around the freestanding one's limits case by case.

**This spec does not touch NativeAOT again.** Its own proof of done is
independent and simpler: a trivial C++ program using a real
`throw`/`catch`, cross-compiled with the new toolchain, booting
correctly on NeoOS. Retrying NativeAOT is a follow-on, once this
toolchain exists.

## Why hosted, and why NeoOS-specific (not generic musl)

The existing `x86_64-elf-gcc` targets bare metal: no libc assumption,
`-ffreestanding` everywhere, every userland program in this org
supplies its own `crt1.o`/`user.ld`/`-nostdlib` by hand. That is right
for a kernel and for the C-based ports that already work this way, but
it structurally cannot provide `libgcc_eh.a`/`crtbeginT.o` — those are
hosted-C++-runtime artifacts that only exist when GCC is built
*against* a real libc, because libgcc's exception-handling runtime and
the CRT constructor-registration objects are built as part of GCC's
own hosted configure/build, not something `-nostdlib` freestanding
mode ever produces.

A **generic** `x86_64-linux-musl` toolchain (stock upstream musl, per
musl-cross-make's own default) would supply those artifacts too, and
they are largely libc-agnostic (mostly `.init_array`/`.fini_array`
plumbing and unwind-table mechanics) — but per your explicit choice,
this build goes further: it builds musl-cross-make's own musl stage
**from `neoos-musl` itself** (shimmed, syscall-translated), so the
resulting toolchain's bundled libc, and everything statically linked
against it, is genuinely NeoOS's own patched musl end to end — one
coherent, self-consistent toolchain per target, matching how
`x86_64-elf-gcc` is already NeoOS's own rather than a generic
bare-metal ELF compiler borrowed as-is.

## Architecture

### Target triple: `x86_64-neoos-linux-musl`

A new GCC target, not a variant of `x86_64-elf`. GCC/binutils bake
real per-triple assumptions (PIC defaults, spec files, multilib
layout) into the build; reusing `x86_64-elf`'s triple for a
fundamentally different (hosted, real-libc) configuration would be
building an inconsistent toolchain under a name that already means
something else. `musl-cross-make`'s architecture detection reads only
the first hyphen-separated component (`x86_64`), so a novel middle/tail
(`neoos-musl` in place of `linux-musl`) is exactly the kind of naming
its tooling already expects to vary.

### Build tool: `musl-cross-make`, with `neoos-musl` substituted for stock musl

`musl-cross-make`'s own three real stages (binutils; stage-1
freestanding GCC; the target libc; stage-2 full GCC/G++) are used
unmodified except for ONE substitution point: `config.mak` pins
`MUSL_VER = 1.2.5` (matching `neoos-musl`'s own pinned
`v1.2.5` — verified: `neoos-musl/upstream` is checked out at exactly
that tag) and `MUSL_REPO` at musl's real upstream (unchanged; the
shim is not a fork of musl's git history, it is a file-copy patch
applied post-checkout — see `neoos-musl/third_party/shim/apply.sh`'s
own comment: "the submodule is meant to stay pristine... third_party/
shim/ holds the real sources").

The shim itself is applied via `musl-cross-make`'s own `patches/`
mechanism: real unified diffs (`diff -u <stock 1.2.5 file> <shimmed
file>`), one per shimmed file (`syscall_arch.h`, `syscall_cp.s`,
`__set_thread_area.s`, `__unmapself.s`, `clone.s`, `restore.s`,
`vfork.s`), generated once from `NeoOS/third_party/shim/*` against a
clean musl `v1.2.5` checkout, committed to
`neoos-hosted-gcc/patches/musl-1.2.5/*.diff`. `neoos_syscall.c` (a
wholly new file, not a patch to an existing one) is copied in via a
small `Makefile` hook rather than a diff, matching how `apply.sh`
itself already treats it as "a new file, so no `.orig` to keep."

This means the shim's *source of truth stays exactly where it already
is* (`NeoOS/third_party/shim/`) — the generated diffs are a build-time
artifact of that source, regenerated whenever the shim changes, not a
second independent copy to keep in sync by hand.

### Repo and install layout

- **`neoos-hosted-gcc`** (new repo, `NeoOSOrganization/neoos-hosted-gcc`,
  cloned to `/home/neo/projects/personal/neoos-hosted-gcc`): holds
  `config.mak`, the generated `patches/musl-1.2.5/*.diff`, a
  `regen-patches.sh` script (re-diffs `third_party/shim/` against a
  clean checkout whenever the shim changes — the plan's own
  reproducibility story), and a `build.sh` wrapper matching every
  other port's convention.
- **The compiled toolchain**: installed to `~/opt/cross-x86_64-neoos`,
  alongside (not replacing) `~/opt/cross-x86_64-elf`. Not committed to
  any repo — host-local infrastructure, exactly like the existing
  freestanding cross-compiler.

### Cost, stated plainly

A full binutils+GCC (C and C++) bootstrap from source is a genuinely
long build — on the order of 30-90+ minutes — run as a background
task spanning several plan steps, not a quick `make` like the
C-based ports' own builds. This spec's plan accounts for that
directly: bootstrap steps are structured to run in the background
with clear, separately-verifiable checkpoints (binutils done; stage-1
GCC done; musl done; stage-2 GCC done) rather than one giant opaque
step.

## Testing plan

1. **Toolchain smoke test**: `x86_64-neoos-linux-musl-gcc --version` /
   `x86_64-neoos-linux-musl-g++ --version` succeed; `x86_64-neoos-linux-musl-gcc
   -print-file-name=crtbeginT.o` and `-print-file-name=libgcc_eh.a`
   resolve to real files (not literally the string back, which is
   GCC's way of saying "not found").
2. **A real C (not C++) hosted-hello-world**, linked the NORMAL hosted
   way this toolchain expects (`x86_64-neoos-linux-musl-gcc -static hello.c
   -o hello`, no manual `-nostdlib`/explicit `crt1.o` juggling — this
   toolchain's whole point is not needing that dance), booted on
   NeoOS via the existing QEMU+serial-log convention. Proves the
   bundled `neoos-musl` sysroot is wired correctly before touching
   C++ at all.
3. **A real C++ program using `throw`/`catch`** (e.g., a function that
   throws a `std::runtime_error` on one input and returns normally on
   another, with a caller that catches it and prints which path ran),
   linked with `x86_64-neoos-linux-musl-g++ -static`, booted the same way.
   This is the milestone's actual proof of done: real stack unwinding
   through real exception tables, working against `neoos-musl`.
4. Full `tools/gauntlet.sh 15 3` regression, zero retries — this
   milestone adds a new host toolchain and a new repo; it changes no
   kernel code and no shared `neoos-musl` shim code (the diffs it
   generates are read-only reflections of the existing shim, not
   edits to it), so a regression would be surprising and worth
   root-causing.

## Self-review

- **Placeholders**: none — target triple, exact musl version
  (`1.2.5`, verified against the real pinned submodule commit), the
  `MUSL_REPO`/`MUSL_VER`/`patches/` substitution mechanism (verified
  against musl-cross-make's real `Makefile`), and the install paths
  are all concrete, not left for the plan to invent.
- **Internal consistency**: "why hosted, and why NeoOS-specific"
  explicitly argues FOR the more expensive choice (rebuilding musl
  from `neoos-musl` rather than reusing generic libc-agnostic
  artifacts), matching your explicit decision between the two
  approaches presented earlier, and the "shim source of truth" point
  in Architecture is consistent with never editing
  `third_party/shim/` from within this new repo.
- **Scope**: one new repo, one new host-level toolchain, a
  self-contained C++ exceptions proof as its own done-criterion —
  appropriately sized for one implementation plan. Retrying NativeAOT
  is explicitly deferred to a follow-on, not folded in here.
