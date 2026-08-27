# Static musl Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run statically-linked musl on NeoOS — `printf`, `malloc`, `__thread`, pthreads and `getrandom` — by building the Linux-shaped kernel primitives musl expects, vendoring musl, and remapping its syscall numbers onto NeoOS's.

**Architecture:** A per-process VMA list with demand paging gives `mmap`; the kernel builds a SysV auxv stack at process creation; `arch_prctl` and a per-thread FS base give TLS; `clone` is `fork`'s register plumbing with a caller-supplied stack; a ChaCha20 CSPRNG backs `getrandom` and `AT_RANDOM`. The shim is a pure rewrite of musl's `__NR_*` table — no runtime translation.

**Tech Stack:** C (gnu11, freestanding, `-mcmodel=kernel`), NASM, x86-64, musl 1.2.5, GRUB/Multiboot2, QEMU.

**Spec:** `docs/superpowers/specs/2026-08-28-musl-static-design.md`
**Roadmap:** `docs/superpowers/specs/2026-08-27-roadmap-architecture-design.md`

## Global Constraints

- **No host test runner.** Every verification is an in-kernel selftest or a userland program under headless QEMU, checked by grepping the serial log.
- **QEMU never exits on its own.** Always wrap it in `timeout`.
- **Fresh disk images every run.** `fat16_write_selftest` creates `/NEWDIR`, so a stale image reports `FAILED` on the second and later boots. `rm -f build/disk.img build/disk2.img && make disk-image`. This has produced false alarms five times in this project's history.
- **Never put `pgrep -f qemu-system` or `pkill -f qemu-system` in a command whose own command line contains that string** — the pattern matches the invoking shell, so `pkill` kills the command before it runs (exit 144) and `pgrep` reports a phantom leftover that is really itself. Use `ps ax -o pid,comm | grep -i qemu`.
- **Work happens directly on `main`** (`CLAUDE.md`).
- **The kernel stays `-mno-sse`.**
- **`docs/stdlib.md` is binding** (`CLAUDE.md`), but its role has changed: it now documents NeoOS-native calls and **deliberate divergences**, not a whole libc. musl documents itself.
- QEMU line: `-cpu Nehalem` for everything except the `RDRAND` path, which needs `-cpu Haswell`.

### Two facts established before writing this plan

1. **`mmap` needs six arguments and the ABI already carries them.**
   `syscall_dispatch` receives only `a1`–`a4`, but `struct syscall_frame`
   begins `r9, r8, ...` and `syscall_entry.asm` pushes those *before* its
   argument shuffle. So the 5th and 6th arguments are `frame->r8` and
   `frame->r9`. **Do not extend the syscall ABI.**
2. **musl's register convention is already NeoOS's.**
   `arch/x86_64/syscall_arch.h` uses `rax` for the number,
   `rdi/rsi/rdx/r10` for arguments, clobbering `rcx`/`r11`. Only the
   numbers differ.

### The "boot log UNCHANGED" check

Tasks 5 and 6 are refactors that must not change behavior. Filter the
timing-dependent lines and compare the **sorted multiset**:

```bash
filter() {
  grep -vE '^\[timer\] tick=|calibrated lapic|kmain address=|free_frames=|^\[(looper|yielder) pid=[0-9]+\] tick$' "$1" | sort
}
```

If the only difference is the pid of a run-time-forked process
(`sigtest`'s `check_segv` child takes whatever `next_id` holds at fork
time), verify it is only an id by comparing the exit-code multiset, the
`task exited` count, and the sorted results — all three identical plus
one shifted pid is explainable; anything else is not.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `third_party/musl/` | vendored musl 1.2.5 |
| `third_party/musl-README.md` | version, URL, sha256, list of local changes |
| `third_party/neoos-syscall.patch` | the `__NR_*` remap, kept separate from upstream |
| `tools/gen_syscall_map.py` | generates the remap from NeoOS's numbers |
| `kernel/mm/vma.h` / `vma.c` | per-process mappings, `mmap`/`munmap`/`mprotect`, fault fill |
| `kernel/random.h` / `random.c` | entropy pool + ChaCha20 CSPRNG |
| `kernel/futex.h` / `futex.c` | futex wait/wake hash |
| `userland/musltest.c` | the musl proof program |

**Modified:** `kernel/sched/proc.h`, `proc.c`, `thread.c`, `sched.c`, `kernel/elf.c`, `elf.h`, `kernel/isr.c`, `kernel/syscall.c`, `kernel/cpu.h`, `kernel/cpu.c`, `kernel/timer.c`, `kernel/fs/devfs.c`, `kernel/kernel.c`, `lib/crt0.asm`, `docs/stdlib.md`, `Makefile`.

---

## Task 1: Vendor musl and prove it builds

**Files:**
- Create: `third_party/musl/`, `third_party/musl-README.md`
- Modify: `Makefile`

**Interfaces:**
- Produces: `third_party/musl/lib/libc.a`, `crt1.o`, `crti.o`, `crtn.o`.

**This task is first for a reason.** Programs link at `0x200000000000`
with `-mcmodel=large`, because `user.ld` avoids the low 4GiB — that is
`PML4[0]`, the kernel's identity map, shared by every process. musl must
build in that configuration, which is not one it is routinely exercised
in. If it cannot, the milestone's shape changes, and that must be known
now rather than at task thirteen.

