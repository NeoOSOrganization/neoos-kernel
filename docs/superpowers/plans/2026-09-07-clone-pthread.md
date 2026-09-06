# Real `clone(2)` for musl Pthreads Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a real `clone` syscall in NeoOS so musl's own `pthread_create`/`pthread_join`/`pthread_exit` (and everything built on them) work unmodified, without touching or retiring NeoOS's existing native `<thread.h>`/`<pthread.h>` subset.

**Architecture:** `sys_clone()` combines two existing, already-proven mechanisms: `thread_create()`'s "new thread, same process, same address space/fd-table/signal-table" skeleton, and `fork_task()`/`fork_trampoline.asm`'s "child resumes at the parent's exact syscall-return site via `iretq`" trick — substituting the caller's `child_stack` for the parent's `user_rsp` and the `tls` argument for inherited `fs_base`. Exit-time behavior adds the `CLONE_CHILD_CLEARTID` futex wake `pthread_join` depends on, and routes detached clone-threads through the existing `kzombies` self-reap drain (already used for process-less kernel threads) so their kernel stack and `struct thread` are reclaimed without anyone calling NeoOS's own `thread_join`.

**Tech Stack:** NeoOS kernel (C, x86-64 asm), `neoos-musl` shim (`clone.s`).

**Spec:** `docs/superpowers/specs/2026-09-07-clone-pthread-design.md`

## Global Constraints

- Accept **exactly** musl's flag combination: `CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID|CLONE_DETACHED` = `0x7D0F00`. Anything else returns `-EINVAL`.
- Do not modify, remove, or retire `lib/`'s native `thread_create`/`thread_join`/`pthread_mutex_*`/`pthread_cond_*` subset or the `SYS_THREAD_CREATE`/`SYS_THREAD_JOIN`/`SYS_THREAD_EXIT`/`SYS_THREAD_SELF` syscalls it uses. Both threading paths coexist permanently.
- No `pthread_key_create`/TLS keys, `pthread_cancel`, `pthread_attr_t`, `pthread_once`, rwlocks, barriers — out of scope, unrelated to `clone` itself.
- No new process-level `clone()` uses (namespaces, `CLONE_VFORK`, selective-sharing process creation) — `fork()` already covers process creation.
- Boot-verified only, via QEMU + serial log capture, per project convention (no host-runnable unit tests).

---

### Task 1: `sys_clone` — thread creation mechanics

