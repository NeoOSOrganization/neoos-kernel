# Fault-Tolerant User-Copy Machinery — Design

**Date:** 2026-09-06
**Status:** approved, ready for implementation
**Trigger:** the NeoOS Doom port (`neoos-doom`) crashed the kernel
loading its 4.2MB WAD file. Root cause, traced to the exact faulting
instruction via `addr2line`: `sys_read` (`kernel/syscall/sys_file.c`)
passes the raw user-space destination pointer straight down to
`fatfs_read`/`fat16_read_at_v`, which writes into it directly from
kernel context. When that destination page has not yet been demand-
paged in (a fresh `malloc()` buffer nothing has touched yet), the CPU
takes a page fault with `cs` = the kernel code segment (ring 0) — and
`isr.c`'s demand-paging path (`vma_fault`) only fires for **ring-3**
faults (`(regs->cs & 3) == 3`), so this instead hits the "kernel bug,
halt the machine" path. This is not FAT16-specific or Doom-specific:
**any syscall writing into or reading from a user buffer large enough
to reach an untouched page will crash the kernel**, regardless of
which filesystem or device is on the other end. Nothing in the
existing test suite ever did a read large enough, into a buffer fresh
enough, to hit it before.

## 1. Problem

A full audit of every syscall handler touching user memory
(`kernel/syscall/sys_file.c`, `sys_mem.c`, `sys_misc.c`, `sys_proc.c`,
`sys_signal.c`, `sys_poll.c`, `sys_net.c`, `kernel/net/socket.c`,
`kernel/ipc/futex.c`) found the kernel in two states at once:

