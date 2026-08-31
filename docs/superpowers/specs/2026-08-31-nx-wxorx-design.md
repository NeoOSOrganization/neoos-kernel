# NX / W^X enforcement — design

**Date:** 2026-08-31
**Status:** design (solo brainstorm — see roadmap). Bounded hardening of
existing paging/loader code; small enough that the plan is short.
**Predecessor context:** `kernel/mm/paging.h` (`PAGE_NO_EXECUTE`,
`PAGE_WRITABLE`), `kernel/elf.c`, `kernel/mm/vma.c`.

## Problem

EFER.NXE is enabled and `PAGE_NO_EXECUTE` works, but the flag's *use*
is inconsistent, so the invariant "no page is both writable and
executable" does not hold:

1. **User `.text` is W+X.** `elf_load()` maps every PT_LOAD segment
   `PAGE_WRITABLE` (`kernel/elf.c:88`, comment: "always writable -- no
   read-only .text/.rodata this milestone"). A read-only executable
   segment gets write permission it should never have.
2. **`mmap` has no W^X check.** `vma.c` sets `PAGE_WRITABLE` from
   `PROT_WRITE` and `PAGE_NO_EXECUTE` from `!PROT_EXEC` independently
   (`vma.c:213-214`), so `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)` or an
   `mprotect` to the same produces a W+X page with no diagnostic.
3. **Kernel mappings.** The physmap and kernel data are mapped
   `PAGE_WRITABLE` with no NX (`paging.c`), and the kernel text mapping
   from `boot.asm` needs auditing — kernel `.text` should be RO+X,
   `.rodata` RO+NX, `.data`/`.bss`/physmap RW+NX.

## Goals

- **W^X for user space:** a user page is writable or executable, never
  both. Enforced at `mmap`, `mprotect`, and ELF load.
- **W^X for the kernel:** `.text` RO+X; everything else NX; nothing
  both W and X. Verified by a boot-time selftest that walks the kernel
  PML4.
- Loader maps segments with the real permissions from `p_flags`
  (RO unless `PF_W`, NX unless `PF_X`).
- A recorded, deliberate divergence from Linux: Linux *permits* W+X
  (with warnings / lockdown knobs); NeoOS refuses it outright.
  `mmap`/`mprotect` with `PROT_WRITE|PROT_EXEC` returns `-EINVAL`.

## Non-goals

- W^X exceptions for JITs (`memfd`, dual-mapping). If a future port
  needs one, it gets an explicit `MAP_` opt-in and its own divergence
  note. Not now.
- Making `.rodata` a separate segment in NeoOS's own userland binaries
  (they are built without `-z separate-code` awareness). The loader
  honors whatever segments the ELF actually has; it does not invent
  finer protection than `p_flags` describes.
- SMEP/SMAP (separate hardening; worth its own milestone).
- Kernel `.rodata` const-ification audit at the C level.

## Design

### 1. Loader (`kernel/elf.c`)

`elf_load` per PT_LOAD segment:

```
flags = PAGE_USER
if (p_flags & PF_W)  flags |= PAGE_WRITABLE
if (!(p_flags & PF_X)) flags |= PAGE_NO_EXECUTE
if ((p_flags & PF_W) && (p_flags & PF_X))
    // A W+X segment in the ELF itself: reject the exec.
    return -ENOEXEC   (log it)
```

The BSS tail (`p_memsz > p_filesz`) is zero-filled as today and takes
the segment's flags — a `.bss` in a `PF_W` segment stays writable,
which is correct.

Relocation for `ET_DYN` (PIE, needed by ASLR milestone) happens before
this; the protection logic is unchanged by it.

### 2. `mmap` / `mprotect` (`kernel/mm/vma.c`)

At the top of `vma_mmap_locked` and `vma_mprotect_locked`:

```
if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -EINVAL;
```

Recorded in `docs/stdlib.md`: "NeoOS rejects `PROT_WRITE|PROT_EXEC` on
mmap/mprotect with `-EINVAL`; Linux permits it."

The existing PTE-flag derivation (`vma.c:213-214`) then needs no
change — it already produces W-only or X-only pages once W+X is
refused at the entry.

Demand-paging fault handler (`vma.c:201-214`) unchanged.

### 3. Kernel address space

Audit and fix, in `boot.asm` (early PML4) and `paging.c`
(`map_physmap`, `map_kernel_*`):

| Region | Now | Target |
|---|---|---|
| kernel `.text` | RW+X (likely) | RO + X |
| kernel `.rodata` | RW+NX or RW+X | RO + NX |
| kernel `.data`/`.bss` | RW+X or RW+NX | RW + NX |
| physmap (`PHYSMAP_PML4_INDEX`) | RW, no NX | RW + NX |
| page-table frames | RW | RW + NX (they are data) |

The linker script (`linker.ld`) already emits `.text` / `.rodata` /
`.data` with symbols at the boundaries (`__text_start` etc. — add if
missing, 4 KiB-aligned). A post-paging pass in `paging_init` walks
`[__text_start, __text_end)` and clears `PAGE_WRITABLE`; walks
`[__rodata_start, __rodata_end)` clears `WRITABLE`, sets `NO_EXECUTE`;
everything else in the kernel range gets `NO_EXECUTE`. Huge-page
mappings covering a boundary are split to 4 KiB first (a `paging_split_
huge(va)` helper — small, and reusable).

Order matters: do this AFTER the physmap and kernel heap are up (the
walk allocates page tables when splitting) and BEFORE `process_init`.

### 4. Selftest

`kernel/mm/wxorx_selftest.c`, run from `paging_init`'s tail:

- Walk the active PML4 over the kernel half. For every present 4 KiB
  leaf: assert not (`PAGE_WRITABLE` and not `PAGE_NO_EXECUTE`). Print
  `[wxorx] kernel selftest passed` or a `FAILED` line naming the first
  offending VA.
- A negative check via a helper that reports whether a hypothetical
  `(W,X)` flag combo would be accepted by `vma_mmap` — like
  `lock_rank_ok`, proves the check fires without faulting.

Userland: extend `mmaptest.c` with
`mmap(PROT_WRITE|PROT_EXEC) == MAP_FAILED && errno == EINVAL`, and an
`mprotect` to W+X on an existing mapping returning `-EINVAL`. Add
`[wxorx]` to `REQUIRED_MARKERS` if a new marker string is introduced;
otherwise fold into `[mmaptest] ALL PASSED`.

## ABI / stdlib impact

- `docs/stdlib.md`: mmap/mprotect W+X divergence (returns `-EINVAL`).
- `docs/abi-compatibility.md`: note NeoOS enforces W^X where Linux
  merely discourages it; a Linux binary that relies on W+X pages
  (rare, mostly old JITs) will not run unmodified.
- No struct layout or constant changes.

## Risks

1. **A NeoOS userland binary with a single RWX load segment** would
   stop loading. Mitigation: check the toolchain output for every
   `userland/*.ELF` during the plan's first task; if any has an RWX
   PT_LOAD, fix the link flags before turning the loader check on.
2. **Splitting a huge page that maps live kernel code** while executing
   from it — must flush TLB correctly and not free the old table until
   after the flush. Reuse the tlb.c deferred-free discipline.
3. **The physmap NX flip** must not break the kernel's own reads
   through it (data only — safe) but a stray `call` through a physmap
   address would now #PF instead of silently working. That is the
   point; if the selftest suite trips it, it found a real bug.

## Plan sketch (for `writing-plans`)

1. Audit `userland/*.ELF` segment flags; fix any RWX link. Add
   `__text_start/_end`, `__rodata_start/_end` to `linker.ld`.
2. `elf_load`: honor `p_flags`, reject in-file W+X. Gauntlet.
3. `vma_mmap`/`vma_mprotect`: `-EINVAL` on `PROT_WRITE|PROT_EXEC`;
   `mmaptest` assertions. Gauntlet.
4. `paging_split_huge` helper + `paging_init` protection pass +
   `[wxorx]` kernel selftest. Gauntlet ×3.
5. Docs: stdlib.md, abi-compatibility.md.
