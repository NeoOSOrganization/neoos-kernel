# NeoOS Roadmap Architecture

**Date:** 2026-08-27
**Status:** Approved
**Scope:** Cross-cutting architecture for the next 17 milestones
**Amended:** 2026-08-27 — musl libc adopted; see the musl section

## Purpose

This document is not a milestone spec. It records the architectural
decisions that span the whole remaining roadmap, so that each
milestone's own spec and plan can be written later without
re-litigating foundations or discovering that an earlier milestone
foreclosed a later one.

Seventeen milestones follow from thirteen requested features. The gap
is foundations: PCI, DMA, a block-device seam, per-CPU data, wait
queues, zoned physical allocation, a dynamic linker, and (since the
musl decision) signals and a syscall-translation shim are all
prerequisites that did not exist in the tree when this was written.

## Load-bearing decisions

These four were decided first because every other decision follows
from them; two more were added later, as noted below.

| # | Decision | Chosen |
|---|---|---|
| 1 | Thread model | Split `struct process` / `struct thread` |
| 2 | Loadable modules | Full module support early, drivers written as real modules |
| 3 | SMP locking | SMP early, **fine-grained from the start** — no big kernel lock |
| 4 | IPC shape | Kernel ports/channels primitive, userland MPI library layered on top |

A fifth was added mid-design: **full TLS with a dynamic linker**,
including `dlopen`, with **eager binding** (`-z now`).

A sixth was added after the threads milestone shipped: **musl is
NeoOS's C library**, reached through a thin adaptor. See the musl
section below; it changes three milestones and adds two.

### On decision 2

Building a module ABI before any driver exists designs an ABI with no
real consumers, which is how ABIs go wrong. The mitigation is that the
modules milestone must prove itself by converting **existing** code —
the `fatfs`/`ramfs`/`devfs` VFS drivers, which already sit behind a
`struct vfs_ops` table and are nearly modules already. The ABI gets a
real consumer immediately rather than waiting for PCI.

### On decision 3

Fine-grained locking with no host test runner means lock-ordering bugs
are otherwise found only by rare deadlocks under QEMU. Two mitigations
are therefore **not optional**, and ship with the SMP milestone:

- A documented global lock-ordering hierarchy. Every lock has a rank;
  acquiring out of ascending rank order is a bug by definition.
- A debug-build lock-order checker recording held ranks per CPU,
  panicking on inversion and naming both locks involved.

## Directory structure

`kernel/` is flat today (33 files). The roadmap adds roughly 30 more,
most of them drivers.

```
kernel/
  mm/       pmm, paging, heap, + dma.c, vmalloc.c
  fs/       vfs, fatfs, ramfs, devfs, + exfat.c
  sched/    proc.c thread.c sched.c class_rt.c class_fair.c waitq.c
  ipc/      port.c pipe.c sem.c fileobj.c
  module/   loader.c symtab.c
  drivers/
    pci/    pci.c msi.c
    block/  blockdev.c ata.c ahci.c fdc.c
    usb/    core.c hub.c xhci.c ehci.c uhci.c ohci.c hid.c storage.c
    audio/  core.c ac97.c hda.c sb16.c
  lock.c    spinlocks, mutexes, rank checker
lib/
  libneoos       NeoOS-native calls only: spawn, wait-by-pid,
                 mount/umount, later ports and MPI
  musl/          vendored musl + the translation shim in its arch dir
```

No `arch/x86_64/` layer: NeoOS is single-architecture and that
directory would be pure ceremony.

## Memory map

Three regions exist; three are added.

| Region | Base | Purpose | Status |
|---|---|---|---|
| Low identity | `0x0` | early boot, 4GiB via 2MiB pages | exists |
| Physmap | `0xFFFF800000000000` | all physical RAM; `kmalloc` lives here | exists |
| Kernel image | `0xFFFFFFFF80000000` | 1GiB window, `-mcmodel=kernel` | exists |
| ioremap | `0xFFFF888000000000` | PCI BARs, MMIO, ECAM | new |
| Module area | `0xFFFFFFFFC0000000` | 1GiB, executable, `.ko` text/data | new |
| Per-CPU | physmap-backed | one block per CPU, reached via `GS` | new |

