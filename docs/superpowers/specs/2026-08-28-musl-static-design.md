# Static musl Milestone

**Date:** 2026-08-28
**Status:** Approved
**Roadmap position:** Milestone 4 of 17
(see `2026-08-27-roadmap-architecture-design.md`)

## Purpose

Bring up **statically linked musl** on NeoOS: build the Linux-shaped
kernel primitives musl is written against, vendor musl, write the
syscall-number translation shim, and run a musl program that uses
`printf`, `malloc` and thread-local storage.

This is the milestone the whole musl direction rests on. Once musl
works statically, the dynamic-linking milestone becomes "port musl's
`ldso`" against a libc already known to work, rather than debugging two
large unproven pieces at once.

## What the current code forces

- **No memory mapping of any kind.** There is no VMA concept:
  `free_address_space` walks page tables, and `thread_stack_alloc` maps
  frames directly. `mallocng` cannot work without `mmap`.
- **No process startup ABI.** `lib/crt0.asm` passes `argc = 0`,
  `argv = NULL`, and no `envp` or auxv at all. musl's `__init_libc`
  walks the stack for `envp` and reads the auxiliary vector after it.
- **`elf_load` returns only the entry point**, so nothing can supply
  `AT_PHDR`, which `__init_tls` needs to find `PT_TLS`.
- **No FS base plumbing.** `grep -rn fs_base kernel/` is empty, and
  `arch_prctl` does not exist.
- **Programs link at `0x200000000000` with `-mcmodel=large`**, because
  `user.ld` deliberately avoids the low 4GiB — that is `PML4[0]`, the
  kernel's identity map, shared by every process.

## Decisions

| # | Decision | Chosen |
|---|---|---|
| 1 | Milestone size | One milestone, through to a running musl binary |
| 2 | musl location | Vendored in-tree under `third_party/musl` |
| 3 | Shim mechanism | Rewrite musl's `__NR_*` table; no runtime translation |
| 4 | mmap population | Demand paging, not eager |
| 5 | `brk` | Deliberately `-ENOSYS` |

### On decision 3

musl's `arch/x86_64/syscall_arch.h` already uses **exactly NeoOS's
register convention**: `rax` = number, `rdi`/`rsi`/`rdx`/`r10` = args,
clobbering `rcx`/`r11`. The only mismatch is the numbers, so the shim
is a pure data change to one generated header — no runtime table, no
indirection, and musl's own code untouched. This is the
"translation, never emulation" rule from `CLAUDE.md` in its cleanest
possible form.

## `mmap` and the address-space manager

```c
struct vma {
    uint64_t   start, end;   /* [start, end), page-aligned  */
    uint32_t   prot;         /* PROT_READ|WRITE|EXEC        */
    uint32_t   flags;        /* MAP_PRIVATE|ANONYMOUS|FIXED */
    struct vma *next;
};

struct process {
    ...
    struct vma *vmas;        /* sorted by start, non-overlapping */
    uint64_t    mmap_next;   /* bump hint for unhinted mmap      */
};
```

### Layout

Using the gap that already exists between the image and the stacks:

```
0x200000000000  ELF image (user.ld)
0x500000000000  mmap region, growing UP        <- new
0x700000000000  thread stacks, growing DOWN (16 slots)
```

### Demand paging

`mmap` records the VMA and maps nothing; the page-fault handler
consults the list and allocates a frame on first touch. Three reasons
this is worth more than eager population:

- `mallocng` maps regions considerably larger than it immediately
  touches, and eager population would burn real frames on untouched
  address space.
- `PROT_NONE` guard regions cost nothing instead of being wasteful.
- **The fault path already exists.** `isr_handler` routes `#PF` to
  `paging_handle_cow_fault`; this adds a second consultation to the
  same hook rather than a new mechanism.

Handler order: COW first (present + write + user), then VMA lookup for
a not-present fault, then `SIGSEGV`. A fault with no matching VMA is
exactly what should raise `SIGSEGV`, which the signals milestone
already delivers correctly.