- [ ] **Step 1: Vendor the source**

The tarball is at `/tmp/musl-1.2.5.tar.gz` (sha256
`a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`,
verified by two independent downloads — **not** a signature check).

```bash
mkdir -p third_party/musl
tar xzf /tmp/musl-1.2.5.tar.gz -C third_party/musl --strip-components=1
```

Write `third_party/musl-README.md` recording: version 1.2.5, upstream
URL `https://musl.libc.org/releases/musl-1.2.5.tar.gz`, the sha256, how
integrity was checked, and a **list of local modifications** (initially
none; task 13 adds the syscall remap).

- [ ] **Step 2: Configure and build with the cross toolchain**

```bash
cd third_party/musl
./configure \
  --target=x86_64 \
  --disable-shared \
  --prefix=$(pwd)/../musl-install \
  CC=$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-gcc \
  CFLAGS="-mcmodel=large -fno-pic -mno-red-zone -O2"
make -j"$(nproc)" 2>&1 | tail -40
```

- [ ] **Step 3: Record what actually happened**

Three outcomes, and the plan branches on which:

- **Builds clean** → proceed; note it in the commit message.
- **Builds but with `-mcmodel=large` rejected somewhere** → try
  `CFLAGS="-mcmodel=large -fno-pic"` without `-O2`, then try building
  only the objects we need. Record exactly which files fail.
- **Cannot build large-model at all** → **stop and report.** Do not
  work around it by moving `user.ld` into the low 2GB: that collides
  with the kernel's identity map. The milestone needs re-planning, and
  the honest move is to say so.

- [ ] **Step 4: Confirm the archive is usable**

```bash
$HOME/opt/cross-x86_64-elf/bin/x86_64-elf-nm third_party/musl/lib/libc.a \
  | grep -c ' T '
ls -la third_party/musl/lib/libc.a third_party/musl/lib/crt1.o
```

Expected: a few thousand defined symbols and a `crt1.o` present.

- [ ] **Step 5: Add build artifacts to `.gitignore`, commit the source**

`third_party/musl/**/*.o`, `*.lo`, `lib/*.a`, `obj/`, `config.mak`.

```bash
git add third_party/musl third_party/musl-README.md .gitignore
git commit -m "Vendor musl 1.2.5 and prove it builds -mcmodel=large"
```

---

## Task 2: The VMA list and `mmap`/`munmap`/`mprotect`

**Files:**
- Create: `kernel/mm/vma.h`, `kernel/mm/vma.c`
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/syscall.c`

**Interfaces:**
- Produces: `struct vma`; `int64_t vma_mmap(struct process *, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags)`; `int vma_munmap(struct process *, uint64_t addr, uint64_t len)`; `int vma_mprotect(struct process *, uint64_t addr, uint64_t len, uint32_t prot)`; `int vma_fault(struct process *, uint64_t addr, int write)`; `void vma_destroy_all(struct process *)`.

- [ ] **Step 1: Create `kernel/mm/vma.h`**

```c
#ifndef NEOOS_VMA_H
#define NEOOS_VMA_H

#include <stdint.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// The gap between the ELF image (0x200000000000, see userland/user.ld)
// and the thread stacks (0x700000000000). Grows up.
#define MMAP_BASE  0x0000500000000000ULL
#define MMAP_LIMIT 0x0000600000000000ULL

struct vma {
    uint64_t    start, end;   // [start, end), page-aligned
    uint32_t    prot;
    uint32_t    flags;
    struct vma *next;         // list is sorted by start, non-overlapping
};

struct process;

int64_t vma_mmap(struct process *p, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags);
int     vma_munmap(struct process *p, uint64_t addr, uint64_t len);
int     vma_mprotect(struct process *p, uint64_t addr, uint64_t len, uint32_t prot);

// Populates one page for a not-present fault at `addr`. Returns 1 if a
// VMA covered it and a frame was mapped, 0 if the address belongs to no
// mapping (the caller then raises SIGSEGV) or the access is disallowed
// by the mapping's protection.
int  vma_fault(struct process *p, uint64_t addr, int write);

// Frees every mapping and every populated frame. Replaces the page-table
// walk in free_address_space.
void vma_destroy_all(struct process *p);

void vma_selftest(void);

#endif
```

- [ ] **Step 2: Add the fields to `struct process`**

```c
    struct vma *vmas;        // sorted by start, non-overlapping
    uint64_t    mmap_next;   // bump hint, starts at MMAP_BASE
