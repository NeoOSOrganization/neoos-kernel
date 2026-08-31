# Phase 12: IPC, TLS and Networking

**Status:** landed.
**Date:** 2026-08-31.
**Baseline:** `6ed47f9` (Phase 11 close — work stealing).

Phase 11 made threads migrate. Phase 12 gives programs something to do
with that: the synchronisation, communication and startup machinery a
real parallel program needs.

---

## 1. What was asked for, and what was built

| Asked | Built |
|---|---|
| syscall table, no switch/if chain | A table indexed by syscall number, one handler per call, plus an integrity selftest |
| POSIX pipes, semaphores, mutexes | `futex` in the kernel; `<semaphore.h>`, `<pthread.h>` and pipes on top |
| MPI message passing | `<mpi.h>`, a subset of MPI-1 over UDP on loopback |
| thread-local storage | `arch_prctl`, per-thread FS base, and the auxiliary vector it needs |
| basic networking / loopback so sockets work | A loopback device, IPv4, UDP, and AF_INET datagram sockets |

Everything is recorded in `docs/stdlib.md` (the API and every
divergence) and `docs/abi-compatibility.md` (the Linux-ABI inventory).

## 2. Decisions taken, and why

These were made without consultation, as instructed. Each is the kind
that would be expensive to reverse later, so the reasoning is here
rather than only in a commit message.

### One kernel primitive for synchronisation: the futex

The alternative was kernel semaphore and mutex objects with their own
syscalls. The futex was chosen because **musl's mutexes, condition
variables, semaphores, barriers and once-control are all built on it**,
so it is the one primitive that makes the eventual musl integration
translation rather than emulation — which `CLAUDE.md` makes the test of
whether a primitive belongs in the kernel at all.

The consequence is that `<semaphore.h>` and `<pthread.h>` live in
`lib/` and are explicitly placeholders. That sits awkwardly with
"`lib/` keeps only what has no POSIX analogue", and the resolution is
that they are marked as such and deleted when musl lands. The kernel
side — the futex — survives.

### Futexes are keyed by PHYSICAL address

Keying by (address space, virtual address) is simpler and works for
everything NeoOS can build today. It is wrong the moment two processes
share a page, which is exactly what a process-shared semaphore and
MPI's future shared-memory fast path are for. A physical key is right
for both cases and costs one page-table walk.

### File descriptors got an ops table, not a `kind` field

A pipe is not a vnode: no inode, no size, no position, and reading one
blocks. The alternative to an ops table was a `kind` field tested in
every syscall that touches an fd — and one forgotten test is a pipe
read as if it were a file. The table also made sockets nearly free
afterwards.

### Sockets rather than a bespoke "port" object for MPI

The roadmap called for kernel "ports" with `send`/`recv`/`reply`. UDP
datagrams already give message boundaries, and building MPI on sockets
means the transport is one already tested, already visible to a
debugger, and already what a real MPI uses for its TCP path. When NeoOS
gets a NIC, this MPI runs between machines with no change above the
socket calls. Ports are not implemented and are not planned.

### `SOCK_STREAM` is refused, not faked

Accepting `SOCK_STREAM` and serving datagrams would hand a program
message boundaries where it expects a stream, and it would corrupt its
own protocol quietly. `-EPROTONOSUPPORT` is the honest answer until
there is a TCP behind it.

### The auxiliary vector was built properly rather than shortcut

TLS needs the `PT_TLS` template, a static executable cannot find its
own program headers without `AT_PHDR`, and the alternative was a
NeoOS-specific "where is my TLS" syscall. Building a real SysV entry
stack instead closed the single largest item on Phase 10's ABI gap list
and is exactly what musl's `__libc_start_main` reads.

## 3. Bugs found on the way

Six, all pre-existing, and all found because something new finally
depended on them.

1. **The user linker script put the ELF headers outside every loaded
   segment.** `AT_PHDR` had no address to report. One `SIZEOF_HEADERS`.

2. **`.tdata` and `.tbss` were orphan sections placed at the SAME
   address.** Every initialised thread-local overlapped an uninitialised
   one, and the offsets the compiler emitted bore no relation to where
   the bytes were. A `__thread int` initialised to `0x5a5a` read as 0.

3. **The UDP checksum was stored without its byte-order conversion.**
   Every datagram was dropped on receive — silently, because a dropped
   datagram has nobody to report to. The symptom was a hang in the
   first `recvfrom`.

4. **`timer_handler` would preempt a CPU with no current thread.**
   Before its first `schedule()` a CPU is on a bootstrap stack with no
   thread to save into, so switching away abandons it permanently.
   `tlb_shootdown` enables interrupts while waiting for
   acknowledgements — it must, or two CPUs shooting down at once
   deadlock — and a tick in that window took the BSP out of `kmain`
   before it had spawned anything.

5. **`tlb_shootdown` released its serialising lock before waiting for
   acknowledgements**, so two concurrent shootdowns shared one counter.
   Each decremented the other's acks; one returned early, the other
   timed out.

6. **`tlb_defer_free` performed an emergency shootdown when its
   64-entry queue filled — while holding `mm_lock`.** Tearing down an
   address space defers hundreds of frames, and `vma_munmap` unmaps
   under `mm_lock`, which is precisely what `tlb_shootdown` asserts
   against. It fired as a panic once enough processes were exiting
   concurrently. The queue is now unbounded: a fast-path array plus a
   linked overflow whose nodes are allocated before the (innermost)
   queue lock is taken.

Numbers 4, 5 and 6 were all reachable before this milestone and became
likely only because Phase 11 moved AP bringup ahead of the spawns, so
`kmain` now runs concurrently with real work.

## 4. The instrument that made (6) findable

**Spinlocks now panic by name after a bounded spin.** Every spinlock in
this kernel is held with interrupts disabled for a bounded number of
instructions, so a long wait is a cycle, not contention.

Before this, the failure presented as: no output, no panic, and not
even a timer tick — because the stuck CPU was spinning with `IF`
clear. A serial log with zero `[timer] tick=` lines in a 150-second run
was the entire evidence base, and working out *which* CPU had stopped
took longer than fixing what it stopped on.

This is the same bet the lock-rank checker made and won: turn a class
of silent hang into a named panic. It is now the second such
instrument, and both have paid for themselves within a milestone.

## 5. Known gaps

Recorded so they are not rediscovered:

- **No TCP.** The socket layer, the file-ops seam and the loopback
  device are all ready for it; it is a protocol implementation, not
  plumbing.
- **No `select`/`poll`.** A program waiting on several descriptors
  needs a thread each. This is the most conspicuous thing missing from
  the socket work.
- **No `stat` family and `struct dirent` still diverges** — now the two
  largest ABI gaps, per `docs/abi-compatibility.md`.
- **`O_CREAT` still has the wrong value** (0x100, Linux 0x40). Called
  out at the close of Phase 10 and still not fixed. One constant.
- **`AT_RANDOM` is not random.** musl seeds its stack guard from it.
- **MPI collectives are O(n) through rank 0**, and `MPI_Send` never
  blocks (a datagram send does not wait for a receiver), so ring
  exchanges that would deadlock on a synchronous MPI happen to work
  here. Programs must not rely on that.
- **`exec` still takes no argument vector.** `spawnv` exists; `execve`'s
  argv is the same mechanism and is not wired up.