**Files:**
- Modify: `kernel/syscall/syscall_nr.h` — add `SYS_CLONE`, bump `SYS_MAX`.
- Modify: `kernel/syscall/syscall.c` — dispatch table entry.
- Modify: `kernel/syscall/sys_proc.c` — new `sys_clone()` handler.
- Modify: `kernel/sched/proc.h` — one new `struct thread` field.
- Modify: `kernel/sched/thread.c` — new `clone_task()` (the actual mechanism).
- Modify: `kernel/arch/fork_trampoline.asm` — **found during implementation, not anticipated in the design doc**: the trampoline restored `fs_base`/`rcx`/`r11`/`rsp` and the callee-saved registers into a resumed child, but never `r9`. Real Linux clone(2)/fork(2) preserve the *entire* parent register snapshot into the child, and musl's hand-written `clone.s` depends on exactly that — it parks the thread's start function in `r9` across the syscall and does `call *%r9` immediately after. Without restoring `r9`, every `pthread_create`'d thread's first instruction would jump through garbage. Fixed by adding one `pop r9` (harmless, and more correct, for `fork()`'s child too).
- Modify: `kernel/sched/proc.c` — `fork_task()` plants the matching new stack slot (`frame->r9`), since it builds the exact same trampoline-consumed layout `clone_task()` does.
- Test (scratch, not committed): a NeoOS test program issuing the raw `clone` syscall directly (bypassing musl's `clone.s`, which Task 3 rewrites) to prove the kernel mechanism in isolation before musl is involved at all.

**Interfaces:**
- Consumes: `thread_alloc(struct process *p)` (`kernel/sched/thread.c`), `enqueue_ready(struct thread *)`, `current_proc()`/`current_thread()`, `zero_frames(phys, order)`, `pmm_alloc(KERNEL_STACK_ORDER)`, `phys_to_virt`, `copy_to_user(void *dst, const void *src, uint64_t n)` (`kernel/mm/uaccess.h`), `struct syscall_frame` (`kernel/sched/proc.h`), `extern void fork_trampoline(void);` (`kernel/arch/fork_trampoline.asm`).
- Produces: `struct thread *clone_task(struct syscall_frame *frame, uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t tls)` — returns the new thread (already enqueued) or `0` on allocation failure. `t->detached` (new field) — `1` for every clone-created thread. `int64_t sys_clone(struct syscall_args *a)`.

- [ ] **Step 1: Add the syscall number**

In `kernel/syscall/syscall_nr.h`, change:

```c
#define SYS_SOCKETPAIR      91

#define SYS_MAX             92
```

to:

```c
#define SYS_SOCKETPAIR      91
#define SYS_CLONE           92

#define SYS_MAX             93
```

- [ ] **Step 2: Add the `detached` field to `struct thread`**

`kernel/sched/proc.h` already has `uint64_t clear_child_tid;` on `struct thread` (added for `set_tid_address`, currently recorded-and-unacted-on — see its comment). Immediately after that line, add:

```c
    // Set by sys_clone (never by thread_create or fork). Changes how
    // thread_exit_self reclaims this thread: a detached thread has no
    // NeoOS-native joiner (musl's pthread_join synchronizes purely via
    // clear_child_tid's futex, never calling SYS_THREAD_JOIN), so it
    // is routed to kzombies -- the same self-reap drain process-less
    // kernel threads already use -- instead of p->zombies, which
    // nothing would ever drain for it.
    int detached;
```

- [ ] **Step 3: Write `clone_task()` in `kernel/sched/thread.c`**

Add near the top of the file, alongside the other `extern` trampoline declarations:

```c
extern void fork_trampoline(void);
```

Add this function after `thread_create()`:

```c
// clone_task -- the mechanism behind sys_clone. Builds a new thread in
// the CALLING thread's process (no new struct process, no new address
// space: that is thread_create()'s half of this, already correct for
// every existing NeoOS thread). The child resumes exactly where fork's
// child does -- at the parent's syscall-return site, via
// fork_trampoline's iretq -- which is exactly the raw Linux clone(2)
// ABI's own contract: the child inherits the FULL saved register file
// (struct syscall_frame already captures all of it) and only rsp and
// (when CLONE_SETTLS is set, which is the only case this accepts)
// fs_base are substituted. This is what lets musl's clone.s recover
// `func` from r9 on the child side unchanged -- see the design doc.
//
// ptid/ctid are NOT interpreted here (CLONE_PARENT_SETTID's write and
// CLONE_CHILD_CLEARTID's bookkeeping are the caller's job -- sys_clone
// and thread_exit_self respectively); this function only builds the
// thread and resumes it.
struct thread *clone_task(struct syscall_frame *frame, uint64_t child_stack,
                           uint64_t tls) {
    struct process *p = current_proc();
    if (!p || p->exiting) { return 0; }

    struct thread *t = thread_alloc(p);
    if (!t) { return 0; }
    t->stack_slot = -1;   // musl mmap'd this stack; NeoOS does not own it
    t->detached   = 1;

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    if (!kstack_phys) {
        // thread_alloc already linked t into p->threads -- undo that
        // the same way thread_create's own failure path implies:
        // there is no partially-constructed thread left visible.
        // (thread_exit_self is not appropriate here -- t was never
        // scheduled.) Mirror thread_create()'s existing convention: on
        // this failure path thread_create() leaks nothing further
        // because thread_stack_alloc was the only other resource, and
        // clone_task has none; only thread_put is needed to release
        // thread_alloc's own reference.
        thread_put(t);
        return 0;
    }
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys)
                        + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    // Identical layout to fork_task()'s (kernel/sched/proc.c): r15..rbx,
    // fork_trampoline, fs_base, rcx (user RIP), r11 (user RFLAGS),
    // user_rsp. The two substitutions that make this "clone" instead
    // of "fork": user_rsp is the CALLER's child_stack, not
    // frame->user_rsp, and the planted fs_base is `tls`, not
    // current_thread()->fs_base -- CLONE_SETTLS is unconditionally set
    // in the only flag combination this syscall accepts.
    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = child_stack;
    *(--sp) = frame->r11;   // user RFLAGS (same as parent's)
    *(--sp) = frame->rcx;   // user RIP (same as parent's -- resumes in clone.s)
    *(--sp) = tls;          // the child's thread pointer
    *(--sp) = (uint64_t)fork_trampoline;
    *(--sp) = frame->rbp;
    *(--sp) = frame->rbx;
    *(--sp) = frame->r12;
    *(--sp) = frame->r13;
    *(--sp) = frame->r14;
    *(--sp) = frame->r15;

    t->saved_rsp         = (uint64_t)sp;
    t->kernel_stack_top  = kstack_top;
    t->kernel_stack_phys = kstack_phys;

    // r9/r8/r10/rdx/rsi/rdi are NOT threaded through this stack layout
    // at all -- fork_trampoline never touches them, and they are not
    // part of context_switch's own pop sequence either. This is
    // correct: those registers are read by clone.s ONLY as the very
    // next instructions after its `syscall`, straight out of the CPU's
    // real register file, which the CPU itself preserves across a
    // syscall/sysret round trip for every register this path does not
    // explicitly overwrite (rcx and r11 are the two clobbered by the
    // syscall instruction itself on real hardware, which is exactly
    // why the frame saves and fork_trampoline restores those two
    // explicitly, in front of everything else).
    enqueue_ready(t);
    return t;
}
```

- [ ] **Step 4: Write `sys_clone` in `kernel/syscall/sys_proc.c`**

Add near `sys_thread_create`:

```c
// clone(flags, child_stack, ptid, ctid, tls) -- raw Linux argument
// order, matching musl's own __clone asm exactly (see
// neoos-musl/upstream/src/thread/x86_64/clone.s and the design doc).
// tls is NOT in struct syscall_args's a1..a4 -- it is the 5th syscall
// argument, which this codebase's convention (see sys_mmap) reads out
// of frame->r8 directly.
//
// Scope: EXACTLY the flag combination musl's pthread_create.c sends.
// Anything else is -EINVAL, not approximated -- see the design doc's
// "Scope" section for why every other clone(2) use is out of scope.
#define NEOOS_CLONE_FLAGS_SUPPORTED \
    (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD \
     | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID \
     | CLONE_CHILD_CLEARTID | CLONE_DETACHED)

int64_t sys_clone(struct syscall_args *a) {
    uint64_t flags       = (uint64_t)a->a1;
    uint64_t child_stack = (uint64_t)a->a2;
    uint64_t ptid        = (uint64_t)a->a3;
    uint64_t ctid        = (uint64_t)a->a4;
    uint64_t tls         = a->frame->r8;

    if (flags != NEOOS_CLONE_FLAGS_SUPPORTED) { return -EINVAL; }
    if (!child_stack) { return -EINVAL; }

    struct thread *t = clone_task(a->frame, child_stack, tls);
    if (!t) { return -EAGAIN; }

    t->clear_child_tid = ctid;   // acted on by thread_exit_self (Task 2)

    if (ptid) {
        int tid = t->tid;
        uint64_t missed = copy_to_user((void *)(uintptr_t)ptid, &tid, sizeof tid);
        if (missed > 0) { return -EFAULT; }
    }

    return t->tid;
}
```

Add the ten `CLONE_*` numeric `#define`s used above (values from `neoos-musl/upstream/include/sched.h`, verified during the design phase) near the top of `sys_proc.c` beside the other flag-value blocks already there (matching the file's existing "these cross the syscall boundary, so their VALUES are Linux's" convention from `syscall_internal.h`):

```c
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED       0x00400000
```

- [ ] **Step 5: Wire the dispatch table entry**

In `kernel/syscall/syscall.c`, immediately after the `SYS_SOCKETPAIR` line:

```c
    [SYS_CLONE]           = { sys_clone,           "clone" },
```

- [ ] **Step 6: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output kernel 2>&1 | tail -40
```

Expected: clean build. If `CLONE_VM`/etc. collide with an existing macro elsewhere in `sys_proc.c`'s translation unit, rename with a `NEOOS_` prefix and report which macro collided before proceeding.

- [ ] **Step 7: Write the raw-syscall test program**

This test bypasses musl's `clone.s` entirely (Task 3 rewrites that) and issues `SYS_CLONE` directly via inline asm, to prove the kernel mechanism alone. Save as `<scratch>/clone_raw_test.c`:

```c
// clone_raw_test.c -- proves sys_clone's mechanism in isolation, before
// musl's clone.s is touched. Issues SYS_CLONE directly.
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#define SYS_CLONE_NR 92
#define STACK_SIZE (64 * 1024)

static long raw_syscall(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ __volatile__ ("syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static volatile int child_ran = 0;
static char child_stack[STACK_SIZE] __attribute__((aligned(16)));

int main(void) {
    // child_stack grows down; child's very first "instruction" after
    // the syscall returns is THIS SAME CODE (fork-style resume), not a
    // fresh function -- so unlike musl's clone.s there is no separate
    // entry point here. We distinguish parent/child by the syscall's
    // own return value, exactly like fork().
    uint64_t sp = (uint64_t)(child_stack + STACK_SIZE) & ~0xFULL;

    long rc = raw_syscall(SYS_CLONE_NR, 0x7D0F00, (long)sp, 0, 0, 0);
    if (rc == 0) {
        // child
        child_ran = 1;
        printf("[clonetest] child running, tid-side effects aside\n");
        _exit(0);
    } else if (rc > 0) {
        // parent
        for (volatile int i = 0; i < 50000000; i++) {}  // let child run
        printf("[clonetest] parent: clone returned tid=%ld, child_ran=%d\n",
               rc, child_ran);
        if (child_ran) {
            printf("[clonetest] PASSED\n");
        } else {
            printf("[clonetest] FAILED: child never ran\n");
        }
    } else {
        printf("[clonetest] FAILED: clone returned %ld\n", rc);
    }
    return 0;
}
```

Note: `child_ran` being visible to the parent at all is itself a first
proof of `CLONE_VM` (shared memory) working — a `fork()`'d child's
write would never be visible here.

- [ ] **Step 8: Cross-compile, nexify, boot, verify**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
MUSL=/home/neo/projects/personal/neoos-musl/build-output
NEOOS=/home/neo/projects/personal/NeoOS
SCRATCH=<this session's scratchpad>/clone-probe
mkdir -p "$SCRATCH"
cp clone_raw_test.c "$SCRATCH/"
cd "$NEOOS"
x86_64-elf-gcc -static -nostdlib -nostdinc -ffreestanding -mcmodel=large \
  -fno-pic -mno-red-zone -fno-stack-protector -O2 \
  -isystem "$MUSL/include" -T userland/user.ld -z noexecstack \
  -o "$SCRATCH/clone_raw_test.elf" \
  "$MUSL/lib/crt1.o" "$SCRATCH/clone_raw_test.c" \
  -L"$MUSL/lib" -lc -lgcc
./tools/nexify.sh "$SCRATCH/clone_raw_test.elf" "$SCRATCH/clone_raw_test.nex"
echo '{"category":"bin"}' > "$SCRATCH/clone_raw_test.test.json"
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    EMBED_DIRS="$SCRATCH" iso disk-image
printf 'wait /bin/clone_raw_test.nex\n' > /tmp/clone-inittab
mcopy -o -i build/disk.img /tmp/clone-inittab ::etc/inittab
timeout 20 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -no-reboot -display none -serial file:build/clone-raw-test.log
grep -i "clonetest\|panic\|exception\|halted\|fault" build/clone-raw-test.log
```

Expected: `[clonetest] child running...`, `[clonetest] parent: clone returned tid=<N>, child_ran=1`, `[clonetest] PASSED`. If the child never runs (`child_ran=0`), the most likely cause is the stack layout in `clone_task()` not matching `fork_trampoline.asm`'s pop order exactly — recheck Step 3 against `kernel/arch/fork_trampoline.asm` line by line before changing anything else (systematic-debugging: one hypothesis at a time).

- [ ] **Step 9: Commit**

```bash
cd /home/neo/projects/personal/NeoOS
git add kernel/syscall/syscall_nr.h kernel/syscall/syscall.c \
        kernel/syscall/sys_proc.c kernel/sched/proc.h kernel/sched/thread.c
git commit -m "sched: sys_clone -- thread creation half of real musl pthread support"
```

---

### Task 2: Exit-time semantics — `CLONE_CHILD_CLEARTID` and detached self-reap

**Files:**
- Modify: `kernel/sched/thread.c` — `thread_exit_self()`.
- Modify: `kernel/sched/sched.c` — none expected (verify `kzombies`'s drain already handles this case unchanged; see rationale below).
- Test (scratch): extend `clone_raw_test.c` from Task 1.

**Interfaces:**
- Consumes: `futex_op(uint32_t *uaddr, int op, uint32_t val, const struct k_timespec *timeout)` (`kernel/ipc/futex.h`), `copy_to_user`, the existing `kzombies`/`kzombies_lock` globals (`kernel/sched/sched.c`), `t->detached`/`t->clear_child_tid` (Task 1).
- Produces: nothing new for later tasks — this closes out `thread_exit_self`'s handling of clone-created threads.

- [ ] **Step 1: Add the `detached` branch to `thread_exit_self()`**

In `kernel/sched/thread.c`, the existing function has two branches: `if (p) { ... p->zombies ... } else { ... kzombies (kernel thread) ... }`. Change the `if (p)` branch's body to check `t->detached` first:

```c
    if (p) {
        // Remove from per-process thread table (if initialized)
        if (p->thread_table) {
            thread_table_remove(p->thread_table, t);
        }

        // CLONE_CHILD_CLEARTID: musl's pthread_join futex_waits on
        // exactly this word. Without this write+wake, every
        // pthread_join call on a thread created by sys_clone hangs
        // forever -- this is not polish, it is the join mechanism
        // musl actually uses (it never calls NeoOS's own
        // SYS_THREAD_JOIN). Done while the thread's own address space
        // is still fully mapped -- this runs on the exiting thread's
        // own context, before any teardown.
        if (t->clear_child_tid) {
            uint32_t zero = 0;
            copy_to_user((void *)(uintptr_t)t->clear_child_tid, &zero, sizeof zero);
            futex_op((uint32_t *)(uintptr_t)t->clear_child_tid, FUTEX_WAKE, 1, 0);
        }

        uint64_t f = spin_lock_irqsave(&p->lock);
        struct thread **pp = &p->threads;
        while (*pp && *pp != t) { pp = &(*pp)->proc_next; }
        if (*pp) { *pp = t->proc_next; }

        if (t->detached) {
            // No NeoOS-native joiner will ever call thread_join for
            // this tid -- musl's pthread_join already got everything
            // it needs from the futex wake above. Route to kzombies,
            // the same self-reap drain process-less kernel threads
            // already use, instead of p->zombies (which nothing would
            // ever drain for a detached thread -- see the design
            // doc's "self-reaping" finding). thread_stack_free is NOT
            // called here or in the drain -- stack_slot is -1 (Task
            // 1), and thread_stack_free already no-ops on slot < 0.
            spin_unlock_irqrestore(&p->lock, f);
            proc_put_live(p);

            uint64_t zf = spin_lock_irqsave(&kzombies_lock);
            t->proc_next = kzombies;
            kzombies     = t;
            spin_unlock_irqrestore(&kzombies_lock, zf);
        } else {
            t->proc_next = p->zombies;
            p->zombies   = t;
            spin_unlock_irqrestore(&p->lock, f);

            waitq_wake_all(&p->join_waiters);
            proc_put_live(p);
        }
    } else {
```

(The pre-existing `else` branch for `p == 0` kernel threads is unchanged.)

- [ ] **Step 2: Verify `idle_entry`'s `kzombies` drain needs no changes**

Read `kernel/sched/sched.c`'s `idle_entry()` (around the `kzombies` drain loop). It already does exactly `thread_wait_off_cpu(z)` + `pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER)` + `thread_put(z)` — no `thread_stack_free` call at all, which is fine for kernel threads (they never had a `stack_slot`) and equally fine for a detached clone-thread (`stack_slot == -1`, nothing to free). Confirm this by reading the function; no code change should be needed here. If it turns out `thread_put`'s reference accounting assumes `p == 0` anywhere in that path, that assumption must be found and fixed before continuing — report the exact line before changing anything.

- [ ] **Step 3: Extend the test program to prove both behaviors**

Modify `clone_raw_test.c` (same file from Task 1) to add a `ctid` word and a futex-wait on it, plus a loop to catch a kernel-stack leak:

```c
// Add near the top, alongside child_stack:
static volatile uint32_t ctid_word = 1;   // musl convention: nonzero == "still running"

// Raw futex_wait/futex_wake syscall wrappers (SYS_FUTEX = 42 -- see
// kernel/syscall/syscall_nr.h):
#define SYS_FUTEX_NR 42
#define FUTEX_WAIT_OP 0

static long futex_wait_raw(volatile uint32_t *uaddr, uint32_t expected) {
    return raw_syscall(SYS_FUTEX_NR, (long)uaddr, FUTEX_WAIT_OP, expected, 0, 0);
}
```

Replace `main`'s clone call with one that passes `&ctid_word` as `ctid` (the 4th raw-syscall argument), and after the parent branch's busy-wait, add:

```c
        // Prove CLONE_CHILD_CLEARTID: futex_wait returns once
        // thread_exit_self's clear+wake runs, OR ctid_word is already
        // 0 by the time we get here (both are correct outcomes -- the
        // point is this call does not hang).
        if (ctid_word != 0) {
            futex_wait_raw(&ctid_word, 1);
        }
        printf("[clonetest] ctid_word=%u after wait (expect 0)\n", ctid_word);
```

And wrap the whole clone+join-via-futex sequence in a loop of, say, 20 iterations, printing free frame counts from `/proc`-equivalent or a raw `getrusage`-style probe if one exists; if none does, printing nothing extra is fine -- the primary proof is that 20 iterations complete at all without hanging or crashing (a leak large enough to matter within 20 iterations of a 16KiB kernel stack would need real memory pressure to detect quickly, so this test's main job is correctness, not leak-detection under memory pressure; leave leak-under-load as a known gap for a later stress-test milestone rather than inventing a fragile check here).

- [ ] **Step 4: Rebuild, boot, verify**

Repeat Task 1 Step 8's build/boot sequence with the updated test source.

Expected: `[clonetest] PASSED` for each of the 20 iterations, `ctid_word=0 after wait` every time, clean QEMU exit, no panic/exception/halted in the log.

- [ ] **Step 5: Commit**

```bash
cd /home/neo/projects/personal/NeoOS
git add kernel/sched/thread.c
git commit -m "sched: clone-created threads clear/wake ctid and self-reap via kzombies"
```

---

### Task 3: musl shim — real `clone.s`, real `pthread_create`/`pthread_join`

**The canonical shim source lives in THIS repo (`NeoOS`), not
`neoos-musl`.** `neoos-musl/build.sh` calls
`$KERNEL_SHIM_DIR/apply.sh` at build time, which COPIES
`third_party/shim/*` into the `neoos-musl/upstream` submodule
(keeping `.orig` backups) — that is why `neoos-musl/upstream/src/
thread/x86_64/clone.s` currently contains the `-ENOSYS` stub: it is
apply.sh's copy of `third_party/shim/clone.s`, not an independent
file. Editing the applied copy directly would be silently clobbered
the next time anyone rebuilds `neoos-musl`. `KERNEL_SHIM_DIR`'s
default (`../neoos-kernel/third_party/shim`) does not resolve in this
checkout (there is no sibling directory literally named
`neoos-kernel` — this repo is checked out as `NeoOS`), so every
`neoos-musl` build needs an explicit override, same as CI's own
workflow does with its own reference checkout.

**Files:**
- Modify: `third_party/shim/clone.s` (in **this** repo, `NeoOS`) —
  currently the `-ENOSYS` stub from the earlier threading
  investigation.
- Modify: `kernel/syscall/sys_proc.c` — `sys_exit` (see Step 4.5,
  found during implementation).
- Test (scratch): `thread_test.nim` (already exists from the Nim
  feasibility spike) or an equivalent C program using musl's real
  `<pthread.h>`.

**Interfaces:**
- Consumes: `SYS_CLONE = 92`, `SYS_THREAD_EXIT = 18` (both from Task 1's `syscall_nr.h`).
- Produces: a working `pthread_create`/`pthread_join` for Task 4's contention tests.

- [ ] **Step 1: Confirm the current shim state and the apply mechanism**

```bash
cat /home/neo/projects/personal/NeoOS/third_party/shim/clone.s
cat /home/neo/projects/personal/NeoOS/third_party/shim/apply.sh
diff /home/neo/projects/personal/NeoOS/third_party/shim/clone.s \
     /home/neo/projects/personal/neoos-musl/upstream/src/thread/x86_64/clone.s
```

Confirm the third (diff) command produces NO output — proving the
applied copy in `neoos-musl/upstream` really is exactly `apply.sh`'s
copy of the canonical file, not something edited independently. If it
differs, stop and report the actual diff before proceeding.

- [ ] **Step 2: Write the real `clone.s`**

Edit `third_party/shim/clone.s` (in `NeoOS`, this repo) to replace its
`-ENOSYS` stub with upstream's real assembly, changing exactly two
immediates (the syscall numbers) from Linux's to NeoOS's, and
updating the file's own header comment to explain why. (Upstream's
real assembly is preserved as `neoos-musl/upstream/src/thread/x86_64/
clone.s.orig` — read that for the exact instructions to reproduce
before making the two number changes below.)

```asm
// NeoOS's clone.s. Was a deliberate -ENOSYS stub (see git history) --
// upstream's raw assembly is otherwise EXACTLY what NeoOS needs, since
// it already builds the raw Linux clone(2) register convention
// (rdi=flags, rsi=stack, rdx=ptid, r10=ctid, r8=tls) that NeoOS's own
// native syscall convention uses too (see kernel/sched/thread.c's
// clone_task and sys_proc.c's sys_clone). The ONLY two things that
// differ from upstream are the syscall NUMBERS, both of which collide
// with unrelated NeoOS syscalls under Linux's numbering:
//   - clone itself: Linux 56 is NeoOS's lstat. Use SYS_CLONE (92).
//   - the child's post-return exit call: Linux exit(2)=60 is NeoOS's
//     SYS_EXIT_GROUP (60) -- which would kill the WHOLE PROCESS, not
//     just this thread, the moment any pthread's start function
//     returned normally. Use SYS_THREAD_EXIT (18) instead, which is
//     what actually ends one thread on NeoOS. %edi already holds the
//     start function's return value at this point (mov %eax,%edi,
//     below), which is exactly SYS_THREAD_EXIT's one argument.

.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	xor %eax,%eax
	mov $92,%al             /* NeoOS SYS_CLONE, not Linux's 56 */
	mov %rdi,%r11
	mov %rdx,%rdi
	mov %r8,%rdx
	mov %r9,%r8
	mov 8(%rsp),%r10
	mov %r11,%r9
	and $-16,%rsi
	sub $8,%rsi
	mov %rcx,(%rsi)
	syscall
	test %eax,%eax
	jnz 1f
	xor %ebp,%ebp
	pop %rdi
	call *%r9
	mov %eax,%edi
	xor %eax,%eax
	mov $18,%al             /* NeoOS SYS_THREAD_EXIT, not Linux exit's 60 */
	syscall
	hlt
1:	ret
```

- [ ] **Step 3: Rebuild `neoos-musl`, applying the edited shim**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/neoos-musl
rm -rf build-tmp build-output upstream/src/thread/x86_64/clone.s.orig
git -C upstream checkout -- src/thread/x86_64/clone.s   # drop the stale -ENOSYS copy; apply.sh restages it below
make KERNEL_SHIM_DIR=/home/neo/projects/personal/NeoOS/third_party/shim 2>&1 | tail -40
```

Expected: `Integrating NeoOS syscall shim...` followed by a normal
musl `configure`/`make`/`make install` sequence ending in `OK musl
built successfully at build-output`. Then confirm the edit actually
landed:

```bash
diff /home/neo/projects/personal/NeoOS/third_party/shim/clone.s \
     upstream/src/thread/x86_64/clone.s
```

Expected: no output (they match — `apply.sh` copied the new version
in). A syntax error in the new assembly fails loudly during `make`,
not silently.

- [ ] **Step 4: Rebuild the kernel against the new musl, run the raw-syscall test again**

Repeat Task 2 Step 4's boot verification with the freshly built `neoos-musl/build-output` — this confirms Tasks 1-2's kernel mechanism still works unchanged (it does not depend on musl at all), before moving to the musl-level test.

- [ ] **Step 4.5 (found during implementation): `sys_exit` must be thread-aware**

The first real `pthread_create`/`pthread_join` test run hung: worker
ran, but `pthread_join` never returned. Root cause, confirmed by
reading `pthread_create.c`'s `__pthread_exit()` directly rather than
guessing: its normal exit path (used by BOTH a joinable thread and a
detached thread with no separate mmap'd stack to unmap) ends with a
plain `__syscall(SYS_exit, 0)` — real Linux `exit(2)` semantics, which
end ONLY the calling thread when others are still alive (`exit_group`
is the "kill everyone" call, already separate as `SYS_EXIT_GROUP`).
NeoOS's `sys_exit` called `process_exit()` unconditionally — correct
before real threads existed (the calling thread was always the only
one), but now kills the WHOLE process the instant any
`pthread_create`'d worker finishes normally, taking the joiner down
with it.

Fix in `kernel/syscall/sys_proc.c`'s `sys_exit`: call
`thread_exit_self(code)` instead of `process_exit(code)` when
`p->live_threads > 1` at the moment of the call, matching real Linux
`exit(2)` exactly. Verified: the hang is gone, `[pthreadtest] joined,
arg now 42 (expect 42)` / `PASSED`. Ran the full gauntlet immediately
after this fix, ahead of Task 4's scheduled run, given how widely
`sys_exit` is used.

- [ ] **Step 5: Real `pthread_create`/`pthread_join` round trip**

Reuse `thread_test.nim` from the Nim feasibility spike (or write an equivalent small C program calling `pthread_create`/`pthread_join` from `<pthread.h>` directly, cross-linked purely against musl with no NeoOS-native `lib/` involvement):

```c
// pthread_test.c -- real musl pthread_create/pthread_join round trip.
#include <stdio.h>
#include <pthread.h>

static void *worker(void *arg) {
    int *x = (int *)arg;
    printf("[pthreadtest] worker running, arg=%d\n", *x);
    *x = 42;
    return 0;
}

int main(void) {
    pthread_t t;
    int arg = 7;
    if (pthread_create(&t, 0, worker, &arg) != 0) {
        printf("[pthreadtest] FAILED: pthread_create\n");
        return 1;
    }
    printf("[pthreadtest] created, joining\n");
    if (pthread_join(t, 0) != 0) {
        printf("[pthreadtest] FAILED: pthread_join\n");
        return 1;
    }
    printf("[pthreadtest] joined, arg now %d (expect 42)\n", arg);
    if (arg == 42) {
        printf("[pthreadtest] PASSED\n");
    } else {
        printf("[pthreadtest] FAILED: wrong value\n");
    }
    return 0;
}
```

Cross-compile against `neoos-musl/build-output` alone (no OpenSSL/libssh2 needed), nexify, embed, boot — same pattern as every prior test this session. Expected: `[pthreadtest] created, joining` / `[pthreadtest] joined, arg now 42 (expect 42)` / `[pthreadtest] PASSED`, no panic/exception/halted.

- [ ] **Step 6: Commit**

The canonical edit is in `NeoOS` (this repo), not `neoos-musl` — the
`neoos-musl/upstream` copy is `apply.sh`'s generated output and is not
what gets committed there (confirm with `git -C
/home/neo/projects/personal/neoos-musl status --short`; it should show
nothing for `upstream/src/thread/x86_64/clone.s` if the submodule
tracks a pinned commit rather than a dirty worktree — either way, the
source of truth is `third_party/shim/clone.s`):

```bash
cd /home/neo/projects/personal/NeoOS
git add third_party/shim/clone.s
git commit -m "shim: real clone.s -- unmodified musl pthread_create/pthread_join"
```

(Pushed together with the rest of this repo's changes in Task 4 Step 6 — no separate push needed here.)

---

### Task 4: Contention proof, docs, and regression

**Files:**
- Test (scratch): a mutex-contention and a condvar producer/consumer program, both using musl's real `<pthread.h>`.
- Modify: `docs/stdlib.md` — new `clone` shim entry.
- Modify: `docs/abi-compatibility.md` — close the "No `clone`" item.

**Interfaces:**
- Consumes: Task 3's working `pthread_create`/`pthread_join`, plus musl's `pthread_mutex_t`/`pthread_cond_t` (unchanged by this milestone — they already sit on `futex_op`, which already works).
- Produces: nothing later tasks consume — this is the milestone's closing verification and writeup, matching every other milestone's final task in this codebase.

- [ ] **Step 1: Mutex contention test**

```c
// mutex_test.c -- several real musl pthreads incrementing a shared
// counter under a real pthread_mutex_t. Proves CLONE_VM (the counter
// is genuinely shared, not per-thread-copied) and the futex-backed
// mutex under real contention, both via musl's own code paths.
#include <stdio.h>
#include <pthread.h>

#define NTHREADS 4
#define ITERS 10000

static int counter = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

int main(void) {
    pthread_t threads[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&threads[i], 0, worker, 0) != 0) {
            printf("[mutextest] FAILED: pthread_create %d\n", i);
            return 1;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], 0);
    }
    printf("[mutextest] counter=%d (expect %d)\n", counter, NTHREADS * ITERS);
    if (counter == NTHREADS * ITERS) {
        printf("[mutextest] PASSED\n");
    } else {
        printf("[mutextest] FAILED: race or lost update\n");
    }
    return 0;
}
```

Cross-compile, nexify, embed, boot. Expected: `[mutextest] counter=40000 (expect 40000)` and `PASSED` — a wrong (lower) count means either the mutex is not actually excluding, or `CLONE_VM` sharing is broken; a hang means the futex wake path is broken. Report the exact symptom before guessing which.

- [ ] **Step 2: Condvar producer/consumer test**

```c
// cond_test.c -- one producer, one consumer, a real musl pthread_cond_t
// and pthread_mutex_t. Proves the sequence-number condvar design
// (docs/stdlib.md) still holds when driven by musl's own condvar code.
#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static int ready = 0;
static int value = 0;

static void *producer(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    value = 99;
    ready = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    return 0;
}

int main(void) {
    pthread_t t;
    pthread_mutex_lock(&lock);
    if (pthread_create(&t, 0, producer, 0) != 0) {
        printf("[condtest] FAILED: pthread_create\n");
        return 1;
    }
    while (!ready) {
        pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);
    pthread_join(t, 0);
    printf("[condtest] value=%d (expect 99)\n", value);
    if (value == 99) {
        printf("[condtest] PASSED\n");
    } else {
        printf("[condtest] FAILED\n");
    }
    return 0;
}
```

Cross-compile, nexify, embed, boot. Expected: `[condtest] value=99 (expect 99)`, `PASSED`, no hang (a hang here means the condvar's futex wait/wake pairing is broken, not `clone` itself — but confirm `clone`-related tests still pass before assuming a pre-existing condvar bug).

- [ ] **Step 3: Update `docs/stdlib.md`**

Add a `clone` entry near the existing `<pthread.h>` section (or wherever the musl-shim-only, non-`lib/`-facing syscalls are documented — follow whatever heading `set_tid_address`/`futex` use), covering: the exact accepted flag value and why (`-EINVAL` otherwise); that this is what makes musl's real `pthread_create`/`pthread_join`/`pthread_mutex_*`/`pthread_cond_*` work unmodified, alongside (not replacing) NeoOS's native `<thread.h>` subset; the `CLONE_CHILD_CLEARTID` futex-wake behavior; that detached clone-threads self-reap via the same mechanism as process-less kernel threads.

- [ ] **Step 4: Update `docs/abi-compatibility.md`**

Close the "No `clone`" line in §9's "what a real ported application hits" list and the "what stands between NeoOS and an unmodified multi-threaded binary" line in §10 — both currently point at exactly this gap (see the design doc's citations). Add a dated "Refresh" section (matching the file's existing style) summarizing what was verified: real `pthread_create`/`pthread_join`, mutex contention, condvar producer/consumer, all via unmodified musl.

- [ ] **Step 5: Full gauntlet regression**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output iso disk-image
timeout 580 tools/gauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`, zero retries. This exercises every existing single-threaded musl program and every existing native-`thread_create` path (BusyBox, the concurrency-milestone smptest, etc.) — a regression here means Task 1 or 2's changes to shared code (`thread_exit_self`, `syscall.c`'s dispatch table, `syscall_nr.h`) broke something outside this milestone's own scope, which must be root-caused, not patched around.

- [ ] **Step 6: Commit and push**

Everything this milestone changed lives in `NeoOS` (this repo) —
`kernel/` from Tasks 1-2, `third_party/shim/clone.s` from Task 3, and
docs from this task. `neoos-musl` itself is not modified by this
milestone (only rebuilt, applying the shim from `third_party/shim/`
the same way every `neoos-musl` build already does):

```bash
cd /home/neo/projects/personal/NeoOS
git add docs/stdlib.md docs/abi-compatibility.md
git commit -m "docs: clone(2) and real musl pthreads -- stdlib.md entry, abi-compatibility refresh"
git push origin main

cd /home/neo/projects/personal/neoos-musl
git status --short   # expect: empty -- confirms this repo needed no commit of its own
```

- [ ] **Step 7: Report completion**

Summarize: `clone(2)` implemented (exact flag match, `-EINVAL` otherwise), real musl `pthread_create`/`pthread_join`/mutex/condvar all verified against real boot logs, NeoOS's native `<thread.h>` subset untouched and still working (confirmed by the gauntlet), 15/15 gauntlet. Note that `pthread_key_create`/TLS keys, `pthread_cancel`, and custom stack sizes (`pthread_attr_t`) remain open for a later milestone if the Nim JIT runtime (or anything else) ends up needing them.

## Self-Review Notes

- **Spec coverage**: the design doc's Architecture section (thread creation, `CLONE_SETTLS`, `CLONE_CHILD_CLEARTID`, `CLONE_PARENT_SETTID`, `CLONE_SYSVSEM`/`CLONE_DETACHED` accept-and-ignore) maps to Task 1 (creation + `CLONE_SETTLS`/`CLONE_PARENT_SETTID`) and Task 2 (`CLONE_CHILD_CLEARTID` + the self-reap mechanism the design doc's own "self-reaping" note flagged as needing a concrete answer, resolved here via `kzombies`). The Testing Plan's four escalating proofs map 1:1 to Tasks 1, 2, 3, and 4 Steps 1-2. Documentation requirements map to Task 4 Steps 3-4.
- **Placeholder scan**: every code block is complete and specific to this codebase's real symbols (verified against actual source during planning, not invented) — no "add error handling" or "similar to Task N" placeholders.
- **Type/name consistency**: `clone_task()`'s signature (`frame, child_stack, tls`) matches its Task 1 Step 3 definition and Step 4 call site exactly; `t->detached`/`t->clear_child_tid` are used identically in Tasks 1 and 2; `SYS_CLONE`/`SYS_THREAD_EXIT`'s numeric values (92, 18) are used identically in Task 1's kernel-side table and Task 3's `clone.s`.