```

Initialise both in `proc_alloc` (`vmas = 0; mmap_next = MMAP_BASE;`).

- [ ] **Step 3: Implement the list operations**

Write `vma.c` with: `vma_find(p, addr)` returning the covering mapping
or 0; `vma_insert(p, start, end, prot, flags)` keeping the list sorted
and merging adjacent mappings with identical `prot`/`flags`;
`vma_split(p, at)` splitting the mapping containing `at` into two.

`vma_mmap`:

```c
int64_t vma_mmap(struct process *p, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags) {
    if (len == 0) { return -EINVAL; }
    len = (len + 0xFFF) & ~0xFFFULL;

    if (flags & MAP_FIXED) {
        if (addr & 0xFFF) { return -EINVAL; }
        // Overlapping an existing mapping REPLACES it, as POSIX requires
        // and as the dynamic linker will later rely on.
        vma_munmap(p, addr, len);
    } else {
        addr = p->mmap_next;
        if (addr + len > MMAP_LIMIT) { return -ENOMEM; }
        p->mmap_next = addr + len;
    }
    if (!vma_insert(p, addr, addr + len, prot, flags)) { return -ENOMEM; }
    return (int64_t)addr;   // nothing is mapped yet -- see vma_fault
}
```

`vma_munmap` must **split**: unmapping the middle of a region leaves
two. For every page in the range that is actually present, unmap it and
free the frame via `paging_unmap_from(pml4, v, 1)`; pages never touched
have no frame and must not be freed. Getting this wrong leaks silently.

- [ ] **Step 4: Implement `vma_fault`**

```c
int vma_fault(struct process *p, uint64_t addr, int write) {
    struct vma *v = vma_find(p, addr);
    if (!v) { return 0; }                       // -> SIGSEGV
    if (!(v->prot & PROT_READ) && !(v->prot & PROT_WRITE)) { return 0; }
    if (write && !(v->prot & PROT_WRITE)) { return 0; }

    uint64_t frame = pmm_alloc(0);
    if (!frame) { return 0; }
    // Zero it here rather than calling sched/'s zero_frames: kernel/mm
    // must not reach into kernel/sched, and a fresh frame handed to
    // userland must never carry another process's bytes.
    uint64_t *z = (uint64_t *)phys_to_virt(frame);
    for (unsigned i = 0; i < PMM_FRAME_SIZE / sizeof(uint64_t); i++) { z[i] = 0; }

    uint64_t pf = PAGE_USER;
    if (v->prot & PROT_WRITE) { pf |= PAGE_WRITABLE; }
    if (!(v->prot & PROT_EXEC)) { pf |= PAGE_NO_EXECUTE; }

    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
    paging_map_into(pml4, addr & ~0xFFFULL, frame, pf);
    return 1;
}
```

- [ ] **Step 5: Add the three syscalls**

```c
#define SYS_MMAP     37
#define SYS_MUNMAP   38
#define SYS_MPROTECT 39
```

**`mmap` takes six arguments.** `a1`–`a4` are `addr`, `len`, `prot`,
`flags`; `fd` and `offset` are `frame->r8` and `frame->r9` — the ABI
already carries them, so do **not** extend `syscall_dispatch`.

```c
        case SYS_MMAP: {
            uint64_t addr = a1, len = a2;
            uint32_t prot = (uint32_t)a3, flags = (uint32_t)a4;
            int64_t  fd   = (int64_t)frame->r8;
            // Anonymous only this milestone; the dynamic linker adds
            // file-backed mappings when it needs them.
            if (!(flags & MAP_ANONYMOUS) || fd >= 0) { return -ENOSYS; }
            return vma_mmap(current_proc(), addr, len, prot, flags);
        }
```

- [ ] **Step 6: Add an in-kernel selftest**

`vma_selftest` runs as a kernel thread (like `waitq`'s) and checks, on
a scratch process-like structure or the current process: an unhinted
`mmap` returns an address in `[MMAP_BASE, MMAP_LIMIT)`; a second one
does not overlap; `munmap` of the middle of a region leaves two
mappings with the right bounds; `munmap` of a whole region leaves none;
`mmap` twice then `munmap` both returns the list to empty.

- [ ] **Step 7: Build and verify**

Standard single-model run. Expected `[vma] selftest passed`, zero
`FAILED`/exceptions, existing boot unchanged (nothing calls `mmap` yet).

- [ ] **Step 8: Commit**

```bash
git add kernel/mm/vma.h kernel/mm/vma.c kernel/sched/proc.h \
        kernel/sched/proc.c kernel/syscall.c kernel/kernel.c
git commit -m "Add per-process VMAs with mmap, munmap and mprotect"
```

---

## Task 3: Demand paging

**Files:**
- Modify: `kernel/isr.c`

- [ ] **Step 1: Consult the VMA list on a not-present fault**

In `isr_handler_inner`'s vector-14 path, **after** the COW check and
**before** the fault-to-signal mapping:

```c
        // Order matters: COW handles a write to a present read-only
        // page; this handles a first touch of a mapping that has no
        // frame yet. A fault matching neither is a genuine SIGSEGV.
        if (!(regs->error_code & 1)) {          // not present
            struct process *p = current_proc();
            if (p && p->pml4_phys &&
                vma_fault(p, cr2, (regs->error_code & 2) != 0)) {
                return;
            }
        }