The module area's address is forced, not chosen. `kmalloc` returns
physmap pointers at `0xFFFF8000…`, far more than 2GB from kernel text,
so `R_X86_64_PC32`/`PLT32` relocations against kernel symbols would
overflow. Module text must live in the top 2GB, immediately above the
kernel's 1GiB window, with its own executable-capable allocator rather
than `kmalloc`.

## Zoned physical allocation

The buddy allocator has no concept of zones. Two device families
cannot use arbitrary memory:

- **ISA DMA (FDC, SB16)** — buffers must be below **16MiB** and must
  **not cross a 64KiB boundary**. This is an 8237 constraint, not a
  preference.
- **PCI devices** may be 32-bit-DMA-only, requiring below **4GiB**.

`pmm_alloc` therefore grows a zone parameter (`ZONE_DMA` <16MiB,
`ZONE_DMA32` <4GiB, `ZONE_NORMAL`), and `mm/dma.c` provides the
boundary-aware allocator above it. Cheap now; impossible to retrofit
cleanly once the FDC driver is half-written.

## Concurrency

### Process / thread split

`struct process` owns the address space, fd table, pid, parent, and
exit status. `struct thread` owns the kernel stack, `saved_rsp`,
state, XSAVE pointer, TLS base, scheduling class and priority, and the
run-queue link.

```c
struct process {
    int pid, parent_pid;
    uint64_t pml4_phys;
    struct file_descriptor files[MAX_OPEN_FILES];
    struct thread *threads;
    int thread_count;
    /* PT_TLS template recorded by elf.c */
};

struct thread {
    struct process *proc;
    uint64_t saved_rsp;
    uint64_t kernel_stack_top;
    uint64_t fs_base;
    void    *xstate;        /* sized from CPUID.0Dh */
    enum thread_state state;
    struct sched_entity sched;
    struct thread *next;
};
```

The scheduler queues threads. `fork` clones a process with one thread;
`thread_create` adds a thread to the current process. Process teardown
happens when the last thread leaves.

### Two SMP-fatal defects in current code

Both are silent corruption, not slowdowns, and the SMP milestone must
fix them:

1. `syscall_entry.asm:13` holds the caller's user RSP in a **single
   global** `user_rsp_scratch`. Two CPUs in `syscall` simultaneously
   clobber each other's return stack.
2. `syscall_entry.asm:26` loads `rsp` from `[rel tss + 4]` — one
   global TSS means one `rsp0` shared by every CPU.

Both are fixed by the same mechanism: **`swapgs` + per-CPU data**. On
kernel entry `swapgs` brings `IA32_KERNEL_GS_BASE` into `GS`; user RSP
is saved to `gs:[CPU_USER_RSP]` and the kernel stack loaded from
`gs:[CPU_KSTACK]`. Each CPU gets its own TSS with its own `rsp0`. The
interrupt stubs need the same treatment, with `swapgs` made
conditional on whether the trap came from ring 3.

### Two lock types

`fs_lock` today wraps operations that perform disk I/O, which makes
the distinction load-bearing:

- **`spinlock`** — IRQ-safe, never sleeps, never held across I/O. Run
  queues, PMM free lists, vnode hash, device registers.
- **`mutex`** — may sleep, therefore never taken in interrupt context.
  Filesystem operations, module load/unload, USB enumeration.

`fs_lock` becomes a mutex. Taking a mutex in IRQ context, and sleeping
while holding a spinlock, are both bugs the rank checker detects.

### Lock-order hierarchy

Ascending acquisition only.

