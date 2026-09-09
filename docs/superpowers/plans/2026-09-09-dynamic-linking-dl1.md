# Dynamic Linking (DL-1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A hello-world built **without** `-static` runs on NeoOS, and a
`dlopen`/`dlsym`/`dlclose` round trip against a NeoOS-built `.so`
succeeds.

**Architecture:** The kernel performs **no relocation**. It loads two
ELF objects instead of one — the executable and the interpreter named
by `PT_INTERP` — describes them accurately in the auxiliary vector
(`AT_PHDR`/`AT_ENTRY` for the executable, `AT_BASE` for the
interpreter), and jumps to the interpreter. Everything after that —
mapping libraries, relocation, symbol resolution, `dlopen`, TLS
layout — is musl's `ldso/dynlink.c`, which already implements it
correctly and is already NeoOS's C library.

**Tech Stack:** C11 freestanding kernel; musl built shared (its
`libc.so` *is* the dynamic linker, installed as
`ld-musl-x86_64.so.1`); `x86_64-neoos-linux-musl-gcc` 9.4.0; QEMU
headless with serial capture for all verification.

**Spec:** `docs/superpowers/specs/2026-09-09-dynamic-linking-design.md`

## Global Constraints

- **No host tests exist and none can.** This is bare-metal code. Every
  task is verified by building, booting headless in QEMU, and grepping
  the serial log for a marker. The TDD cycle is: write the test so its
  marker is *absent* (or a `FAILED:` marker appears), implement, boot
  again, see `passed`.
- Verification one-liner, used verbatim in "run" steps below:

  ```bash
  cd ~/projects/personal/NeoOS && \
  make clean-kernel iso disk-image QUIET=1 2>&1 | grep -iE "error:|Error [0-9]" ; \
  timeout 150 qemu-system-x86_64 -cpu Nehalem -smp 4 -boot order=d \
    -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
    -drive file=build/disk2.img,format=raw -vga std \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -audiodev none,id=a0 -device AC97,audiodev=a0,addr=0x6 \
    -no-reboot -display none -serial file:build/serial.dl.log > /dev/null 2>&1 ; \
  grep -nE "\[vma\]|\[elf\]|\[dyn\]|PANIC|\[exception\]" build/serial.dl.log
  ```
- **`tools/gauntlet.sh 15 3` is the acceptance bar for every task that
  touches the fault path** (Tasks 2–4), not only the last. A
  lost-wakeup or a missed re-validation shows up as a rare hang, not a
  failure. The gauntlet needs an **exclusive build** — any concurrent
  `make` kills it silently while appearing to still run.
- **Syscall numbers are NeoOS's own.** `SYS_MAX` is currently 136.
  This plan adds exactly one: `SYS_PREAD 136`, moving `SYS_MAX` to 137.
- **Everything crossing into userland is Linux-shaped.** Struct
  layouts, flag values and errno numbers match Linux x86-64. Exact
  values are given in each task — copy them, do not invent them.
- **Rebuild `neoos-musl` AND copy the result into the cross toolchain
  sysroot** (`~/opt/cross-x86_64-neoos/x86_64-neoos-linux-musl/lib/`).
  There are TWO musl installs; `--sysroot` picks up one and the
  toolchain's default picks up the other. Forgetting the second
  produces a phantom `[shim] ENOSYS` and has already cost time.
- Work happens directly on `main`. No feature branches.

## File Structure

**Kernel, modified:**
- `kernel/syscall/syscall_nr.h`, `syscall.c`, `syscall_internal.h`,
  `sys_file.c` — `pread`.
- `kernel/mm/vma.h` — `struct vma` gains the file-backing fields; new
  `VMA_FILE` flag.
- `kernel/mm/vma.c` — `vma_mmap_file`, and the fault-path restructure.
  This is the file that matters.
- `kernel/syscall/sys_mem.c` — `sys_mmap` stops refusing non-anonymous.
- `kernel/elf.c` / `elf.h` — `PT_INTERP`, `ET_DYN`, load-at-base.
- `kernel/sched/proc.c` — load the interpreter, push `AT_BASE`.

**Userland, created:**
- `userland/dyntest.c` — the milestone: built **without** `-static`.
- `userland/dynlib.c` — a tiny `.so` for the `dlopen` round trip.
- `userland/mmapfile.c` — file-mapping conformance, including the
  concurrent-fault and short-read cases.

**Outside this repo:**
- `neoos-musl/build.sh` — `--enable-shared`.

---

## Task 1: `pread`