```

- [ ] **Step 2: Prove it with a temporary test**

Temporarily add a userland program that `mmap`s a page, writes a
pattern across several pages, reads it back, and `munmap`s. Verify the
pattern survives and no exception occurs. Then extend it: `mmap` a
`PROT_READ` region and write to it — that must raise `SIGSEGV`, proving
the protection check rather than the happy path.

- [ ] **Step 3: Build, verify, commit**

```bash
git add kernel/isr.c
git commit -m "Populate mmap regions on first touch from the page-fault handler"
```

---

## Task 4: Fold stacks and ELF segments into the VMA list

**Files:**
- Modify: `kernel/sched/proc.c`, `kernel/mm/paging.c`, `kernel/elf.c`

Gated on the boot log.

- [ ] **Step 1: Capture the baseline.**

- [ ] **Step 2: Make `thread_stack_alloc` a `MAP_FIXED` mapping**

It keeps its slot bitmap (the layout is fixed by design), but records a
VMA and lets the fault handler populate pages. The guard page is simply
the absence of a mapping — which is now literally true rather than an
unmapped gap the handler happens to reject.

- [ ] **Step 3: Record ELF segments as VMAs**

`elf_load` maps segments eagerly (it must — it copies file contents
into them). It additionally records each as a VMA with the right
`prot`, so `free_address_space` and `munmap` see one representation.

- [ ] **Step 4: Replace `free_address_space`'s walk with `vma_destroy_all`**

Page tables themselves are still freed by the existing walk; the
*frames* now come from the VMA list.

- [ ] **Step 5: Verify the log is unchanged, then commit.**

---

## Task 5: `elf_load` reports the program headers

**Files:**
- Modify: `kernel/elf.h`, `kernel/elf.c`, `kernel/sched/proc.c`

- [ ] **Step 1: Extend `elf_load`'s output**

```c
struct elf_info {
    uint64_t entry;
    uint64_t phdr;      // VIRTUAL address the program headers landed at
    uint16_t phent;     // e_phentsize
    uint16_t phnum;     // e_phnum
};

int elf_load(const uint8_t *data, uint32_t size, uint64_t *pml4,
             struct elf_info *out);
```

`phdr` is the `PT_LOAD` segment containing `e_phoff`, giving
`p_vaddr + (e_phoff - p_offset)`. If no segment contains it, report 0 —
musl then finds no `PT_TLS` and uses its builtin TLS, which is correct
for a program that has none.

- [ ] **Step 2: Thread it through `build_user_address_space`, `spawn`, `exec_task`. Store it on `struct process`** (musl needs it at startup; `exec` replaces it).

- [ ] **Step 3: Build, verify unchanged, commit.**

---

## Task 6: The auxv startup stack

**Files:**
- Modify: `kernel/sched/proc.c`, `lib/crt0.asm`

Gated on the boot log — **every existing program links `crt0.o`**, so
they all exercise this immediately.

- [ ] **Step 1: Capture the baseline.**

- [ ] **Step 2: Build the initial stack**

Compose it in a **kernel buffer**, then copy into the top page of the
user stack through the physmap. Building it directly in the new address
space would mean switching `CR3`, which `spawn` must not do.

```c
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_ENTRY 9
#define AT_RANDOM 25

// Layout, high to low: argv[0] string, 16 random bytes, auxv (AT_NULL
// terminated), NULL, envp (empty), NULL, argv[0] ptr, argc.
// rsp must be 16-byte aligned at _start.
static uint64_t build_initial_stack(struct process *p, const char *path,
                                    uint64_t stack_top,
                                    const struct elf_info *info);
```

Entries written: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ` (4096),
`AT_BASE` (0 — no interpreter), `AT_ENTRY`, `AT_RANDOM` (pointer to the
16 bytes), `AT_NULL`.

Until task 14, fill the 16 bytes from `RDTSC`; task 14 switches them to
the CSPRNG and this plan's final regression checks that.

- [ ] **Step 3: Rewrite `lib/crt0.asm`**

```nasm
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv
    call main
    mov edi, eax
    call exit
```

Keep the existing comment about 16-byte alignment; it is now the
kernel's responsibility to deliver it, and the comment should say so.

- [ ] **Step 4: Verify the log is unchanged**

Every existing program now receives a real `argc`/`argv` and ignores
them. Any change here is a bug in the stack layout, not a behavior
change. Expected: `IDENTICAL`.

- [ ] **Step 5: Commit.**

---