- **Already safe**: `sys_net.c`/`socket.c` (via shared `addr_in`/
  `addr_out` helpers), `sys_poll.c`, `sys_mem.c`, `ipc/futex.c`, and a
  handful of others (`sys_pipe2`, `sys_uname`, `copy_user_argv`) call
  `user_range_writable`/`user_range_readable` before touching the
  pointer. These do not crash today — but `user_range_writable` only
  checks that a page is **currently `PAGE_PRESENT`**, so a legitimate,
  valid, but never-yet-touched buffer (exactly Doom's case) still
  fails these checks with a spurious `-EFAULT`, rather than working.
- **Genuinely unguarded**: 18 call sites across `sys_file.c`,
  `sys_misc.c`, `sys_proc.c`, and `sys_signal.c` do only a NULL or
  truthy check on the pointer, then dereference it directly — each one
  crashable exactly like `sys_read` was. Full list in §4.

Both states share one root cause: there is no primitive in this kernel
that safely copies to/from a user address while (a) tolerating a fault
on a legitimately unbacked-but-valid page by demand-paging it in and
retrying, and (b) still treating a fault on a genuinely invalid
address as a clean, contained failure — not a machine-halting "kernel
bug", and not a silent, unbounded "just keep going" either.

## 2. Scope

**In scope:**
- A general `copy_to_user`/`copy_from_user` primitive (§3.2 has the
  exact signatures) built on an exception-table mechanism: specific
  copy-loop
  instructions are marked recoverable; a ring-0 fault at one of those
  exact instructions is checked against the current process's VMA
  (reusing `vma_fault`'s existing logic) and either demand-pages +
  retries transparently, or lands at a fixup that returns a clean
  `-EFAULT` — every *other* kernel-mode fault still crashes loudly,
  exactly as today. This is the safety-preserving design: a blanket
  "any ring-0 fault touching a low address is a demand-page candidate"
  would mask genuine kernel bugs (a wild pointer that happens to land
  in valid user-mapped memory) instead of catching them.
- Converting all 18 confirmed-unguarded call sites (§4) to the new
  primitive — this is the actual crash fix.
- Migrating the already-guarded call sites (`user_range_writable` +
  raw copy) onto the same primitive, so they gain the same "valid but
  untouched" tolerance instead of a spurious `-EFAULT`.
- `docs/abi-compatibility.md` / a kernel-internals note recording this
  as a new, general-purpose safety primitive (not user-visible ABI,
  but significant enough to document per this project's own
  "refresh at the end of a milestone" convention).

**Out of scope (deliberately, YAGNI):**
- Rewriting `user_range_writable`/`user_range_readable` themselves.
  They stay as fast pre-checks for callers that only need a boolean
  answer (e.g. before allocating a buffer sized from user input); the
  new primitive is for the actual copy.
- A general "copy N bytes as fast as possible" optimization pass
  (SIMD, unrolling). Correctness first, matching this project's
  existing "widest safe copy without the machinery for wide vector
  stores" precedent from the AC97 driver's `blit_row`.
- Any change to demand-paging for ring-3 faults. `vma_fault`'s
  existing ring-3 path is unaffected; this only adds an equivalent
  path reachable from ring-0 through the exception table.

## 3. Decisions

### 3.1 The exception table

A `.ex_table` linker section, each entry a fixed-size struct pairing a
faulting instruction's address with its fixup's address:

```c
// kernel/mm/uaccess.h
struct exception_entry {
    uint64_t fault_addr;   // address of the instruction that may fault
    uint64_t fixup_addr;   // where to resume instead, on an unrecoverable fault
};
```

Every recoverable copy instruction gets a matching entry, emitted via
inline asm's own local labels so the addresses are always exactly
right even after compiler scheduling/optimization:

```c
// One word (8 bytes) at a time, with a byte-copy tail -- matches this
// kernel's existing "widest safe copy without vector-store machinery"
// convention (see vesafb_blit's blit_row). `label` is a unique
// per-call-site identifier (e.g. __LINE__ pasted into the symbol) so
// two inlined copies in the same translation unit do not collide.
#define EX_TABLE_ENTRY(fault_label, fixup_label) \
    ".pushsection .ex_table, \"a\"\n" \
    ".quad " #fault_label "\n" \
    ".quad " #fixup_label "\n" \
    ".popsection\n"
```

The table is a plain array in a dedicated linker section (`linker.ld`
gains one new `.ex_table` output section, page-aligned like the
others), with `__ex_table_start`/`__ex_table_end` symbols bounding it
— the same "linker-provided bounds symbols" pattern already used
elsewhere in this kernel's boot/init sequence (grep for existing
`_start`/`_end`-style symbols in `linker.ld` and match that style
exactly).

### 3.2 The copy primitive

```c
// kernel/mm/uaccess.h

// Copies `n` bytes from kernel memory `src` to user memory `dst`.
// Returns 0 on full success, or the number of bytes that could NOT be
// copied (Linux's copy_to_user convention exactly) if `dst` turned out
// to be genuinely invalid partway through. A page that is valid but
// not yet backed is transparently demand-paged in and the copy
// continues -- the caller never sees that case as a failure.
uint64_t copy_to_user(void *dst, const void *src, uint64_t n);

// Same contract, opposite direction.
uint64_t copy_from_user(void *dst, const void *src, uint64_t n);
```

Implementation: a byte-at-a-time (or word-at-a-time with a byte tail,
matching §3.1's `blit_row` precedent) loop over `n`, each store/load
instruction wrapped in the `EX_TABLE_ENTRY` macro pointing at a local
fixup label. The fixup records how many bytes were completed before
the fault (already known: the loop's own index variable) and returns
that count, per the Linux `copy_to_user` convention noted above.

### 3.3 The page-fault dispatcher extension

In `kernel/arch/isr.c`'s page-fault handling (vector 14), the existing
structure is:

```c
if (regs->vector_number == 14) {
    // ... COW check (ring-3 write to a read-only present page) ...
    // ... vma_fault check, GATED ON (regs->cs & 3) == 3 (ring 3 ONLY) ...
}
if (regs->vector_number < 32) {
    // ring 0 (or unhandled ring 3) -> exception_dump_and_halt or SIGSEGV
}
```

A new check is inserted between the existing ring-3 `vma_fault` block
and the generic `< 32` fallthrough — specifically for vector 14,
`(regs->cs & 3) != 3` (ring 0), BEFORE the fallthrough halts the
machine:

1. Walk `__ex_table_start`..`__ex_table_end` for an entry whose
   `fault_addr == regs->rip`. Linear scan is fine — the table has one
   entry per copy-loop instruction, not one per syscall, so it stays
   small (tens of entries, not thousands).
2. No match → fall through to the existing `exception_dump_and_halt`
   path unchanged. This is the safety-preserving case: a kernel bug
   with a wild pointer that happens to land in user address space, at
   an instruction NOT marked recoverable, still halts loudly exactly
   as today.
3. Match found → call `vma_fault(current_proc(), cr2, write)`, with
   `write = (regs->error_code & 2) != 0` — the exact same derivation
   the existing ring-3 call site uses (`kernel/arch/isr.c:134`).
   `kernel/mm/vma.h`'s signature already takes exactly these generic
   arguments — nothing ring-3-specific is baked into `vma_fault`
   itself; the ring-3 gating lives in `isr.c`'s caller code, so no
   refactor is needed to reuse it here.
   - Returns success (VMA covered it, a frame is now installed) →
     `return` from the exception handler WITHOUT changing `regs->rip`
     — the faulting instruction retries automatically on IRET, now
     succeeding.
   - Returns failure (genuinely invalid address) → set
     `regs->rip = entry->fixup_addr` and return. The fixup landing pad
     (part of the copy loop's own asm) records the partial count and
     returns to `copy_to_user`/`copy_from_user`'s C caller normally.

### 3.4 Call site conversion

All 18 sites in §4's table get their raw dereference replaced with a
`copy_to_user`/`copy_from_user` call, translating the return value
(bytes NOT copied) into this kernel's existing `-EFAULT` convention
(non-zero return → `-EFAULT`, matching how these sites already report
a bad pointer today — no caller-visible behavior change on the error
path, only on the "valid but untouched" path, which goes from
incorrectly failing to correctly succeeding).

The already-guarded sites (`user_range_writable` + raw copy) are
migrated the same way, with their now-redundant pre-check removed —
the copy primitive's own fault handling supersedes it.

## 4. Site inventory

**Unguarded today (the actual crash fix — highest priority):**

| File | Site | Direction | Feeds |
|---|---|---|---|
| `sys_file.c` | `sys_write` | from-user (read) | — |
| `sys_file.c` | `sys_read` | to-user (write) | **confirmed crash site** |
| `sys_file.c` | `sys_getcwd` | to-user | — |
| `sys_file.c` | `stat_by_path` | to-user | `sys_stat`, `sys_lstat`, `sys_newfstatat` |
| `sys_file.c` | `sys_fstat` | to-user | — |
| `sys_file.c` | `rw_vectored` | both | `sys_readv`, `sys_writev` |
| `sys_file.c` | `sys_getdents` | to-user | — |
| `sys_misc.c` | `sys_clock_gettime` | to-user | — |
| `sys_misc.c` | `sys_nanosleep` | from-user | — |
| `sys_proc.c` | `sys_wait4` | to-user | — |
| `sys_proc.c` | `sys_thread_join` | to-user | — |
| `sys_signal.c` | `sys_rt_sigaction` | both | — |
| `sys_signal.c` | `sys_rt_sigprocmask` | both | — |
| `sys_signal.c` | `sys_rt_sigpending` | to-user | — |
| `sys_signal.c` | `sys_rt_sigsuspend` | from-user | — |
| `sys_signal.c` | `sys_rt_sigtimedwait` | both | — |
| `sys_signal.c` | `sys_rt_sigqueueinfo` | from-user | — |
| `sys_signal.c` | `sys_sigaltstack` | both | — |

**Already guarded (migrate for consistency, not urgency):**
`sys_pipe2`, `sys_uname`, `copy_user_argv`/`copy_user_vector`
(`sys_proc.c`), `sys_arch_prctl` (`ARCH_GET_FS`), `sys_getrandom`
(`sys_mem.c`), every handler in `sys_poll.c`, every handler in
`sys_net.c`/`socket.c`'s shared `addr_in`/`addr_out` helpers,
`futex_op` (`ipc/futex.c`).

## 5. Testing

- A new kernel selftest (`[uaccess] selftest passed`, added to
  `REQUIRED_MARKERS`) that: (a) copies into a small, already-touched
  stack buffer (baseline correctness), (b) `mmap`s a large anonymous
  region, immediately `copy_to_user`s into an offset past the first
  page without touching it first, and confirms the copy succeeds AND
  the destination reads back correctly, (c) attempts a copy to a
  deliberately unmapped/invalid address and confirms a clean
  `-EFAULT`-equivalent return with no machine halt.
- Re-run `./tools/gauntlet.sh` (15/15, matching this project's
  existing bar) to confirm no regression on any currently-passing
  syscall path.
- The actual regression test: re-run the `neoos-doom` ad hoc boot (the
  embedfs + custom-inittab setup already used to find this bug) and
  confirm the WAD load completes past `adding doom1.wad` without a
  page fault.

## 6. Migration ordering

1. `.ex_table` linker section + `struct exception_entry` + the
   `EX_TABLE_ENTRY` macro (no behavior change yet — nothing populates
   or reads it).
2. `copy_to_user`/`copy_from_user` implementations using it.
3. The page-fault dispatcher extension (§3.3) — the exception table
   has entries to find now.
4. The `[uaccess] selftest` (§5a/b/c) — proves the mechanism works in
   isolation before any real syscall depends on it.
5. Convert the 18 unguarded sites (§4), highest-crash-risk first:
   `sys_read`/`rw_vectored`/`sys_getdents`/`sys_write` (arbitrary-size
   buffers, the ones actually reachable by a large read like Doom's),
   then the smaller fixed-struct sites.
6. Migrate the already-guarded sites.
7. Gauntlet green, the neoos-doom regression re-run, docs.

This lands in `neoos-kernel` directly — it is a kernel-internal safety
mechanism, not a new userland-visible interface.