`ldso/dynlink.c` reads ELF headers with `pread`. It is the smaller of
the two missing syscalls and independent of everything else, so it goes
first and gets the harness built.

**Files:**
- Modify: `kernel/syscall/syscall_nr.h`, `syscall.c`,
  `syscall_internal.h`, `kernel/syscall/sys_file.c`
- Modify: `third_party/shim/neoos_syscall.c`
- Create: `userland/mmapfile.c` (grows across Tasks 1–4)
- Modify: `Makefile` (a `mmapfile` target)

**Interfaces:**
- Produces: `int64_t sys_pread(struct syscall_args *a)` —
  `pread(fd, buf, count, offset)`. Reads at an absolute offset
  **without moving the file position**, which is the whole point.
  Returns bytes read, 0 at EOF, `-EBADF`, `-EINVAL` for a negative
  offset, `-ESPIPE` for an object with no position (pipe, socket).

- [ ] **Step 1: Write the failing test**

Create `userland/mmapfile.c`, modelled on `userland/uxtest.c` (raw
syscalls, NeoOS's own numbers, so a shim bug cannot mask a kernel bug):

```c
static void test_pread(void) {
    const char *path = "/tmp/dl-pread";
    (void)neo_unlink(path);
    long fd = neo_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open");
    if (fd < 0) { return; }

    static char src[512];
    for (int i = 0; i < 512; i++) { src[i] = (char)(i & 0x7F); }
    check(neo3(SYS_WRITE, fd, src, sizeof src) == sizeof src, "seed write");

    // The position is at EOF after that write. pread must ignore it
    // entirely -- that is what distinguishes it from read.
    static char buf[64];
    check(neo4(SYS_PREAD, fd, buf, 64, 100) == 64, "pread returns 64");
    int ok = 1;
    for (int i = 0; i < 64; i++) { if (buf[i] != (char)((100 + i) & 0x7F)) { ok = 0; } }
    check(ok, "pread read from the right offset");

    // ...and must NOT have moved the position: a read() here is at EOF.
    check(neo3(SYS_READ, fd, buf, 64) == 0, "pread did not move the position");

    check(neo4(SYS_PREAD, fd, buf, 64, 1000) == 0, "pread past EOF is 0");
    check(neo4(SYS_PREAD, fd, buf, 64, -1) == -22, "negative offset is -EINVAL");

    (void)neo1(SYS_CLOSE, fd);
    (void)neo_unlink(path);
    printf("[dyn] pread ok\n");
}
```

Add a `mmapfile` target to the `Makefile`, copying the `uxtest` target
verbatim and substituting the name — its own `INITTAB`, boots alone,
greps for `[dyn] ALL PASSED`.

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make mmapfile
```
Expected: `[dyn] FAILED: pread returns 64` — the syscall does not exist,
so the dispatch table returns `-ENOSYS`.

- [ ] **Step 3: Implement**

`syscall_nr.h`:

```c
// ldso/dynlink.c reads ELF headers at absolute offsets without
// disturbing the file position.
#define SYS_PREAD           136

// One past the highest number in use. The dispatch table is this long.
#define SYS_MAX             137
```

`sys_file.c`:

```c
// pread(fd, buf, count, offset). Distinct from read in exactly one
// way that matters: the file position is untouched, so a caller can
// read headers from anywhere in a file another thread is also reading.
int64_t sys_pread(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    if ((int64_t)a->a4 < 0) { return -EINVAL; }
    if (!f->vn) { return -ESPIPE; }         // pipe, socket, tty: no position
    if (f->vn->type == VNODE_DIR) { return -EISDIR; }

    uint32_t saved = f->position;
    f->position = (uint32_t)a->a4;
    int64_t rc = file_read(f, (void *)(uintptr_t)a->a2, a->a3);
    f->position = saved;
    return rc;
}
```

Add the declaration to `syscall_internal.h` and the dispatch entry
`[SYS_PREAD] = { sys_pread, "pread" }` to `syscall.c`.

Shim: `#define NEO_PREAD 136`, `#define LX_PREAD64 17`, and
`case LX_PREAD64: return neo(NEO_PREAD, a1, a2, a3, a4, 0, 0);`

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make mmapfile
```
Expected: `[dyn] pread ok`.

- [ ] **Step 5: Commit**

```bash
git add kernel/ third_party/shim/ userland/mmapfile.c Makefile
git commit -m "syscall: pread

musl's dynamic linker reads ELF headers at absolute offsets without
disturbing the file position, which read cannot do. Saving and
restoring f->position around file_read is the whole implementation;
the tests pin down that the position really is untouched, which is the
only thing distinguishing this from read."
```

---

## Task 2: File-backed VMAs, eagerly populated

Demand paging needs the fault-path restructure, and doing both at once
means two hard things failing together. So this task adds the *mapping*
with pages populated at `mmap` time, and Task 3 makes it lazy. After
this task file mappings are correct but eager; after Task 3 they are
correct and lazy. The tests written here do not change.

**Files:**
- Modify: `kernel/mm/vma.h` (the `VMA_FILE` flag and the backing fields)
- Modify: `kernel/mm/vma.c` (`vma_mmap_file`)
- Modify: `kernel/syscall/sys_mem.c` (`sys_mmap` accepts a file fd)
- Modify: `userland/mmapfile.c`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `#define VMA_FILE 0x1000` in `vma.h`, alongside the existing
    `VMA_PHYS`.
  - `struct vma` gains `struct vnode *vn; uint64_t file_off;` — `vn`
    holds a reference for the VMA's whole life, `file_off` is the file
    offset of `start`.
  - `int64_t vma_mmap_file(struct process *p, uint64_t addr,
    uint64_t len, uint32_t prot, uint32_t flags, struct vnode *vn,
    uint64_t off)` — returns the mapped address or a negative errno.
    Takes its own reference on `vn`; the caller keeps theirs.

- [ ] **Step 1: Write the failing test**

Append to `userland/mmapfile.c`:

```c
static void test_map_file(void) {
    const char *path = "/tmp/dl-map";
    (void)neo_unlink(path);
    long fd = neo_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open");
    if (fd < 0) { return; }

    // Two pages plus a bit, so the short-read case at the end is real.
    static char src[8192 + 100];
    for (unsigned i = 0; i < sizeof src; i++) { src[i] = (char)((i * 7) & 0xFF); }
    check(neo3(SYS_WRITE, fd, src, sizeof src) == (long)sizeof src, "seed write");

    long a = neo6(SYS_MMAP, 0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(a > 0, "mmap MAP_PRIVATE of a file");
    if (a <= 0) { return; }
    const volatile unsigned char *p = (const volatile unsigned char *)a;

    check(p[0]    == (unsigned char)0,           "first byte");
    check(p[4095] == (unsigned char)((4095*7) & 0xFF), "last byte of page 0");
    check(p[4096] == (unsigned char)((4096*7) & 0xFF), "first byte of page 1");
    check(p[8191] == (unsigned char)((8191*7) & 0xFF), "last byte of page 1");

    // MAP_PRIVATE: a write is private. It must not reach the file, and
    // a fresh mapping must not see it.
    volatile unsigned char *w = (volatile unsigned char *)a;
    w[10] = 0xEE;
    check(w[10] == 0xEE, "private write took");
    long b = neo6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    check(b > 0, "second mapping");
    if (b > 0) {
        check(((const volatile unsigned char *)b)[10] == (unsigned char)(10*7 & 0xFF),
              "private write did not leak to a second mapping");
        (void)neo2(SYS_MUNMAP, b, 4096);
    }
    static char vfy[16];
    check(neo4(SYS_PREAD, fd, vfy, 16, 0) == 16, "read the file back");
    check(vfy[10] == (char)((10*7) & 0xFF), "private write did not reach the file");

    // A non-page-aligned offset is -EINVAL, as on Linux.
    check(neo6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 100) == -22,
          "unaligned offset is -EINVAL");

    // Mapping past EOF: the tail beyond the file reads as zeros.
    long c = neo6(SYS_MMAP, 0, 8192, PROT_READ, MAP_PRIVATE, fd, 8192);
    check(c > 0, "map the last partial page");
    if (c > 0) {
        const volatile unsigned char *q = (const volatile unsigned char *)c;
        check(q[99]  == (unsigned char)(((8192+99)*7) & 0xFF), "last real byte");
        int zeros = 1;
        for (int i = 100; i < 4096; i++) { if (q[i] != 0) { zeros = 0; break; } }
        check(zeros, "beyond EOF reads as zeros");
        (void)neo2(SYS_MUNMAP, c, 8192);
    }

    (void)neo2(SYS_MUNMAP, a, 8192);
    (void)neo1(SYS_CLOSE, fd);
    (void)neo_unlink(path);
    printf("[dyn] map file ok\n");
}
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make mmapfile
```
Expected: `[dyn] FAILED: mmap MAP_PRIVATE of a file` — `sys_mmap`
returns `-ENOSYS` for a non-anonymous mapping today.

- [ ] **Step 3: Implement**

`vma.h`:

```c
// A file-backed private mapping. The frames belong to this process
// (unlike VMA_PHYS, which borrows device frames), so they are freed on
// unmap; `vn` holds a reference for the VMA's whole life so the file
// cannot be freed while the mapping exists.
#define VMA_FILE 0x1000
```

and in `struct vma`:

```c
    struct vnode *vn;         // VMA_FILE only; reference held
    uint64_t      file_off;   // file offset corresponding to `start`
```

Every existing `vma_insert` call site must set `vn = 0` — a garbage
pointer here is a `vnode_put` of nonsense at unmap time.

`vma.c`, populating eagerly for now:

```c
// Read one page of `vn` at `off` into `frame`, zero-filling any part
// that lies past the end of the file. POSIX requires the tail of a
// mapping beyond EOF to read as zeros, and a .bss at the end of a data
// segment depends on exactly that.
static int vma_fill_page_from_file(struct vnode *vn, uint64_t off, uint64_t frame) {
    uint8_t *dst = (uint8_t *)phys_to_virt(frame);
    for (unsigned i = 0; i < PMM_FRAME_SIZE; i++) { dst[i] = 0; }
    if (!vn->mount || !vn->mount->ops->read) { return -EIO; }
    int64_t n = vn->mount->ops->read(vn, (uint32_t)off, dst, PMM_FRAME_SIZE);
    return n < 0 ? (int)n : 0;
}
```

`vma_mmap_file` inserts the VMA with `VMA_FILE`, takes a reference on
`vn`, and — in this task only — allocates and fills every page up
front, mapping each with `paging_map_into`.

`vma_munmap` and `vma_destroy_all` must `vnode_put(v->vn)` for a
`VMA_FILE` VMA. `vma_copy_all` (fork) must take another reference for
the child.

`sys_mem.c`: when `fd >= 0` and the object has no `file_ops.mmap`, fall
through to `vma_mmap_file` with the fd's vnode rather than returning
the device-hook error:

```c
    if (fd >= 0) {
        struct file_descriptor *f = fd_get(current_proc(), (int)fd);
        if (!f) { return -EBADF; }
        if (f->ops && f->ops->mmap) {
            struct mmap_req req = { addr, len, prot, flags, (uint64_t)a->frame->r9, 0 };
            int64_t rc = file_mmap(f, &req);
            return rc < 0 ? rc : (int64_t)req.out_addr;
        }
        // A plain file. MAP_SHARED needs a page cache that does not
        // exist yet; MAP_PRIVATE is what a dynamic linker uses.
        if (!f->vn) { return -ENODEV; }
        if (flags & MAP_SHARED) { return -ENOSYS; }
        uint64_t off = (uint64_t)a->frame->r9;
        if (off & (PMM_FRAME_SIZE - 1)) { return -EINVAL; }
        return vma_mmap_file(current_proc(), addr, len, prot, flags, f->vn, off);
    }
```

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make mmapfile
```
Expected: `[dyn] map file ok`.

- [ ] **Step 5: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15. Nothing else may be building concurrently.

- [ ] **Step 6: Commit**

```bash
git add kernel/ userland/mmapfile.c
git commit -m "mm: file-backed private mappings, eagerly populated

sys_mmap refused anything non-anonymous outside the /dev/fb0 and memfd
device hooks; its own comment already anticipated this work. A VMA can
now be backed by a vnode, holding a reference for its whole life so the
file cannot go away under the mapping.

Pages are populated at mmap time here rather than on fault. Demand
paging needs the fault path restructured to drop mm_lock, and doing
both at once means two hard things failing together -- the next commit
makes this lazy without changing a single test.

MAP_SHARED is -ENOSYS: it needs a page cache that does not exist. The
tail of a mapping past EOF reads as zeros, which POSIX requires and
which a .bss at the end of a data segment depends on."
```

---

## Task 3: Make file mappings demand-paged

The hard one. `vma_fault` holds `p->mm_lock` — a spinlock, interrupts
off — across the whole fault, and `vfs_lock` is a mutex that sleeps
(`kernel/fs/vfs.c:77`). The filesystem cannot be reached from there, so
the fault path drops the lock and re-validates on the way back.

**Files:**
- Modify: `kernel/mm/vma.c` (`vma_fault`, `vma_fault_locked`,
  `vma_mmap_file`)
- Modify: `userland/mmapfile.c` (the concurrent test)

**Interfaces:**
- Consumes: `VMA_FILE`, `struct vma.vn/.file_off`,
  `vma_fill_page_from_file` from Task 2.
- Produces: no signature change. `vma_fault(p, addr, write)` keeps its
  contract — 1 handled, 0 → SIGSEGV — and file pages arrive lazily.

- [ ] **Step 1: Write the failing test**

The property that matters is that two threads faulting the same page
end up sharing one frame and leak nothing. Append to
`userland/mmapfile.c`:

```c
// Two threads touch the same page of one mapping at the same time.
// Exactly one frame must end up installed, and the free-frame count
// must return to its baseline after the unmap -- a missed
// re-validation shows up here as a leak, and nowhere else.
static volatile int race_go, race_done;
static long race_addr;

static void *race_thread(void *arg) {
    (void)arg;
    while (!race_go) { }
    volatile unsigned char v = *(const volatile unsigned char *)race_addr;
    (void)v;
    __atomic_fetch_add(&race_done, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void test_concurrent_fault(void) {
    const char *path = "/tmp/dl-race";
    (void)neo_unlink(path);
    long fd = neo_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    static char src[4096];
    for (int i = 0; i < 4096; i++) { src[i] = (char)(i & 0xFF); }
    (void)neo3(SYS_WRITE, fd, src, sizeof src);

    long before = neoos_test_pmm_free();

    for (int round = 0; round < 8; round++) {
        race_addr = neo6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        check(race_addr > 0, "race mapping");
        if (race_addr <= 0) { break; }

        race_go = 0; race_done = 0;
        pthread_t t1, t2;
        pthread_create(&t1, 0, race_thread, 0);
        pthread_create(&t2, 0, race_thread, 0);
        race_go = 1;
        pthread_join(t1, 0);
        pthread_join(t2, 0);
        check(race_done == 2, "both threads finished");
        check(((const volatile unsigned char *)race_addr)[7] == 7, "page content correct");
        (void)neo2(SYS_MUNMAP, race_addr, 4096);
    }

    long after = neoos_test_pmm_free();
    // A slack of a few frames covers thread stacks the allocator has
    // not returned; a per-round leak would be 8 frames or more.
    check(after >= before - 4, "no frames leaked by concurrent faults");
    if (after < before - 4) {
        printf("[dyn]   free before=%d after=%d\n", (int)before, (int)after);
    }

    (void)neo1(SYS_CLOSE, fd);
    (void)neo_unlink(path);
    printf("[dyn] concurrent fault ok\n");
}
```

`neoos_test_pmm_free()` is the existing `TESTHOOK_PMM_FREE`; the
`mmapfile` target must build with `-DNEOOS_TEST_HOOKS` the way `uxtest`
does, or the check compares `-ENOSYS` to itself and proves nothing.

- [ ] **Step 2: Run it and confirm it passes trivially, then make it meaningful**

With Task 2's eager population there is no fault to race, so this test
passes without proving anything. Confirm it passes, then proceed —
Step 4 is where it becomes a real test.

```bash
cd ~/projects/personal/NeoOS && make mmapfile
```
Expected: `[dyn] concurrent fault ok`, proving nothing yet.

- [ ] **Step 3: Restructure the fault path**

In `vma.c`, `vma_fault_locked` gains an out-parameter describing work
that must happen without the lock:

```c
// What a fault needs done outside mm_lock. The filesystem cannot be
// reached from vma_fault_locked: mm_lock is a spinlock held with
// interrupts off, and vfs_lock is a mutex that sleeps.
struct vma_fault_io {
    struct vnode *vn;         // 0 when nothing is needed
    uint64_t      file_off;
    uint64_t      page_va;
    uint32_t      prot;
};
```

`vma_fault_locked` handles an anonymous fault exactly as today and
returns 1. For a `VMA_FILE` page that is not present it fills `*io`,
returns a distinct code, and touches nothing.

`vma_fault` becomes:

```c
int vma_fault(struct process *p, uint64_t addr, int write) {
    struct vma_fault_io io = { 0, 0, 0, 0 };

    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    int rc = vma_fault_locked(p, addr, write, &io);
    spin_unlock_irqrestore(&p->mm_lock, f);
    if (rc != VMA_FAULT_NEEDS_IO) { return rc; }

    // No lock held. Allocate and read.
    uint64_t frame = pmm_alloc(0);
    if (!frame) { vnode_put(io.vn); return 0; }
    int frc = vma_fill_page_from_file(io.vn, io.file_off, frame);
    vnode_put(io.vn);                  // the reference taken under the lock
    if (frc != 0) { pmm_free(frame, 0); return 0; }

    // RE-VALIDATE. The world moved while the lock was down: the VMA may
    // have been unmapped or replaced, or another thread may have
    // faulted the same page and installed it. Getting this wrong is a
    // leaked frame or a use-after-free, and both hide.
    f = spin_lock_irqsave(&p->mm_lock);
    int installed = vma_install_faulted_page_locked(p, &io, frame);
    spin_unlock_irqrestore(&p->mm_lock, f);

    if (!installed) { pmm_free(frame, 0); }   // someone else won, or it went away
    return 1;
}
```

`vma_install_faulted_page_locked` re-finds the VMA, checks it is still
`VMA_FILE` with the same vnode and offset, checks the page is still
absent (`paging_translate_in` returns 0), and only then maps the frame.
It returns 0 in every other case — and returning 0 when another thread
won is **not** an error: the fault is handled either way, which is why
`vma_fault` returns 1 regardless.

The reference on `io.vn` is taken **under** `mm_lock` in
`vma_fault_locked` and released after the read, so the vnode cannot be
freed while the lock is down.

`vma_mmap_file` stops populating: it inserts the VMA and returns.

- [ ] **Step 4: Run and confirm it still passes, now meaningfully**

```bash
cd ~/projects/personal/NeoOS && make mmapfile
```
Expected: `[dyn] map file ok`, `[dyn] concurrent fault ok`. Both tests
are unchanged from Task 2/Step 1; they now exercise the fault path.

- [ ] **Step 5: Run the gauntlet, twice**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15 both times. A dropped-lock window is exactly the defect
that appears once in twenty boots, so one clean run is not evidence.

- [ ] **Step 6: Commit**

```bash
git add kernel/mm/vma.c kernel/mm/vma.h userland/mmapfile.c
git commit -m "mm: demand-page file-backed mappings

vma_fault held mm_lock -- a spinlock, interrupts off -- across the
whole fault, and vfs_lock is a mutex that sleeps, so the filesystem
simply could not be reached from a fault. The path now drops the lock
and retries, which is what Linux does with mmap_lock for the same
reason: copy out what is needed, release, read with nothing held,
re-acquire and RE-VALIDATE.

The re-validation is the whole risk. The VMA may have been unmapped or
replaced, or another thread may have installed the page while the lock
was down. Losing that race is not an error -- the fault is handled
either way -- but failing to notice it leaks a frame or maps over a
live page. The concurrent test races two threads onto one page eight
times and checks the free-frame count against its baseline, because
that is the only way a missed re-validation becomes visible.

The anonymous path is untouched and still handled entirely under the
lock: it is the common case and must not pay for this."
```

---

## Task 4: `PT_INTERP`, `ET_DYN`, and `AT_BASE`

**Files:**
- Modify: `kernel/elf.c`, `kernel/elf.h`
- Modify: `kernel/sched/proc.c`

**Interfaces:**
- Consumes: nothing from Tasks 1–3 (independent, but pointless without
  them).
- Produces:
  - `struct elf_info` gains `char interp[128]; int has_interp;` and
    `uint64_t load_base;`.
  - `int elf_load_at(const uint8_t *data, uint32_t size, uint64_t *pml4,
    uint64_t base, struct elf_info *out)` — loads with every `p_vaddr`
    offset by `base`. `elf_load` becomes `elf_load_at(..., 0, ...)`.
  - Interpreter load base: `0x0000300000000000`, chosen to sit above
    the executable and below `MMAP_BASE` (`0x0000500000000000`).

- [ ] **Step 1: Write the failing test**

Create `userland/dyntest.c` — a program whose *only* job is to exist as
a dynamic executable:

```c
// userland/dyntest.c -- built WITHOUT -static.
//
// That it runs at all is the milestone: reaching main means the kernel
// loaded PT_INTERP, placed ld-musl, described both correctly in the
// auxiliary vector, and musl relocated everything.
#include <stdio.h>

int main(void) {
    printf("[dyn] dynamic executable running\n");
    printf("[dyn] ALL PASSED\n");
    return 0;
}
```

Add a `dyntest` target to the `Makefile` that builds it **without**
`-static` and against the shared musl, boots it alone, and greps for
`[dyn] ALL PASSED`.

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make dyntest
```
Expected: the spawn fails, or the process dies immediately —
`kernel/elf.c` ignores `PT_INTERP` entirely, so control goes to an
entry point whose relocations were never applied.

- [ ] **Step 3: Implement**

`elf.c`: add `#define ELF_PT_INTERP 3` and, in the phdr loop, record
the interpreter path (bounded by `sizeof out->interp`, and rejected if
longer — a path that does not fit must fail loudly, not be truncated
into a wrong filename). Add `base` to every `p_vaddr` use, and accept
`e_type == ET_DYN` (3) as well as `ET_EXEC` (2).

`proc.c`, in the spawn path after the first `elf_load`:

```c
    // A dynamic executable names its interpreter in PT_INTERP. Load it
    // too, at a base of our choosing, and enter THROUGH it: musl's
    // ld.so relocates itself using AT_BASE, then relocates and starts
    // the executable using AT_PHDR / AT_ENTRY.
    uint64_t entry = info.entry;
    uint64_t interp_base = 0;
    if (info.has_interp) {
        struct elf_info ii;
        if (load_elf_file(info.interp, pml4_phys, INTERP_LOAD_BASE, &ii) != 0) {
            serial_write_string("[elf] interpreter load FAILED: ");
            serial_write_string(info.interp);
            serial_write_string("\n");
            return 0;
        }
        interp_base = INTERP_LOAD_BASE;
        entry = ii.entry;              // enter the INTERPRETER
    }
```

and in the auxv block, alongside the existing pushes:

```c
    // AT_BASE tells ld.so where it was placed, which it needs before it
    // can relocate itself -- and so before it can do anything at all.
    // AT_ENTRY stays the EXECUTABLE's entry: it is how ld.so knows
    // where to hand control once linking is done.
    if (interp_base) { PUSH(AT_BASE); PUSH(interp_base); }
```

`INTERP_LOAD_BASE` is `0x0000300000000000`.

- [ ] **Step 4: Build musl shared**

In `neoos-musl/build.sh`, change `--disable-shared` to
`--enable-shared`. Rebuild, then install into **both** locations:

```bash
cd ~/projects/personal/neoos-musl && \
  KERNEL_SHIM_DIR=$HOME/projects/personal/NeoOS/third_party/shim ./build.sh
cp -a build-output/lib/libc.so \
  ~/opt/cross-x86_64-neoos/x86_64-neoos-linux-musl/lib/
```

musl's `libc.so` **is** the dynamic linker. Install it on the disk
image as `/lib/ld-musl-x86_64.so.1`, which is the path a musl-linked
binary records in its `PT_INTERP`. Verify with:

```bash
x86_64-neoos-linux-musl-readelf -l build/userland/DYNTEST.ELF | grep -A1 INTERP
```

- [ ] **Step 5: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make dyntest
```
Expected: `[dyn] dynamic executable running` and `[dyn] ALL PASSED`.

- [ ] **Step 6: Run the gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: 15/15. Every existing binary is still static and must be
unaffected — that is what this run checks.

- [ ] **Step 7: Commit**

```bash
git add kernel/ userland/dyntest.c Makefile
git commit -m "elf: load PT_INTERP and ET_DYN; push AT_BASE

A dynamically linked executable runs. The kernel does no relocation:
it loads the interpreter named by PT_INTERP at a base of its choosing,
describes the executable with AT_PHDR/AT_PHNUM/AT_ENTRY and the
interpreter with AT_BASE, and enters through the interpreter. musl's
ld.so relocates itself using AT_BASE -- which it needs before it can do
anything at all -- then relocates and starts the executable.

AT_BASE was already #defined in proc.c and simply never pushed, which
is a fair summary of how close this was."
```

---

## Task 5: `dlopen`

**Files:**
- Create: `userland/dynlib.c` (a `.so` to open)
- Modify: `userland/dyntest.c`, `Makefile`

**Interfaces:**
- Consumes: everything above.
- Produces: `int neoos_dl_answer(int x)` in `dynlib.so`, returning
  `x * 2 + 1` — an arithmetic result rather than a constant, so a
  wrongly-resolved symbol cannot pass by accident.

- [ ] **Step 1: Write the failing test**

`userland/dynlib.c`:

```c
// A deliberately trivial shared object, existing only to be dlopen'd.
int neoos_dl_answer(int x) { return x * 2 + 1; }
```

Extend `userland/dyntest.c`:

```c
#include <dlfcn.h>

static void test_dlopen(void) {
    void *h = dlopen("/lib/dynlib.so", RTLD_NOW);
    if (!h) { printf("[dyn] FAILED: dlopen: %s\n", dlerror()); return; }

    int (*answer)(int) = (int (*)(int))dlsym(h, "neoos_dl_answer");
    if (!answer) { printf("[dyn] FAILED: dlsym: %s\n", dlerror()); dlclose(h); return; }

    // Arithmetic, not a constant: a symbol resolved to the wrong
    // function cannot pass this by luck.
    if (answer(20) != 41) { printf("[dyn] FAILED: dlsym returned the wrong value\n"); }
    else                  { printf("[dyn] dlopen ok\n"); }

    if (dlclose(h) != 0) { printf("[dyn] FAILED: dlclose\n"); }
}
```

Build `dynlib.so` with `-shared -fPIC` and install it at `/lib/` on the
disk image.

- [ ] **Step 2: Run it and confirm it fails**

```bash
cd ~/projects/personal/NeoOS && make dyntest
```
Expected: `[dyn] FAILED: dlopen: ...` before the library is built and
installed.

- [ ] **Step 3: Implement**

No kernel work should be required — `dlopen` is the same musl code path
`ld.so` already runs. If it fails, read the `[shim] ENOSYS` number and
add the syscall; **rebuild musl and copy it into the toolchain sysroot
before concluding anything is missing.**

- [ ] **Step 4: Run and confirm it passes**

```bash
cd ~/projects/personal/NeoOS && make dyntest
```
Expected: `[dyn] dlopen ok` and `[dyn] ALL PASSED`.

- [ ] **Step 5: Commit**

---

## Task 6: Documentation, and the markers the gauntlet enforces

Per CLAUDE.md, a kernel feature reachable from userland is not finished
without a library path and a `docs/stdlib.md` entry.

**Files:**
- Modify: `docs/stdlib.md`, `docs/abi-compatibility.md`, `Makefile`

- [ ] **Step 1: Add the new markers to `CORE_REQUIRED_MARKERS`**

`[dyn] pread ok`, `[dyn] map file ok`, `[dyn] concurrent fault ok`, so
the gauntlet enforces them rather than tolerating their absence.

- [ ] **Step 2: Update `docs/stdlib.md`**

Document `pread`; `mmap` of a file (`MAP_PRIVATE` only, `MAP_SHARED`
`-ENOSYS`, page-aligned offsets, zeros past EOF); and dynamic linking
(what works, and the divergences).

Record these divergences explicitly:

- **No `MAP_SHARED` file mappings.** Needs a page cache.
- **No cross-process text sharing.** `MAP_PRIVATE` means each process
  gets its own copy of a library's pages; demand paging changes *when*
  they are allocated, not *whether* they are shared.
- **Faulting a file page is a synchronous polled PIO read** — correct,
  and slow.
- **`dlopen` of a foreign Linux `.so` is untested.**

- [ ] **Step 3: Refresh `docs/abi-compatibility.md`**

Move dynamic linking from "what a ported application would still hit"
to implemented, and state plainly what remains: no `MAP_SHARED`, no
page cache, no verified foreign-binary support.

- [ ] **Step 4: Run the gauntlet and commit**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```

---

## Self-Review Notes

- **Spec coverage.** DL-1's six numbered items map to Tasks 1–6:
  `pread` (1), file-backed mmap (2) and its demand paging (3),
  `PT_INTERP`/`ET_DYN` (4), `AT_BASE` (4), musl shared (4 Step 4), docs
  (6). `dlopen` is Task 5. The spec's four test requirements all appear:
  `dyntest` (4), the `dlopen` round trip (5), the concurrent-fault test
  (3), and the short-read test (2).
- **Naming consistency.** `VMA_FILE`, `vma_mmap_file`,
  `vma_fill_page_from_file`, `vma_install_faulted_page_locked`,
  `struct vma_fault_io`, `elf_load_at`, `INTERP_LOAD_BASE`,
  `neoos_dl_answer` are each defined once and used with the same
  signature throughout.
- **Deliberate split.** Task 2 populates eagerly and Task 3 makes it
  lazy, reusing Task 2's tests unchanged. That is not redundancy: it
  separates "is the mapping correct" from "is the fault path correct",
  so a failure in Task 3 can only be the restructure.
- **Known soft spot.** Task 4 Step 4 (musl shared, and the
  `ld-musl-x86_64.so.1` install path) is the step most likely to need
  iteration, because it depends on how `build.sh` installs and what
  path the linker records in `PT_INTERP`. The `readelf` check is there
  to make the actual recorded path visible rather than assumed.