## Task 7: `arch_prctl` and the per-thread FS base

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/sched.c`, `kernel/sched/proc.c`, `kernel/syscall.c`, `kernel/context_switch.asm`, `kernel/fork_trampoline.asm`

- [ ] **Step 1: Add `fs_base` and restore it on context switch**

`struct thread` gains `uint64_t fs_base;` (zero-initialised).
`schedule()` writes MSR `0xC0000100` for the incoming thread.

- [ ] **Step 2: Add the syscall**

```c
#define SYS_ARCH_PRCTL 40
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
```

`SET_FS` stores and writes the MSR immediately; `GET_FS` writes the
stored value to the user pointer. Other codes return `-EINVAL`.

- [ ] **Step 3: Fix the trampolines**

**`mov fs, dx` zeroes `FS_BASE`** — exactly as `mov gs, ax` zeroed
`GS_BASE` in the threads milestone. `fork_trampoline` resumes a child
through that instruction, so the child must have its `fs_base`
rewritten *after* the segment loads, or a forked child silently loses
its TLS pointer.

The cleanest fix keeps it in C: `schedule()` already writes the MSR for
the incoming thread, and a forked child is *scheduled* before it runs.
Confirm that ordering rather than assuming it — if the trampoline runs
without an intervening `schedule()`, the MSR write must move into the
trampoline path.

- [ ] **Step 4: Prove it**

A temporary userland test: `arch_prctl(ARCH_SET_FS, &block)`, read back
via `ARCH_GET_FS`, then read `%fs:0` after many `yield()`s and confirm
it still points at `block`. Two threads with different bases, to prove
it is per-thread.

- [ ] **Step 5: Commit.**

---

## Task 8: futex

**Files:**
- Create: `kernel/futex.h`, `kernel/futex.c`
- Modify: `kernel/syscall.c`

- [ ] **Step 1: Implement the hash**

```c
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define FUTEX_BUCKETS 64
```

A bucket array of `struct waitq`, keyed by hashing
`(process pid, user address)`. Private futexes are all musl uses, so a
per-process key is sufficient and shared futexes return `-ENOSYS`.

`FUTEX_WAIT`: validate the address is user-readable, **re-read the word
and compare to `val`** — if it differs, return `-EAGAIN` without
sleeping. That re-check is the entire point of the primitive: it closes
the race between userland's own check and the syscall. Then
`waitq_sleep` (or `waitq_sleep_timeout` if a timeout is supplied).

`FUTEX_WAKE`: wake up to `val` waiters on that key, return the count.

- [ ] **Step 2: Add the syscall (41) and a selftest**

Two kernel threads: one waits on an address, the other wakes it.
Additionally check that `FUTEX_WAIT` with a mismatched value returns
`-EAGAIN` immediately — the error path is the one that hangs the system
if wrong.

- [ ] **Step 3: Build, verify, commit.**

---

## Task 9: `set_tid_address`, `exit` versus `exit_group`

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/thread.c`, `kernel/syscall.c`

- [ ] **Step 1: Store `clear_child_tid`**

`struct thread` gains `uint64_t clear_child_tid;`.
`set_tid_address(ptr)` (syscall 42) stores it and returns the tid.

- [ ] **Step 2: Clear and wake at thread exit**

In `thread_exit_self`, before the thread becomes a zombie:

```c
    // How pthread_join blocks and wakes: musl waits on this word via
    // futex, and the kernel clearing it is the join's completion signal.
    if (t->clear_child_tid) {
        write 0 to that user address (if still mapped);
        futex_wake(proc, t->clear_child_tid, 1);
    }
```

The address is in the dying thread's own address space, which is still
live at this point because other threads hold the process.

- [ ] **Step 3: Record the exit mapping**

No new syscalls: the remap points musl's `__NR_exit` at NeoOS's
existing `SYS_THREAD_EXIT` (18) and `__NR_exit_group` at `SYS_EXIT`
(0). **Getting these backwards makes any thread's return kill the whole
process**, so task 15's generated table must be checked for exactly
this pair.

- [ ] **Step 4: Commit.**

---

## Task 10: `writev`, `readv`, `fstat`, `ioctl`, `clock_gettime`, `-ENOSYS`

**Files:**
- Modify: `kernel/syscall.c`, `kernel/fs/vfs.h`, `kernel/fs/devfs.c`

- [ ] **Step 1: `writev`/`readv` (43, 44)**

```c
struct iovec { void *iov_base; uint64_t iov_len; };
```

Loop over the existing read/write paths, accumulating the byte count.
stdio's real write path is `writev`, not `write`.

- [ ] **Step 2: `fstat` (45) with Linux's exact layout**

```c
struct k_stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, __pad0;
    uint64_t st_rdev, st_size;
    int64_t  st_blksize, st_blocks;
    int64_t  st_atime, st_atime_nsec;
    int64_t  st_mtime, st_mtime_nsec;
    int64_t  st_ctime, st_ctime_nsec;
    int64_t  __unused[3];
};   /* 144 bytes -- musl's struct stat must agree exactly */
```

`st_mode` from the vnode type: `S_IFREG 0100000`, `S_IFDIR 0040000`,
`S_IFCHR 0020000`. `st_size` from the vnode. `st_blksize` 4096 — stdio
sizes its buffer from it.

- [ ] **Step 3: `ioctl` (46)**

```c
#define TCGETS 0x5401
```

For a `VNODE_DEVICE` fd on `/dev/CONSOLE`, fill a `termios` and return
0; for anything else return `-ENOTTY`. **That single distinction is
what makes `isatty()` true** and gives line-buffered console output
instead of fully-buffered — without it, a musl program's `printf`
produces nothing until it exits.

- [ ] **Step 4: `clock_gettime` (47)**

`CLOCK_REALTIME` (0) and `CLOCK_MONOTONIC` (1) from `timer_ticks()` at
`TIMER_HZ`. Coarse by construction; say so in `docs/stdlib.md`.

- [ ] **Step 5: `default: return -ENOSYS;`**

Check what `syscall_dispatch`'s default currently returns and make it
`-ENOSYS`. musl probes for features and falls back when a syscall is
missing — **but only if it receives `-ENOSYS`** rather than a wrong
answer.

- [ ] **Step 6: Commit.**

---