### `munmap` splits

Unmapping the middle of a region leaves two, so the list needs split
and merge, and only the frames actually populated may be freed. Getting
this wrong leaks silently, which is what the leak gate is for.

### One representation, not three

Thread stacks, ELF segments and `free_address_space` all fold into the
VMA list. `thread_stack_alloc` becomes a `MAP_FIXED` mapping and
`free_address_space` walks VMAs. The alternative — stacks outside the
list — means `munmap` has to special-case them forever.

## Process startup ABI

musl's `__init_libc` walks the initial stack for `envp` and reads the
auxiliary vector immediately after it; `__init_tls` uses `AT_PHDR`/
`AT_PHENT`/`AT_PHNUM` to locate `PT_TLS`; `__init_ssp` dereferences
`AT_RANDOM`. None is optional — a musl binary cannot start without
them.

```
high
        "PROG.ELF\0"             argv[0] string
        16 random bytes          AT_RANDOM points here
        AT_NULL, 0
        AT_RANDOM, <ptr>   AT_ENTRY, <e_entry>   AT_BASE, 0
        AT_PAGESZ, 4096    AT_PHNUM, <n>
        AT_PHENT, 56       AT_PHDR,  <vaddr>
        NULL                     end of envp
        NULL                     end of argv
        argv[0] ptr
rsp ->  argc
low
```

`rsp` must be **16-byte aligned at `_start`** — the same constraint
`crt0.asm` already documents for its `call main`.

### `elf_load` reports more

`AT_PHDR` needs the *virtual* address the program headers landed at:
the `PT_LOAD` segment containing `e_phoff`, giving
`p_vaddr + (e_phoff - p_offset)`. `AT_PHENT` and `AT_PHNUM` come
straight from the ELF header.

### `crt0.asm` is rewritten

```nasm
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv
    call main
```

This is a useful property: **every existing test program exercises the
new stack layout immediately**, because they all link `crt0.o`. They
ignore `argc`/`argv`, so the boot log should be unchanged — making it a
gate-able refactor rather than something only musl proves.

### Two C runtimes coexist

`libneoos` keeps `crt0.o` for the existing programs; musl brings its
own `crt1.o` and reads the same stack. Nothing is shared between them
except the layout, which is exactly why matching Linux's matters.

### `AT_RANDOM` is not a security boundary

NeoOS has no entropy source. `AT_RANDOM` is seeded from `RDTSC` so
musl's stack canary is non-constant. It is **not** cryptographic and
must not be treated as such.

## FS base and the remaining primitives

`arch_prctl(ARCH_SET_FS)` is mandatory even for a program with no
thread-locals: musl's `__init_tls` always calls `__set_thread_area`.
`struct thread` gains `uint64_t fs_base`, written to MSR `0xC0000100`
on every context switch.

**A trap this project has already hit once:** `kernel_thread_trampoline`
and `fork_trampoline` both do `mov fs, dx`, and **loading the FS
selector zeroes `FS_BASE`** — exactly as loading GS zeroed `GS_BASE` in
the threads milestone. `fork`, which resumes a child through that
trampoline, must restore `fs_base` *after* the segment loads, or a
forked child silently loses its TLS pointer.

| Syscall | Notes |
|---|---|
| `futex` | `WAIT`/`WAKE` + `PRIVATE_FLAG`, all musl uses. Hash of wait queues keyed by `(process, user address)`; `WAIT` re-reads the user word and returns `-EAGAIN` if it no longer matches. Sits on the existing `waitq`, including `waitq_sleep_timeout`. |
| `set_tid_address` | Stores `clear_child_tid`, returns the tid. On thread exit the kernel writes 0 there and futex-wakes. Unused until `pthread_join`. |
| `writev`/`readv` | Loops over the existing read/write paths. stdio's real write path is `writev`. |
| `fstat` | Linux's exact 144-byte `struct stat`. stdio picks buffering from `st_blksize` and behaviour from `st_mode`, so `S_IFREG`/`S_IFDIR`/`S_IFCHR` must be right. |
| `ioctl(TCGETS)` | 0 with a filled `termios` for `/dev/CONSOLE`, `-ENOTTY` for regular files. That distinction alone makes `isatty()` true and gives line-buffered console output. |
| `clock_gettime` | `CLOCK_REALTIME` and `CLOCK_MONOTONIC` from `timer_ticks()` at 100Hz. Coarse by construction. |
| `brk` | `-ENOSYS`. `mallocng` uses `mmap` exclusively; `brk` would be a second allocator to keep correct. |

