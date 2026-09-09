# Dynamic linking on NeoOS

## Why

Every binary NeoOS runs today is static. `kernel/elf.c` handles
`PT_LOAD` and `PT_TLS` and nothing else: no `PT_INTERP`, no `ET_DYN`
relocation, no `dlopen`. That single gap is the largest obstacle
between NeoOS and the stated long-term goal of running real Linux
applications unmodified, and it turns up in every porting decision
made so far:

- The C# application links LVGL through NativeAOT's `DirectPInvoke`
  rather than an ordinary `DllImport`, because a runtime-resolved
  P/Invoke needs `dlopen`.
- The Flutter engine ships as `libflutter_engine.so` and static
  linking of it was rejected upstream (flutter/flutter#124902, closed
  `r: invalid`), so Flutter is unreachable purely for this reason.
- Every port is a bespoke static build rather than a package.

The prize is not any one of those. It is that essentially every
prebuilt Linux userland artefact assumes a dynamic loader.

## What already exists

Established by reading the tree on 2026-09-09, not assumed:

| piece | state |
|---|---|
| `kernel/elf.c` | 204 lines. `PT_LOAD` + `PT_TLS` only. No `PT_INTERP`, no `ET_DYN`. |
| auxv | `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM` are emitted. **`AT_BASE` is `#define`d but never pushed.** |
| `MAP_FIXED` | **implemented** in `vma_mmap` — ld.so needs it to place segments. |
| `mmap` of a file | **absent.** `sys_mmap` returns `-ENOSYS` for anything non-anonymous outside the `/dev/fb0` and memfd device hooks. Its comment already anticipates this work. |
| `mprotect`, `munmap`, `fstat`, `membarrier` | present. |
| `pread` | **absent.** `dynlink.c` uses it to read ELF headers. |
| musl | built `--disable-shared`, so `libc.so` / `ld-musl-x86_64.so.1` do not exist. |
| W^X | `vma_mmap_locked` refuses `W|X`; `mprotect` refuses the transition. |

**What musl's `ldso/dynlink.c` actually needs from the kernel**, by
inspection: `open`, `read`, `pread`, `close`, `fstat`, `mmap`,
`mprotect`, `munmap`, `membarrier`. Nine calls, of which NeoOS lacks
two — and one of those is the only substantial item in this document.

## The shape

**The kernel performs no relocation.** Relocation, symbol resolution,
`dlopen`, and TLS block layout stay entirely in musl, which already
implements them correctly and is already the C library. The kernel's
job is to load two ELF objects instead of one and to describe them
accurately in the auxiliary vector.

```
execve("/bin/prog")
  └─ kernel/elf.c
       ├─ no PT_INTERP  → load, jump to e_entry                  (today)
       └─ PT_INTERP     → load the executable
                        → load the interpreter at a base address
                        → auxv: AT_PHDR / AT_PHNUM / AT_ENTRY describe
                                THE EXECUTABLE, AT_BASE is the
                                INTERPRETER's load base
                        → jump to the INTERPRETER's entry
                             └─ ld.so maps the libraries, relocates,
                                and calls the executable's entry
```

`AT_BASE` is what tells ld.so where it was placed so it can relocate
itself before it can do anything else. `AT_ENTRY` must still be the
executable's entry point, not the interpreter's: it is how ld.so knows
where to hand control once linking is done.

## W^X