## Task 11: `clone`

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/thread.c`, `kernel/syscall.c`

**Interfaces:**
- Produces: `struct thread *thread_clone(uint64_t flags, uint64_t child_stack, uint64_t tls, struct syscall_frame *frame)`.

- [ ] **Step 1: Understand what the child needs**

musl's `arch/x86_64/clone.s` pushes the entry point and argument onto
the new stack *before* the syscall, then in the child returns from the
syscall with `rax == 0`, pops, and calls. **So the kernel needs no
entry-point concept**: a clone child is a copy of the caller's syscall
frame with `user_rsp` set to the supplied stack and `rax` forced to 0 —
`fork_task`'s register plumbing, sharing the process rather than
duplicating it.

- [ ] **Step 2: Add caller-supplied stacks to threads**

`thread_create` allocates from the slot bitmap; musl bypasses that.
Threads gain a mode where the stack is supplied and `stack_slot` is
`-1`, meaning `thread_join` and the reaper have nothing to unmap.

- [ ] **Step 3: Implement `clone` (syscall 48)**

Argument order on x86-64 is `(flags, stack, ptid, ctid, tls)`, so
`tls` is `frame->r8`.

```c
#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000
#define CLONE_SETTLS  0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
```

Require `CLONE_VM | CLONE_THREAD`; anything else returns `-EINVAL`
rather than being silently approximated — that is what stops `clone`
becoming a half-working `fork`.

Then: allocate a thread in the *current* process; copy the caller's
frame; set `user_rsp = child_stack`, `rax = 0`; if `CLONE_SETTLS`, set
`fs_base = tls`; if `CLONE_PARENT_SETTID`, write the new tid to `ptid`;
if `CLONE_CHILD_CLEARTID`, store `ctid`. Enqueue and return the tid.

- [ ] **Step 4: Prove it before musl exists**

A libneoos test calling `clone` directly with an `mmap`ed stack, a
trivial entry, and `CHILD_CLEARTID`, then futex-waiting on the ctid.
That exercises the whole pthread mechanism without musl in the picture
— which is the point.

- [ ] **Step 5: Commit.**

---

## Task 12: Entropy — pool and ChaCha20

**Files:**
- Create: `kernel/random.h`, `kernel/random.c`
- Modify: `kernel/timer.c`, `kernel/keyboard.c`, `kernel/cpu.c`, `kernel/kernel.c`

- [ ] **Step 1: Implement ChaCha20**

The block function only, per RFC 8439 section 2.3. State is 16 words:
four constants `"expand 32-byte k"`, eight key words, one counter,
three nonce words.

```c
#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d)                       \
    a += b; d ^= a; d = ROTL32(d, 16);       \
    c += d; b ^= c; b = ROTL32(b, 12);       \
    a += b; d ^= a; d = ROTL32(d, 8);        \
    c += d; b ^= c; b = ROTL32(b, 7)