### `default: return -ENOSYS`

Small change, outsized importance. musl probes for features and falls
back when a syscall is missing — but only if it receives `-ENOSYS`
rather than a wrong answer. Together with the sentinel remap below,
that is what keeps an unimplemented syscall a clean failure instead of
an accidental `spawn`.

## Vendoring, the remap, and the build

`third_party/musl/` holds musl 1.2.5, sha256
`a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`
(verified by two independent downloads; **not** a signature check).
`third_party/musl-README.md` records the version, upstream URL,
checksum, and every local modification.

**The NeoOS patch stays a separate file** rather than being applied in
place, so `git diff` against pristine upstream is always available and
a future musl bump is a re-apply rather than archaeology.

### The remap

A generated rewrite of `arch/x86_64/bits/syscall.h.in`. All 363
entries change:

```
implemented    __NR_write  -> 1            (NeoOS SYS_WRITE)
unimplemented  __NR_stat   -> 0x8000 + 4   sentinel, always -ENOSYS
```

The sentinel matters as much as the mapping. Leaving Linux's
`__NR_stat = 4` in place would call NeoOS's `SYS_SPAWN`; every unmapped
number must be pushed out of range so it lands on the `-ENOSYS`
default.

### The top build risk

Programs link at `0x200000000000` with `-mcmodel=large`, because
`user.ld` avoids the low 4GiB — that region is `PML4[0]`, the kernel's
identity map, shared by every process. So **musl must also be built
`-mcmodel=large -fno-pic`**, which is not a configuration musl is
routinely exercised in.

If musl will not build or link that way, the alternatives are worse:
moving programs into the low 2GB collides with the identity map, and
removing that dependency is its own milestone.

**"Get musl to build and link at all" is therefore the first task**,
ahead of any kernel work. If it cannot be done, the milestone's shape
changes, and that should be known on day one rather than at task six.

## Verification

Existing convention: in-kernel selftests announcing `passed`/`FAILED`,
userland test programs, headless QEMU under `timeout`, serial log
grepping. `-cpu Nehalem` suffices; nothing here needs AVX.

Layered so each failure is attributable:

- musl builds; a program linking `crt1.o` + `libc.a` reaches `main` and
  returns — proving the auxv stack and `arch_prctl`
- raw `write()` through musl — proving the shim's number mapping
- `printf` — proving `writev`, `fstat`, `ioctl(TCGETS)`, line buffering
- `malloc`/`free` across varied sizes — proving `mmap`, demand paging
  and `munmap`
- a `__thread` variable — proving the FS base survives context switches
- **the existing suite unchanged**, since every current program links
  the rewritten `crt0.o` and so exercises the new stack layout
- the leak gate at 5 and 10 iterations, since `mmap` adds a new class
  of per-process allocation

## Out of scope

- **pthreads.** Needs `clone` semantics and per-thread TLS blocks;
  the shim maps `pthread_create` onto NeoOS threads in a later
  milestone.
- **Dynamic linking.** musl's `ldso` is the next milestone.
- **`brk`**, and any second allocator.
- **File-backed `mmap`.** Anonymous only; the dynamic linker will need
  file-backed and can add it.
- **A real entropy source.** `AT_RANDOM` is `RDTSC`-seeded and is not a
  security boundary.
- **`vDSO`.** `AT_SYSINFO_EHDR` is absent; musl falls back to real
  syscalls, which is what we want.