Not an obstacle, and this was checked rather than assumed. musl maps
each `PT_LOAD` with that segment's own flags — text `PROT_READ |
PROT_EXEC`, data `PROT_READ | PROT_WRITE` — and never requests both on
one mapping. RELRO only ever *drops* permission, to read-only, after
relocation. No `W|X` mapping is ever requested, so NeoOS's refusal
never fires.

This is worth stating plainly because W^X blocked the Dart JIT and it
would be reasonable to assume it blocks this too. It does not.

## The hard part: faulting a file page

`sys_mmap` must accept `MAP_PRIVATE` of a file. The mapping is
**demand-paged**: pages are read from the file when first touched, and
a write to one is copy-on-write against the caller's private copy.

The obstacle is the current fault path. `vma_fault` takes
`p->mm_lock` — a **spinlock**, taken with `spin_lock_irqsave`, so
interrupts are off — and calls `vma_fault_locked` with it held. The
filesystem cannot be reached from there:

- `vfs_lock` is a **mutex** (`kernel/fs/vfs.c:77`). It sleeps. Sleeping
  while holding a spinlock with interrupts disabled is a deadlock.
- Even where it did not sleep, holding a spinlock across a disk read is
  indefensible.

So the fault path is restructured to **drop and retry**, which is what
Linux does with `mmap_lock` for exactly this reason:

1. Take `mm_lock`, find the VMA.
2. **Anonymous fault:** handled inline, entirely as today. This path is
   unchanged and stays fast — it is by far the common case, and the
   restructure must not slow it down.
3. **File-backed fault, page absent:** copy out what is needed (the
   vnode reference, the file offset, the prot bits), **release
   `mm_lock`**, then with no lock held allocate a frame and read the
   page through the VFS.
4. **Re-acquire `mm_lock` and re-validate.** The world may have moved:
   the VMA may have been unmapped or replaced, or another thread may
   have faulted the same page and installed it. If the mapping is gone,
   free the frame and fail the fault. If a page is already present,
   free our frame and use the installed one. Only otherwise install.

Step 4 is where this goes wrong if hurried — a missed re-validation is
either a leaked frame or a use-after-free, and both hide. It gets a
dedicated test rather than trusting inspection.

A partial read (the mapping extends past EOF) zero-fills the remainder,
which is what POSIX requires and what a `.bss` at the end of a data
segment depends on.

## Milestones

This document specifies **DL-1** only. DL-2 is named here so the split
is explicit, and gets its own spec when DL-1 is solid.

### DL-1 — the mechanism

Running dynamically linked executables, and `dlopen`.

1. **`pread`** — a new syscall. Small and self-contained.
2. **File-backed `MAP_PRIVATE` mmap**, demand-paged, including the
   fault-path restructure above.
3. **`PT_INTERP` and `ET_DYN`** in `kernel/elf.c`: load a second object
   at a chosen base, and load position-independent executables.
4. **`AT_BASE`** in the auxiliary vector.
5. **musl built shared**, producing `libc.so` — which in musl *is* the
   dynamic linker, installed as `ld-musl-x86_64.so.1`.
6. **`docs/stdlib.md`** and **`docs/abi-compatibility.md`** updated, per
   CLAUDE.md.

**Done when** a hello-world built *without* `-static` runs and prints
its marker, and a `dlopen` / `dlsym` / `dlclose` round trip against a
NeoOS-built `.so` succeeds.

### DL-2 — the conversion

Rebuilding NeoOS's own libraries as shared objects: `libneoos`, LVGL,
`libwmclient`, and the ports, with the existing programs relinked
against them. Cross-repo churn touching five repositories, and it risks
destabilising a set of programs that currently work. It is worth doing
— it is what makes the memory saving real — but only after DL-1 has
proven the mechanism, and not in the same milestone.

## Testing

No host tests exist and none can: this is bare-metal code. Every task
is verified by building, booting headless in QEMU, and grepping the
serial log, and `tools/gauntlet.sh 15 3` runs after every task that
changes the fault path — a bug there shows up as a rare hang, not a
failure.

- **`userland/dyntest.c`**, built **without `-static`**. That it runs at
  all is the milestone; it prints a marker to say so.
- **A `dlopen` round trip** against a small purpose-built `.so`:
  `dlopen`, `dlsym` a function, call it, check the result, `dlclose`.
- **A concurrent file-fault test**: two threads touching the same page
  of one mapping must end up sharing a single frame, with the free-frame
  count returning to its baseline afterwards. This is the
  re-validation test, and it is the one that matters.
- **A short-read test**: a mapping extending past EOF must read zeros
  beyond the end rather than stale memory.

## Risks

- **The fault restructure is the whole risk.** Everything else in DL-1
  is mechanical. Dropping a lock mid-fault introduces a window that did
  not previously exist, and the failure modes are silent.
- **Faulting through PIO ATA is slow.** `kernel/drivers/block/ata.c`
  polls `BSY` and moves data with `insw`; every 4 KiB page fault is a
  synchronous polled read. Correct, and slow. A page cache is what
  fixes it, and is deliberately not in this milestone.
- **No cross-process text sharing.** `MAP_PRIVATE` means each process
  still gets its own copy of a library's pages; demand paging changes
  *when* they are allocated, not *whether* they are shared. Recorded as
  a divergence, and the reason DL-2 alone does not deliver the memory
  saving people expect from shared libraries.
- **A foreign Linux `.so` is out of scope.** `dlopen` of a NeoOS-built
  object is the target. Loading something from a distribution will
  surface a long tail of missing syscalls, and that hunt is unbounded
  in a way the rest of this is not.