```
 0  process table          5  vnode
 1  process                6  block device queue
 2  thread                 7  driver locks
 3  mount table            8  run queue (per-CPU)
 4  vnode cache bucket     9  heap
                          10  PMM zone      (innermost)
```

### Wait queues

One primitive — `waitq_sleep(&q, &lock)` / `waitq_wake(&q)` — replaces
the ad-hoc `waiting_for_pid` field in `struct task`. It serves
`wait()`, pipes, semaphores, ports, USB transfer completion, and audio
buffer refill. Everything that blocks in this roadmap blocks here.

### SMP, x2APIC, shootdown

AP bring-up uses INIT–SIPI–SIPI with a real-mode trampoline in low
memory, which the existing PML4[0] identity map already makes
reachable. Each AP gets a TSS, a per-CPU block, an idle thread, and
its own run queue.

x2APIC is small **if** LAPIC access is abstracted first: reads and
writes go behind `lapic_read`/`lapic_write`, dispatching to MMIO or to
MSR `0x800+n` per CPUID. x2APIC also lifts the 255-CPU APIC-ID ceiling
and gives cleaner IPI addressing.

**TLB shootdown is mandatory with the second CPU.** NeoOS already has
COW `fork`, which write-protects pages in an address space another CPU
may be running. Write-protecting without an invalidation IPI is a
lost-write bug. This ships with SMP, not after it.

### Scheduling classes