static void chacha20_block(const uint32_t key[8], uint32_t counter,
                           const uint32_t nonce[3], uint8_t out[64]) {
    static const uint32_t c[4] = { 0x61707865, 0x3320646e,
                                   0x79622d32, 0x6b206574 };
    uint32_t s[16], x[16];
    s[0]=c[0]; s[1]=c[1]; s[2]=c[2]; s[3]=c[3];
    for (int i = 0; i < 8; i++) { s[4 + i] = key[i]; }
    s[12] = counter; s[13] = nonce[0]; s[14] = nonce[1]; s[15] = nonce[2];
    for (int i = 0; i < 16; i++) { x[i] = s[i]; }

    for (int i = 0; i < 10; i++) {          /* 20 rounds = 10 double */
        QR(x[0], x[4], x[ 8], x[12]);  QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);  QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);  QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);  QR(x[3], x[4], x[ 9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[i*4+0] = (uint8_t)v;         out[i*4+1] = (uint8_t)(v >> 8);
        out[i*4+2] = (uint8_t)(v >> 16); out[i*4+3] = (uint8_t)(v >> 24);
    }
}
```

- [ ] **Step 2: The pool**

```c
// Raw pool bytes are NEVER handed out. The pool rekeys the stream and
// the stream produces output, which is what makes an unaccounted pool
// acceptable: an observer of the output learns nothing about the pool.
static uint8_t pool[32];
static uint64_t pool_writes;
```

`random_add_entropy(const void *, uint32_t)` folds bytes into the pool
(rotate-and-xor accumulate is sufficient — this is mixing, not
hashing). Called from:

- **`timer_handler`** with the low bits of `RDTSC` — cheap and runs
  100 times a second.
- **`keyboard_handler`** with `RDTSC` — genuinely unpredictable timing.
- **`RDRAND`/`RDSEED`** at init and on each rekey, where present.

- [ ] **Step 3: Detect `RDRAND`/`RDSEED`**

`CPUID.1:ECX[30]` and `CPUID.7:EBX[18]`. **Haswell has them; Nehalem
does not**, so the same runtime-detection pattern as AVX applies, and
the fallback is what the standard boot exercises. Report which at boot,
as `cpu_init` does for XSAVE.

- [ ] **Step 4: `random_bytes(void *buf, uint32_t len)`**

Rekey the ChaCha state from the pool (plus `RDRAND` where available)
every N bytes or M pool writes, then emit stream bytes.

- [ ] **Step 5: `getrandom` (49), `/dev/random`, `/dev/urandom`**

`getrandom(buf, len, flags)` ignores `GRND_RANDOM` (no blocking) and
honours nothing else. Add both devfs nodes to `devfs.c`'s static table;
read yields CSPRNG output, write mixes into the pool.

- [ ] **Step 6: Point `AT_RANDOM` at it**

Replace task 6's `RDTSC` fill with `random_bytes(..., 16)`.

- [ ] **Step 7: Selftest and verification**

In-kernel: two `random_bytes` calls of 64 bytes must differ, and
neither may be all-zero. Userland: `getrandom` twice, compare. Under
`-cpu Haswell`, confirm the `RDRAND`-present path is reported.

**State the limitation in the boot log and the docs**: no entropy
estimation, no blocking, `/dev/random` behaves as `/dev/urandom`, and
early-boot output without `RDRAND` is weak.

- [ ] **Step 8: Commit.**

---

## Task 13: The `__NR_*` remap

**Files:**
- Create: `tools/gen_syscall_map.py`, `third_party/neoos-syscall.patch`
- Modify: `third_party/musl/arch/x86_64/bits/syscall.h.in`, `third_party/musl-README.md`

- [ ] **Step 1: Write the generator**

`tools/gen_syscall_map.py` holds the mapping from musl's Linux names to
NeoOS numbers and rewrites `syscall.h.in`:

```python
NEOOS = {
    "read": 6, "write": 1, "open": 7, "close": 8, "lseek": 11,
    "mmap": 37, "munmap": 38, "mprotect": 39,
    "rt_sigaction": 21, "rt_sigprocmask": 22, "rt_sigreturn": 23,
    "ioctl": 46, "writev": 43, "readv": 44, "fstat": 45,
    "clone": 48, "exit": 18, "exit_group": 0,
    "wait4": 32, "kill": 29, "tkill": 30, "tgkill": 31,
    "futex": 41, "set_tid_address": 42, "arch_prctl": 40,
    "clock_gettime": 47, "getrandom": 49, "getpid": 3,
    "sigaltstack": 28, "rt_sigsuspend": 25, "rt_sigpending": 24,
    "rt_sigtimedwait": 26, "getdents64": 16, "mkdir": 9, "unlink": 10,
    "setpgid": 33, "getpgid": 34, "setsid": 35, "getsid": 36,
}
SENTINEL_BASE = 0x8000
```

Every entry not in `NEOOS` becomes `SENTINEL_BASE + linux_nr`.

**The sentinel matters as much as the mapping.** Linux's `__NR_stat` is
4, which is NeoOS's `SYS_SPAWN`; leaving it would not fail, it would
silently spawn a process. Every unmapped number must land out of range
so it reaches the `-ENOSYS` default from task 10.

- [ ] **Step 2: Sanity-check the generated table**

Assert in the generator: no two names map to the same NeoOS number
except where NeoOS genuinely shares one; `exit` maps to 18 and
`exit_group` to 0 and **not** the reverse; every NeoOS number used is
one `syscall_dispatch` actually implements.

- [ ] **Step 3: Keep the diff as a patch**

```bash
cd third_party/musl && git diff --no-index arch/x86_64/bits/syscall.h.in{.orig,} \
  > ../neoos-syscall.patch
```

Record it in `musl-README.md` as the only local modification, with the
command to regenerate it.

- [ ] **Step 4: Rebuild musl and commit.**

---

## Task 14: Build musl programs from the Makefile

**Files:**
- Modify: `Makefile`
- Create: `userland/musltest.c` (minimal at first)

- [ ] **Step 1: Add a musl link rule**

```makefile
MUSL := third_party/musl
MUSL_CFLAGS := -mcmodel=large -fno-pic -mno-red-zone -static -nostdinc \
               -isystem $(MUSL)/include -isystem $(MUSL)/arch/x86_64 \
               -isystem $(MUSL)/arch/generic -O2 -Wall

$(USERLAND_BUILD)/MUSLTEST.ELF: $(USERLAND_DIR)/musltest.c $(MUSL)/lib/libc.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(MUSL_CFLAGS) -T $(USERLAND_DIR)/user.ld -nostdlib -o $@ \
	  $(MUSL)/lib/crt1.o $(MUSL)/lib/crti.o $(USERLAND_DIR)/musltest.c \
	  $(MUSL)/lib/libc.a $(MUSL)/lib/crtn.o
