# `clone(2)` for Real musl Pthreads — Design

## Goal

Implement a real `clone` syscall in the NeoOS kernel, wired through the
`neoos-musl` shim, so that musl's own `pthread_create`/`pthread_join`/
`pthread_exit` — and everything built on them (`pthread_mutex_*`,
`pthread_cond_*`, POSIX semaphores, `pthread_key_create` TLS keys once
that lands) — work **unmodified**, exactly as documented in
`docs/stdlib.md`'s `<pthread.h>` divergence list ("musl supplies all of
them, on this same syscall, unchanged").

This does **not** retire NeoOS's existing native `<thread.h>`/
`<pthread.h>` subset in `lib/` (`thread_create`/`thread_join`/its own
mutex and condvar types) — by explicit decision, that stays in place
side by side with musl's real pthreads. The two are independent APIs
built on the same futex substrate; nothing here changes or removes the
native one.

`fork()`, TLS (`arch_prctl`/`fs_base`), and `futex` already work and are
unchanged by this milestone — `clone` is the one missing primitive
between NeoOS and an unmodified multi-threaded musl binary, per
`docs/abi-compatibility.md`'s own "what stands between NeoOS and an
unmodified multi-threaded binary" line.

## Scope

**In scope:** exactly the flag combination musl's `pthread_create.c`
sends — nothing broader. Linux's `clone(2)` flag space also covers
process creation with selective sharing, namespaces, and vfork-style
optimizations; none of that is needed here and none of it is built.

**Out of scope, explicitly:**
- Any `flags` combination other than the one musl sends (rejected with
  `-EINVAL`, not approximated).
- `pthread_key_create`/TLS keys, `pthread_cancel`, `pthread_attr_t`
  (custom stack sizes, detached-at-create), `pthread_once`, rwlocks,
  barriers — all separate, later milestones per `docs/stdlib.md`'s own
  list of what NeoOS's native subset (and, by extension, musl's
  equivalents) still lack for reasons unrelated to `clone`.
- Retiring or touching the native `thread_create`/`thread_join`
  syscalls or the `lib/` pthread subset built on them.
- Process-level `clone()` uses (new PID, selective sharing) — that is
  what `fork()` already covers.

## The exact contract to match

Read directly from `neoos-musl/upstream/src/thread/pthread_create.c`
and `.../src/thread/x86_64/clone.s.orig` (the pre-shim original,
preserved alongside the current `-ENOSYS` stub):

```c
// pthread_create.c
unsigned flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
    | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS
    | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED;
ret = __clone(start, stack, flags, args, &new->tid, TP_ADJ(new), &__thread_list_lock);
```

Numerically (from `neoos-musl/upstream/include/sched.h`): `CLONE_VM
0x100`, `CLONE_FS 0x200`, `CLONE_FILES 0x400`, `CLONE_SIGHAND 0x800`,
`CLONE_THREAD 0x10000`, `CLONE_SYSVSEM 0x40000`, `CLONE_SETTLS
0x80000`, `CLONE_PARENT_SETTID 0x100000`, `CLONE_CHILD_CLEARTID
0x200000`, `CLONE_DETACHED 0x400000` — combined, `0x7D0F00`. The
kernel's flag check is against this literal value (computed
independently by whoever implements it, not copy-pasted from this
doc — the plan's first step is reading the source directly).