Per-CPU run queues consulted in fixed order: real-time (FIFO/RR,
strict priority) → fair (weighted, generalizing today's round-robin) →
idle. Class is a field on the thread and dispatch is a table, not a
chain of conditionals. Load balancing pulls from the busiest queue,
informed by **SMT topology from CPUID leaf `0x1F`/`0x0B`**: an idle
physical core is a better target than an idle sibling sharing
execution units.

## Extended CPU state

`cpu.h:6` hardcodes `FPU_STATE_SIZE 512` and inlines the FXSAVE area
into the task struct. AVX2 does not fit.

Enable `CR4.OSXSAVE`, set `XCR0` to x87|SSE|AVX, read the required
area size from `CPUID.0Dh`, and give each **thread** a right-sized,
64-byte-aligned area allocated at creation. Use `XSAVEOPT`/`XSAVEC`
where `CPUID.0Dh.EAX` advertises them, since this runs on every
context switch. Save/restore stays **eager**; lazy FPU switching buys
little and has a bad history.

AVX-512 is explicitly out of scope.

**MMX is nearly free.** MMX registers alias the x87 stack, so XSAVE's
x87 component already preserves them. Enabling MMX means dropping
`-mno-mmx` from `USER_CFLAGS`; `EMMS` discipline is userland's
problem.

## Dynamic linking and TLS

Full TLS requires the general-dynamic and local-dynamic models, which
exist only because shared objects exist. The dynamic linker is
therefore a prerequisite milestone, not an optional extra.

### Dynamic linking

*Kernel:* honor `PT_INTERP` in `spawn`/`exec` — load `/LIB/LD.SO` and
hand it control. This changes the process startup ABI: the initial
stack must carry an **auxiliary vector** (`AT_PHDR`, `AT_PHENT`,
`AT_PHNUM`, `AT_ENTRY`, `AT_BASE`, `AT_PAGESZ`), which `crt0.asm` does
not build today. Also `mmap`/`munmap`/`mprotect` syscalls, since
`ld.so` must map segments — MPI shared memory wants `mmap` too, so it
pays for itself twice.

*Userland:* `ld.so` — self-relocating with no libc available,
`.dynamic` parsing, `DT_NEEDED` dependency graph, symbol lookup via
`DT_GNU_HASH`, and `R_X86_64_RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`
processing with PLT/GOT setup. `dlopen`/`dlsym`/`dlclose` included.

*Binding is eager* (`-z now`): every relocation resolved at load, no
`_dl_runtime_resolve` trampoline, no lazy-resolution race against
threads. Lazy binding is a later optimization milestone if wanted.

*Build:* `libneoos.so` built `-fPIC -shared`; programs built `-pie`
with `--dynamic-linker=/LIB/LD.SO`. Both names fit FAT 8.3.

### TLS

All four access models:

```
local-exec       %fs:offset              static executable only
initial-exec     %fs:GOT(offset)         DT_NEEDED libraries
local-dynamic    __tls_get_addr          library, its own variables
general-dynamic  __tls_get_addr + DTV    dlopen'd libraries
```

Per thread: a static TLS block plus a **DTV** (dynamic thread vector)
that grows on `dlopen`.

```c
struct dtv {
    uintptr_t counter;   /* generation */
    void     *blocks[];  /* one per TLS module */
};
```

x86-64 variant-II layout: the TLS block sits below the thread pointer,
and `%fs:0` is a self-pointer. `elf.c` records the `PT_TLS` template
(address, `filesz`, `memsz`, align) in `struct process`. Thread
creation allocates a static block sized by `ld.so`'s accounting,
copies `.tdata`, zeroes `.tbss`, and sets `FS_BASE` on context switch
— via `WRFSBASE` where CPUID allows, otherwise MSR `0xC0000100`.
`__tls_get_addr` lives in `ld.so`.

TLS relocations: `DTPMOD64`, `DTPOFF64`, `TPOFF64`, `TLSGD`, `TLSLD`,
`GOTTPOFF`.

## Device architecture

### Boot bootstrap

If AHCI is a loadable module it lives on the root filesystem, which
needs AHCI to read. Resolution: **ATA PIO and FAT stay compiled in**
as the guaranteed boot path; AHCI, exFAT, USB, audio, and FDC are
loadable. A GRUB-supplied initrd remains available later without
invalidating this.

### Module format

`.ko` files are ordinary `ET_REL` ELF objects. The loader handles
`R_X86_64_64`, `PC32`, `PLT32`, and `32S`; allocates text and data in
the module region; resolves undefined symbols against a kernel symbol
table generated at build time (an `nm` pass over `kernel.elf` emitted
as a generated `.c`); runs `init`; and refcounts so `rmmod` refuses a
module in use.

Symbols are exported explicitly via `EXPORT_SYMBOL(name)` — an opt-in
surface, not the whole kernel. ABI safety is a build-id stamped into
both kernel and module and compared at load; per-symbol CRC versioning
would be ceremony, since modules are built from the same tree.

### Driver model

Three buses appear: PCI, USB, and "platform" (ISA/legacy devices with
no enumeration — FDC, SB16). Each gets its own driver struct and match
rules. A unified `struct bus_type` abstraction over three members is
not worth its weight.

```c
struct pci_driver {
    const char *name;
    const struct pci_match *match;   /* vendor/device/class */
    int  (*probe)(struct pci_device *dev);
    void (*remove)(struct pci_device *dev);
};
```

`DRIVER_REGISTER` behaves identically whether the driver is built in
or loaded.

### PCI

Config access via **ECAM**, discovered from the ACPI `MCFG` table —
`acpi.c` already parses ACPI tables, so this extends existing code —
with the legacy `0xCF8`/`0xCFC` port pair as fallback. Enumeration
recurses through bridges. BARs are sized by the write-all-ones trick
and mapped into the ioremap region. The capability list is walked for
MSI (`0x05`), MSI-X (`0x11`), and PCIe (`0x10`).

### Interrupts

A vector allocator owns `0x20`–`0xFE`. Legacy INTx handlers chain on
shared lines. **MSI/MSI-X is preferred wherever offered**: each vector
is unshared and steerable to a chosen CPU's LAPIC, which matters once
the scheduler spreads work across cores.

### DMA

`dma_alloc_coherent(size, zone)` returns a virtual pointer plus the
device-visible physical address; the physmap makes that conversion
free. `dma_alloc_isa()` is separate and must satisfy below-16MiB
**and** no-64KiB-boundary-crossing, which is why zoned PMM comes
first.

### Block layer

`struct block_device` with capacity, sector size, and read/write entry
points. Synchronous to start — a request queue is a later
optimization; the seam is what matters. `fatfs.c` moves off its direct
`ata_read_sectors()` call onto it, which is the milestone's proof.
exFAT and FDC then cost nothing extra.

### Driver specifics

- **AHCI** — PCI class `0x010601`, ABAR at BAR5, per-port command list
  and FIS receive area, MSI. This is the vertical slice validating
  PCI + DMA + MSI together.
- **USB** — a core layer (device model, endpoints, URB-style
  transfers, address assignment, hub driver) with HCI drivers behind
  `usb_hcd_ops` and class drivers (HID, mass storage) above. **xHCI
  first**: cleanest interface, well-behaved in QEMU, covers USB 1–3
  alone. EHCI, then UHCI/OHCI follow as separate modules; on real
  hardware those are EHCI companion controllers, a wiring detail for
  that milestone.
- **Audio** — a core layer owning PCM streams, ring buffers, and
  format negotiation, exposed through a devfs node. **AC97 first**
  (simple bus-master descriptor list), then **HDA** (CORB/RIRB, codec
  widget graph — much larger), then **SB16** (ISA DMA, so it wants the
  FDC milestone's 8237 work done).
- **exFAT** — a VFS driver, but not "FAT with bigger numbers": upcase
  table, cluster allocation bitmap, multi-entry directory records with
  name hashes, and the `NoFatChain` contiguous-file case. Its own
  milestone, not an extension of `fatfs.c`.

## IPC

### The file-object refactor

POSIX `pipe()` returns file descriptors: `read`, `write`, and `close`
must work on them unchanged. Today `struct file_descriptor` points
straight at a `struct vnode`, which a pipe is not.

So an fd gains one level of indirection: it points at a **file object**
with an ops table (`read`/`write`/`close`/`seek`). Vnode-backed files
become one implementation; pipes another; ports another. This keeps
the syscall layer uniform at the cost of one small change made once,
rather than special-casing pipes in every syscall.

### Primitives

- **Pipes** — ring buffer, blocking on wait queues, correct EOF when
  the write end closes.
- **Semaphores** — kernel objects, named and unnamed, `sem_wait` /
  `sem_post` on the same wait queues.
- **Ports** — named channels, bounded message queues,
  `send`/`recv`/`reply`. Messages are **copied**; page-granting for
  large transfers is explicitly out of scope until something needs it.
- **MPI library** — userland. Ranks are processes started by a
  launcher, a communicator is a set of ports, collectives are built
  from point-to-point. Shared-memory fast paths depend on `mmap`,
  which the dynamic-linking milestone delivers.

## Standard library surface

`CLAUDE.md` makes this binding: any kernel feature reachable from user
mode ships with a `lib/` wrapper and a `docs/stdlib.md` update.

| Milestone | Header / addition |
|---|---|
| Threads | `<thread.h>` — create, join, exit, self |
| Extended state | no API; `-mno-mmx` dropped from `USER_CFLAGS` |
| Dynamic linking | `<dlfcn.h>`, `mmap`/`munmap`/`mprotect` in `<sys/mman.h>` |
| TLS | no API; `__thread` becomes usable |
| Modules | `insmod`/`rmmod` tools + syscalls |
| IPC | `pipe()` in `<unistd.h>`, `<semaphore.h>`, `<port.h>` |
| MPI | `<mpi.h>` |
| Audio | **`ioctl`** — a syscall NeoOS lacks; format negotiation needs it |

## Verification

The project has no host-runnable tests. Existing convention holds:
in-kernel selftests announcing `passed`/`FAILED`, a userland test
program per milestone, headless QEMU under `timeout`, serial log
grepping. Two things change.

### Serial output under SMP

Every log line today is an unlocked sequence of `serial_putc` calls;
two CPUs logging concurrently interleave character by character. This
project has already debugged one bug *from* interleaved serial output
— the `schedule()` reentrancy bug, found via a doubled
`[DBG] switch prev_pid=` line. Under SMP that becomes the normal case.
Serial output needs a spinlock and a `[cpu N]` prefix, landing with
the SMP milestone.

### QEMU invocation per milestone

`-cpu Nehalem`, used for every run so far, has no AVX.

| Milestone | Flags |
|---|---|
| Extended state | `-cpu Haswell` (or `max`) — Nehalem fails |
| SMP + x2APIC | `-smp 4,cores=2,threads=2 -cpu Haswell,+x2apic -machine q35` |
| PCI / AHCI | `-machine q35 -drive if=none,id=d0 -device ide-hd,drive=d0` |
| exFAT | `mkfs.exfat` image as a third drive |
| FDC | `-drive if=floppy,file=…` |
| USB | `-device nec-usb-xhci -device usb-storage,…` / `usb-ehci` / `piix3-usb-uhci` / `pci-ohci` |
| Audio | `-device AC97` / `-device intel-hda -device hda-duplex` / `-device sb16` |

`-smp 4,cores=2,threads=2` is chosen so SMT topology detection has
something real to detect: two physical cores, two siblings each.

**Audio is verifiable, not just audible.**
`-audiodev wav,id=a,path=out.wav` makes QEMU write playback to a WAV
file, turning "does audio work" into a byte comparison against an
expected buffer — the only form that fits the no-host-test-runner
constraint.

## musl libc

Decided 2026-08-27, after milestone 1. NeoOS's C library is **musl**,
reached through a **thin adaptor layer**. The OS is deliberately not
reshaped to suit musl: partial compatibility is acceptable and NeoOS
may diverge from Linux where it chooses to.

### Where the adaptor sits

**Hybrid: Linux-shaped primitives under NeoOS numbering.** The kernel
adopts Linux *semantics* for the handful of primitives musl is built
on — `futex`, `mmap`, `stat`, signals, `clock_gettime` — but keeps its
own syscall numbers and its own native calls alongside. The adaptor is
then pure number and argument translation in musl's arch directory.

```c
/* musl: arch/x86_64/syscall_arch.h (patched) */
static inline long __syscall2(long n, long a, long b) {
    return __neoos_syscall(neoos_nr[n], a, b);
}

/* the kernel keeps its own numbering AND its own calls */
#define SYS_FUTEX      21   /* Linux semantics, NeoOS number */
#define SYS_MMAP       22
#define SYS_SPAWN       4   /* NeoOS-native, no Linux analogue */
```

**The rule that keeps it thin: translation, never emulation.** If the
shim ever starts emulating a primitive instead of forwarding to one,
that is the signal to add the primitive to the kernel.

### What musl needs that NeoOS lacks

| musl needs | NeoOS status |
|---|---|
| `futex` | nothing; `waitq` (milestone 1) is the substrate |
| `rt_sigaction`/`rt_sigprocmask`/`rt_sigreturn` | **no signals at all** — needed for `abort()` and pthread cancellation |
| `fstat`/`statx` | VFS has no `stat`; stdio picks buffering from `st_blksize`, `isatty` from `st_mode` |
| `writev` | stdio's real write path |
| `clock_gettime`, `nanosleep` | timer exists, nothing exposed |
| `ioctl` (TCGETS) | none; also wanted by audio |
| `mmap`/`munmap`/`mprotect` | dynamic-linking milestone |
| `arch_prctl(ARCH_SET_FS)` | TLS milestone |

One piece of luck: `errno.h` already uses Linux-compatible values
(`EPERM` 1, `ESRCH` 3, `EINTR` 4, `EAGAIN` 11, `EDEADLK` 35), so that
class of mismatch does not exist.

### Effect on planned milestones

- **Dynamic linking** — musl ships its own linker. The work becomes
  *porting* `ldso/dynlink.c`, not writing one; the eager-vs-lazy
  binding decision becomes musl's.
- **Full TLS** — shrinks substantially. musl's `__init_tls`/
  `__copy_tls` own the DTV and `__tls_get_addr`; the kernel supplies
  only `arch_prctl(ARCH_SET_FS)` and the auxv.
- **IPC** — pthread mutexes, condvars and POSIX semaphores are all
  futex-backed, so those come from musl rather than from `lib/`.
- **New: signals**, its own milestone, before musl.
- **New: musl (static)**, before dynamic linking — static musl needs
  far less than dynamic musl, so it is brought up first. Dynamic
  linking then ports `ldso` against a libc already known to work,
  rather than debugging two large unproven pieces failing together.

### Effect on `lib/`

`lib/`'s current surface (`printf`, `opendir`, `thread_create`,
`string.h`) is superseded by musl. What survives as `libneoos` is only
what has no POSIX analogue: `spawn`, wait-by-pid, `mount`/`umount`,
and later ports and MPI. `docs/stdlib.md` documents the NeoOS
extensions and the deliberate divergences, not a whole libc.

## Milestone sequence

```
 1  Threads          DONE (2026-08-27)
 2  Signals          DONE (2026-08-27) -- POSIX signals, job control,
                     wait4; taken before extended state
 3  Extended state   XSAVE/XSAVEOPT, AVX, AVX2, MMX. Widens the signal
                     frame's FP area, which signals defined as
                     FXSAVE-shaped. Runtime detection: the userland
                     baseline stays SSE4.2 so -cpu Nehalem keeps working
 4  musl (static)    futex, mmap, stat, writev, clock_gettime,
                     arch_prctl, ioctl + the translation shim
 5  Dynamic linking  port musl's ldso, PT_INTERP, auxv, dlopen
 6  Full TLS         kernel: arch_prctl + auxv; musl owns the DTV
 7  SMP + x2APIC     AP bringup, per-CPU, fine-grained locks, shootdown
 8  Sched classes    RT / fair / idle, SMT-aware balancing
 9  Modules          loader + symtab      -> proves: VFS drivers
10  PCI/DMA/IRQ      ECAM, MSI, DMA zones -> proves: AHCI/SATA
11  Block layer      generic bdev         -> proves: fatfs
12  exFAT            upcase, bitmap, NoFatChain
13  IPC              file objects, pipes, ports (sems come from musl)
14  MPI library      userland, over ports + shared memory
15  ISA DMA + FDC    8237, 82077
16  USB              core + xHCI, then EHCI/UHCI/OHCI, HID, storage
17  Audio            ioctl, PCM core, AC97, HDA, SB16
```

Ordering rationale, where it is not obvious:

- **Signals and static musl before dynamic linking** — static musl
  needs only syscall primitives, so a failing shim is diagnosable
  without a freshly ported dynamic linker in the picture.
- **Dynamic linking before modules** — both are ELF relocation
  processing. Doing the userland one first means the kernel module
  loader is the second time that code is written, not the first.
- **TLS immediately after dynamic linking** — TLS depends on it, and
  both touch thread creation, which should settle before SMP adds
  per-CPU complexity.
- **Modules before PCI** — so every device driver in the roadmap is
  written as a module from birth, per decision 2.
- **Block layer before exFAT** — exFAT is the second consumer that
  proves the seam is real.
- **FDC before SB16** — SB16 needs the 8237 ISA DMA work FDC
  introduces.

Each milestone gets its own spec and implementation plan, written when
it is reached.

## Out of scope

Recorded so these do not get relitigated per milestone:

- AVX-512 and its opmask/ZMM state.
- Lazy PLT binding (`_dl_runtime_resolve`).
- Page-granting for large IPC messages.
- Per-symbol CRC ABI versioning for modules.
- A block request queue / I/O scheduler.
- An `arch/` directory; NeoOS is x86-64 only.
- IOMMU.