```

The exact include and link order will need adjusting against what task
1 actually produced; treat the above as the shape, not gospel.

- [ ] **Step 2: The first program does the least possible**

```c
int main(void) {
    // Raw write, no stdio: proves the auxv stack, arch_prctl, and the
    // shim's number mapping, and NOTHING else.
    const char msg[] = "[musltest] raw write works\n";
    __asm__ volatile ("syscall" :: "a"(1L), "D"(1L), "S"(msg),
                                   "d"(sizeof(msg) - 1)
                                : "rcx", "r11", "memory");
    return 0;
}
```

If this prints, musl's startup ran: `_start` → `__libc_start_main` →
`__init_libc` (auxv walk) → `__init_tls` (`arch_prctl`) → `main`.

- [ ] **Step 3: Add it to the disk image and spawn it. Verify.**

**If it hangs or faults, do not debug through the full boot.** Spawn
only `MUSLTEST` and nothing else — the extended-state milestone showed
that turns a 150-second cycle into 30 seconds.

- [ ] **Step 4: Commit.**

---

## Task 15: musl `printf`, `malloc`, `__thread`

**Files:**
- Modify: `userland/musltest.c`

- [ ] **Step 1: `printf`**

Proves `writev`, `fstat`, `ioctl(TCGETS)` and line buffering together.
If output appears only at exit, `isatty()` is false — check `ioctl`.

- [ ] **Step 2: `malloc`**

Allocate and free across sizes spanning `mallocng`'s classes (16 bytes
to several pages), write a pattern into each, read it back. Proves
`mmap`, demand paging and `munmap`.

- [ ] **Step 3: `__thread`**

A thread-local counter, incremented across many `sched_yield()` calls,
proving the FS base survives context switches.

- [ ] **Step 4: Verify and commit.**

---

## Task 16: musl pthreads

**Files:**
- Modify: `userland/musltest.c`

- [ ] **Step 1: `pthread_create` / `pthread_join`**

Four threads, each incrementing a shared counter under a
`pthread_mutex_t` and its own `__thread` counter. Join all four, check
both totals.

This exercises `clone` with a caller-supplied `mmap`ed stack,
`CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_CLEARTID`, futex
wait/wake, and `SYS_exit` versus `SYS_exit_group` — the whole chain.

- [ ] **Step 2: If `pthread_join` hangs**

That is `CHILD_CLEARTID` or the futex wake, in that order. Check the
kernel writes 0 to the ctid address *and* wakes the futex on it.

- [ ] **Step 3: `getrandom` through musl**

Two calls, compare, confirm they differ.

- [ ] **Step 4: Verify and commit.**

---

## Task 17: Documentation

**Files:**
- Modify: `docs/stdlib.md`

- [ ] **Step 1: Reframe the document**

Its role has changed: it documents **NeoOS-native calls and deliberate
divergences**, not a whole libc. Add a preamble saying musl provides
the standard C library and documents itself, and that this file covers
what NeoOS adds or does differently.

- [ ] **Step 2: Record the divergences**

- `wait` is NeoOS-native beside POSIX `wait4`.
- `spawn` has no POSIX analogue.
- `brk` returns `-ENOSYS`; allocation goes through `mmap`.
- `mmap` is anonymous-only; no file-backed mappings yet.
- `/dev/random` does not block and is not accounted; early-boot output
  without `RDRAND` is weak.
- `clock_gettime` is 100Hz-coarse.
- Unimplemented syscalls return `-ENOSYS` by design, and musl's
  fallbacks depend on it.

- [ ] **Step 3: Commit.**

---

## Task 18: Leak gate and final regression

- [ ] **Step 1: Leak gate at 5 and 10 iterations**

Spawn `MUSLTEST` repeatedly from a kernel thread, comparing
`free_frames` and `vfs_vnode_in_use_count()`.

**`wait_for_pid` must stay parentage-free** — the signals milestone
found that routing it through `wait4` makes a kernel-thread gate spawn
everything concurrently and measure nothing.

`mmap` adds a whole new class of per-process allocation, and pthreads
add caller-supplied stacks that the kernel must *not* try to free. The
delta must be **identical** at 5 and 10; the threads milestone's
apparent "fixed cost" of 7 frames was in fact a per-iteration leak that
only the 10-iteration run exposed.

- [ ] **Step 2: Final regression**

Every criterion, on `-cpu Nehalem` and `-cpu Haswell`:

- every prior selftest: `pmm`, `paging`, `lock`, `heap`, `fat16`,
  `fat16 write`, `vfs`, `waitq`, `signal`, `cpu state`, plus the new
  `vma`, `futex` and `random`
- `[musltest] ALL PASSED` with raw write, `printf`, `malloc`,
  `__thread`, pthreads and `getrandom`
- `[vfstest]`, `[threadtest]`, `[sigtest]`, `[avxtest]` all passing —
  the `crt0` rewrite touched every one of them
- `faulter` dying alone; **zero `exception` lines**
- zero `FAILED`

---

## Notes for the implementer

- **Task 1 can end this milestone.** If musl will not build
  `-mcmodel=large`, say so and stop rather than working around it by
  moving `user.ld` into the low 4GiB — that region is the kernel's
  identity map, shared by every process.
- **`mov fs, dx` zeroes `FS_BASE`.** The threads milestone lost a long
  debugging session to the GS version of this. Assume the FS version
  will bite in `fork`.
- **`exit` and `exit_group` must not be swapped.** Backwards, every
  thread's return kills the process, and it will look like a scheduler
  bug.
- **`ioctl(TCGETS)` decides whether `printf` appears at all.** Without
  it `isatty()` is false, stdio buffers fully, and a short program
  produces no output before exiting — which reads as a hang.
- **Debug musl failures with a shrunk boot**, spawning only the musl
  program. This turned a 150-second cycle into 30 seconds last
  milestone and was the difference between guessing and knowing.
- **Fresh disk images every run**; **never** `pkill -f qemu-system`
  inside a command that mentions it.