`__clone`'s asm (`clone.s.orig`) reorders the C-level arguments
`(func, stack, flags, arg, ptid, tls, ctid)` into the **raw Linux
clone syscall's** register convention: `rdi=flags, rsi=child_stack
(16-byte aligned, arg pushed at its top), rdx=ptid, r10=ctid, r8=tls`.
Critically, **all other general-purpose registers (including `r9`,
which the asm parks `func` in before the syscall) survive into the
child unchanged** — real Linux `clone()`, like `fork()`, copies the
parent's full saved register state into the child and only overrides
`rsp` (and, for the kernel, `rax`). This is exactly what NeoOS's
`struct syscall_frame` already captures in full (`r9, r8, r10, rdx,
rsi, rdi, r15, r14, r13, r12, rbp, rbx, r11, rcx, user_rsp`) and what
`fork_task()`/`fork_trampoline.asm` already restore in full when
resuming a fork()'d child — so reusing that exact trampoline for
clone's child is not an approximation, it is the same real mechanism
Linux uses under the hood.

Two behaviors on the child side, both required for musl's own
`pthread_join`/`pthread_exit` to work, not optional polish:

- **`CLONE_SETTLS`**: the new thread's `fs_base` is the `tls` argument,
  not inherited from the caller (contrast `fork_trampoline`, which
  inherits the parent's `fs_base` because fork's child IS the same
  logical thread).
- **`CLONE_CHILD_CLEARTID`**: when the new thread exits, the kernel
  writes 0 to `*ctid` and does `futex_wake(ctid, 1)`. This is not
  cosmetic — musl's `pthread_join` `futex_wait`s on exactly that word,
  so without this, every `pthread_join` call hangs forever.
- **`CLONE_PARENT_SETTID`**: the kernel writes the new thread's tid to
  `*ptid`, in the calling thread's address space, before the `clone`
  syscall returns to the parent. (musl's own `pthread_create.c` also
  sets `new->tid` from the syscall's return value directly, so this
  write is likely redundant with musl's own bookkeeping in this
  specific call site — implement it anyway, both because the flag is
  set and a future caller may rely on it, and because skipping a
  requested flag silently would be exactly the kind of divergence
  `docs/stdlib.md` requires recording, not omitting.)
- **`CLONE_SYSVSEM`, `CLONE_DETACHED`**: accepted and ignored. NeoOS
  has no SysV semaphore undo lists for `CLONE_SYSVSEM` to share, and
  `CLONE_DETACHED` has been a no-op on Linux itself for two decades
  (the kernel ignores it; musl and glibc still set it for old-kernel
  compatibility) — accepting-and-ignoring both matches real Linux
  behavior, not a NeoOS-specific shortcut, the same precedent already
  used for `FUTEX_PRIVATE_FLAG`.

## Architecture

**Kernel (`neoos-kernel`):**

- New syscall `sys_clone(flags, child_stack, ptid, ctid, tls)` in
  `kernel/syscall/sys_proc.c`, dispatched under a NeoOS-assigned
  syscall number (not Linux's 56, which already collides with NeoOS's
  own `lstat` — this is the reason `clone.s` currently hard-fails; see
  its own comment).
- Validate `flags == 0x7D0F00` exactly (or symbolically OR'd from the
  ten macros above) — anything else returns `-EINVAL`.
- Allocate a `struct thread` under `current_proc()` — no new `struct
  process`, no `paging_alloc_pml4()`, no `fork_duplicate_user_pages`,
  no `vma_copy_all`, no `fd_table_dup`. This is `thread_create()`'s
  "same process, same address space, same fd table" half.
- Build the new kernel stack with the SAME layout `fork_task()` builds
  (r15..rbx, `fork_trampoline`, fs_base, rcx, r11, user_rsp) but:
  - `user_rsp` = the caller's `child_stack` argument, not
    `frame->user_rsp`.
  - the planted "fs_base" value = `tls` argument (since `CLONE_SETTLS`
    is always set in the flags this accepts), not
    `current_thread()->fs_base`.
  - every other saved register (`r9`/`r8`/`r10`/`rdx`/`rsi`/`rdi`/
    `r15`/`r14`/`r13`/`r12`/`rbp`/`rbx`) copied from the calling
    thread's `frame` unchanged, exactly as `fork_task()` already does
    — this is what lets `r9` (holding `func` in musl's asm) survive
    into the child.
  - `stack_slot = -1` (this thread's user stack is musl's `mmap`, not
    NeoOS's stack-slot allocator — nothing to free at exit).
- Record `ctid` on `struct thread` (a new field, e.g.
  `clear_child_tid`) for the exit-time clear+wake.
- `thread_exit_self()`: if `clear_child_tid` is non-null, write 0 to
  `*clear_child_tid` (in the exiting thread's still-live address space)
  and `futex_wake(clear_child_tid, 1)`, before the existing
  zombie-list/`waitq_wake_all` logic.
- `thread_join()`/wherever else unconditionally calls
  `thread_stack_free(p, z->stack_slot)`: guard it on `stack_slot >= 0`
  — a clone-created thread's `stack_slot` is `-1` and its stack must
  NOT be freed by NeoOS (musl owns and frees it).
- `CLONE_PARENT_SETTID`: write the new tid to `*ptid` via the existing
  user-memory-write helper, before returning to the parent.
- Return: new tid to the parent (normal syscall return value); the
  child never "returns" from this syscall handler at all — it resumes
  in userland via `fork_trampoline`'s `iretq`, exactly like a fork()'d
  child, with `rax=0` already baked into that trampoline.

**musl shim (`neoos-musl`):**

- `upstream/src/thread/x86_64/clone.s` (currently the deliberate
  `-ENOSYS` stub) is rewritten — not simply reverted to
  `clone.s.orig` — to funnel through `__neoos_syscall` with the new
  NeoOS clone syscall number, preserving the original's register
  reshuffling and child-side `pop %rdi; call *%r9` / exit dance.
  `syscall_arch.h`'s existing `__neoos_syscall` funnel is the only
  legal way to reach a NeoOS syscall from musl per this repo's own
  adaptor convention; a hand-rolled raw `syscall` instruction in the
  new `clone.s` would defeat the funnel's entire reason for existing
  (a single point that can return `-ENOSYS` loudly instead of landing
  on the wrong NeoOS syscall).

## Testing plan

Boot verification only (no host-runnable unit tests, per project
convention) — a NeoOS test program cross-linked against real musl
`pthread_create`, embedded and run under QEMU, escalating in what it
proves:

1. **Single `pthread_create`/`pthread_join` round trip**: worker thread
   writes a known value/prints a known string, main thread joins and
   observes it (this is exactly `thread_test.nim`'s scenario from the
   feasibility spike, this time expected to say `PASSED` instead of
   `cannot create thread` — a real Nim `--threads:on` binary is a fine
   test vehicle here since it already exists from that spike).
2. **Several threads incrementing a shared counter under a real
   `pthread_mutex_t`** (musl's, not NeoOS's native one) — proves
   `CLONE_VM` (shared memory) and the futex-backed mutex both hold up
   under real contention.
3. **`pthread_cond_wait`/`pthread_cond_signal`** producer/consumer —
   proves the condvar's sequence-number design still works when driven
   by musl's own condvar code rather than NeoOS's native one.
4. **Re-run `docs/superpowers/plans/2026-09-06-libssh2-port.md`'s and
   the curl port's boot verifications** (or at minimum the gauntlet) to
   confirm nothing regresses for single-threaded musl programs, which
   exercise the surrounding syscall paths this milestone touches
   (`thread_exit_self`, `thread_join`).
5. Full `tools/gauntlet.sh 15 3`, zero retries, as every milestone
   requires.

## Documentation

- `docs/stdlib.md`: new `clone` entry under whatever section covers
  raw syscalls exposed via the musl shim (this is a musl-internal
  primitive, not a new `lib/`-style NeoOS API — no new public header,
  since musl's own `<pthread.h>` is the interface applications use).
  Record the flag-exact-match divergence from Linux (Linux accepts
  the full `clone` flag space; NeoOS accepts exactly musl's own
  combination and `-EINVAL`s everything else).
- `docs/abi-compatibility.md`: close out the "No `clone`" line in the
  "what a real ported application hits" list (§9 item 3) and the
  "what stands between NeoOS and an unmodified multi-threaded binary"
  summary (§10) — both currently point at this exact gap.

## Self-review

- **Placeholders**: none — every task-shaping detail (flag value,
  register convention, which existing trampoline/allocator paths to
  reuse vs. bypass) is pinned to source already read during this
  design, not left as "figure it out."
- **Internal consistency**: the native `lib/` pthread subset and
  musl's real pthreads are explicitly declared independent and
  non-interacting throughout — no section implies one replaces or
  touches the other.
- **Scope**: single kernel primitive plus its musl-side wiring; the
  testing plan is the natural boundary for one implementation plan
  (no need to decompose further).
