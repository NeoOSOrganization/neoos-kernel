# NeoOS

A hobby x86_64 kernel built from scratch: Multiboot2 boot, interrupts,
memory management, storage, and onward, one milestone at a time.

## Project conventions

- Development proceeds in milestones: brainstorm -> design spec
  (`docs/superpowers/specs/`) -> implementation plan
  (`docs/superpowers/plans/`) -> implementation, verified via headless
  QEMU and serial log capture (no host-runnable unit tests -- this is
  bare-metal code with no host runtime to run tests in).
- Work happens directly on `main` (no feature branches), matching
  every milestone so far, by explicit user preference.

## Standard library convention

NeoOS's C library is **musl**, reached through a thin adaptor layer.
The OS is deliberately NOT reshaped to suit musl: partial
compatibility is fine, and NeoOS may diverge from Linux semantics
where it chooses to. The adaptor absorbs the mismatch.

The adaptor is **translation only, never emulation**: the kernel
provides Linux-SHAPED primitives (futex, mmap, stat, signals,
clock_gettime) under NeoOS's own syscall numbers, and the shim in
musl's arch directory maps musl's Linux numbers onto them. If the shim
ever starts emulating a primitive rather than forwarding to one, that
is the signal to add the primitive to the kernel instead.

`lib/` keeps only what has no POSIX analogue — `spawn`, wait-by-pid,
`mount`/`umount`, and later ports/MPI. Everything musl provides
(`printf`, `opendir`, pthreads, `string.h`) comes from musl.

**Any kernel feature that becomes usable by an external/user-mode
application (a new syscall, a new syscall argument, a new capability)
MUST be accompanied by:**
1. Either a musl-visible path (a shim entry mapping the relevant musl
   syscall onto it) or, for NeoOS-native features, a `lib/` wrapper.
2. An update to `docs/stdlib.md` describing the new or changed
   function, or — for anything that deviates from POSIX/Linux
   behaviour — an explicit note of the divergence.

Do not leave a user-facing kernel feature exposed only via a raw
syscall number with no library support. `docs/stdlib.md` documents the
NeoOS extensions and the deliberate divergences, not a whole libc;
musl documents itself.

## Keeping neoos-os-builder in sync

`neoos-os-builder` (sibling repo) assembles a bootable image from
neoos-kernel + neoos-musl + a user-chosen set of ports, via
`scripts/build.sh`. It does not discover ports or their dependencies
on its own — it clones whatever `neoos-<port>` a config's `ports:`
list names and builds each with whatever directories `build.sh`
already knows to pass. **Adding a new port, or a new kernel feature a
port depends on, does nothing for os-builder users until that script
is updated to match.**

Concretely, after landing a new port repo or a kernel feature/syscall
a port needs:

- If the port has build dependencies beyond musl (e.g. `neoos-curl`
  needs `neoos-openssl` and `neoos-libssh2` built first, in that
  order), `scripts/build.sh`'s per-port build loop must pass the right
  `<DEP>_DIR=` variables for that port's `Makefile` — the loop
  currently only ever passes `MUSL_DIR`, which silently breaks any
  port with a real dependency chain (confirmed still broken for
  `openssl`/`libssh2`/`curl` as of the curl milestone) and has no
  notion of build ORDER between ports (a port must be built after
  every port it depends on, not just after musl).
- Update `docs/BUILD_ORDER.md`'s pipeline diagram and dependency list
  to include the new port/chain.
- Add or update an example `config/*.yaml` demonstrating the new
  port(s), the way `busybox-doom.yaml` does for the two it names.

Treat an os-builder gap surfaced this way as a real bug in that repo,
not a documentation nicety — a port nobody can actually select and
build through the tool that exists to do exactly that is not finished.

## Linux ABI compatibility

**Internals are ours; the ABI is not.** Anything that never crosses
into userland — kernel data structures, internal calling conventions,
lock ranks, NeoOS's own syscall numbers, scheduler and memory
internals — may be designed, renamed, and reshaped freely. There is no
obligation to resemble Linux inside the kernel.

But **the long-term goal is to run real Linux applications on NeoOS
without patching them**. So wherever a kernel primitive is
*observable* from a user-mode program, it must be Linux-SHAPED:

- struct layouts crossing the boundary (`stat`, `dirent`, `timespec`,
  `sigaction`, `utsname`, ...) match Linux's x86_64 field order,
  sizes, and padding
- flag and constant values (`O_*`, `PROT_*`, `MAP_*`, `SIG*`, `AT_*`,
  `CLOCK_*`, errno numbers) match Linux's values
- semantics match Linux's where an application could tell the
  difference — return values, error codes, edge-case behaviour
- the auxv/ELF entry contract, TLS setup, and signal frame layout
  follow the x86_64 SysV + Linux conventions

The syscall *numbers* stay NeoOS's own; the shim translates those. It
is the shapes and semantics behind the numbers that must not diverge,
because no shim can retrofit a struct layout an application compiled
against.

**Every deliberate divergence from Linux must be recorded in
`docs/stdlib.md`** with its reason. An unrecorded divergence is a bug,
not a design choice.

At the end of each milestone, refresh `docs/abi-compatibility.md`: what
of the Linux ABI is implemented, what is stubbed, what diverges and
why, and what a real ported application would still hit. If that file
does not exist yet, the milestone that first needs it creates it.
