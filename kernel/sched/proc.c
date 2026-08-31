// kernel/sched/proc.c -- process lifecycle: creation, fork/exec,
// teardown and reaping. Split out of the former kernel/process.c; the
// code is unchanged, only relocated.

#include "kernel.h"
#include "sched/sched.h"
#include "sched/proc_table.h"
#include "sched/thread_table.h"
#include "sched/fd_table.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "arch/tss.h"
#include "dev/serial.h"
#include "fs/vfs.h"
#include "elf.h"
#include "arch/cpu.h"
#include "arch/cpu_local.h"
#include "sync/waitq.h"
#include "errno.h"
#include "dev/timer.h"
#include "lib/rand.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);
extern void kernel_thread_trampoline(void);
extern void fork_trampoline(void);

// Count of live user processes -- those that have been created and have
// not yet run process_exit(). kmain() seeds it with 1 (its own
// reference, dropped just before schedule()) so a fast child exiting on
// another CPU mid-boot cannot drive it to zero prematurely. The drop
// that reaches zero powers the machine off; until M2 gives us an init
// that calls reboot(2), this is how a headless run ends.
static int user_proc_count;

void user_proc_started(void) {
    __atomic_add_fetch(&user_proc_count, 1, __ATOMIC_ACQ_REL);
}

void user_proc_exited(void) {
    if (__atomic_sub_fetch(&user_proc_count, 1, __ATOMIC_ACQ_REL) == 0) {
        kernel_shutdown();   // does not return
    }
}

// Allocate a new PID (uses new proc_table allocator)
int alloc_id(void) {
    return proc_table_alloc_pid();
}

// A cwd is a fixed-size, always-NUL-terminated buffer, and the only
// string this file copies. vfs.c's str_copy is static to that file and
// not worth exporting for one caller.
static void cwd_copy(char *dst, const char *src) {
    uint64_t i = 0;
    for (; i < VFS_MAX_PATH - 1 && src[i]; i++) { dst[i] = src[i]; }
    dst[i] = '\0';
}

static struct process *proc_alloc(void) {
    struct process *p = (struct process *)kmalloc(sizeof(struct process));
    if (!p) { return 0; }
    for (unsigned i = 0; i < sizeof(struct process); i++) { ((uint8_t *)p)[i] = 0; }
    p->ref = 1;   // the proc_table's reference; dropped by proc_reap

    // Allocate per-process file descriptor table
    struct fd_table *ft = (struct fd_table *)kmalloc(sizeof(struct fd_table));
    if (!ft) { kfree(p); return 0; }
    fd_table_init(ft);
    p->fd_table = ft;

    // Allocate per-process thread table
    struct thread_table *tt = (struct thread_table *)kmalloc(sizeof(struct thread_table));
    if (!tt) { kfree(ft); kfree(p); return 0; }
    thread_table_init(tt);
    p->thread_table = tt;

    p->pid   = alloc_id();  // Uses new proc_table_alloc_pid()
    p->state = PROC_ALIVE;
    // Set HERE, not at each creation site, so that "every process has a
    // cwd" is a property of the allocator rather than a rule each new
    // caller has to remember. fork and spawn overwrite it with the
    // parent's immediately below; anything else inherits the root.
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    p->vmas      = 0;
    p->mmap_next = MMAP_BASE;
    spin_init(&p->mm_lock, LOCK_RANK_MM, "mm");
    waitq_init(&p->exit_waiters);
    waitq_init(&p->join_waiters);
    waitq_init(&p->sig_waiters);
    waitq_init(&p->child_waiters);
    signal_init_process(p);
    // A newly created process leads its own group and session until
    // setpgid/setsid say otherwise; fork/spawn inherit these later.
    p->pgid = p->pid;
    p->sid  = p->pid;

    // The process is discoverable (proc_find, proc_table_for_each_ref)
    // from this point; it already carries ref = 1.
    proc_table_insert(p->pid, p);

    return p;
}

struct process *proc_find(int pid) {
    // Use new hash-based lookup (O(1) average vs O(n) before)
    return proc_table_lookup(pid);
}

void process_init(void) {
    proc_table_init();
    spin_init(&kzombies_lock, LOCK_RANK_PROCTABLE, "kzombies");

    signal_queue_init();
    // Phase 7: Per-CPU ready queues (removed global ready_head/ready_tail init)
    // Now initialized per-CPU
    this_cpu()->ready_head = 0;
    this_cpu()->ready_tail = 0;
    this_cpu()->current = 0;
    idle_init();
    serial_write_string("[process] initialized\n");
}

// The stack_slots bitmap AND the process page tables it maps into are
// both per-process shared state: a concurrent thread_create() on a live
// multi-threaded process would otherwise race two threads onto one slot
// or corrupt the bitmap. p->mm_lock (rank LOCK_RANK_MM) is the guard --
// the same lock a demand-paging fault takes to touch these page tables.
// pmm_alloc (rank PMM) and tlb_defer_free (rank TLB) both rank strictly
// above MM, so the map/unmap loops are legal under it. Callers from
// spawn()/exec_task() run on a process no other CPU can see yet, but
// none of them hold mm_lock, so taking it unconditionally here does not
// double-lock.
int thread_stack_alloc(struct process *p, uint64_t *out_top) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);

    int slot = -1;
    for (int i = 0; i < MAX_THREADS_PER_PROC; i++) {
        if (!(p->stack_slots & (1u << i))) { slot = i; break; }
    }
    if (slot < 0) { spin_unlock_irqrestore(&p->mm_lock, f); return -1; }

    uint64_t top = thread_stack_top_for(slot);
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc(0);
        if (!frame) {
            for (int j = 0; j < i; j++) {
                uint64_t v = top - (uint64_t)(USER_STACK_PAGES - j) * PMM_FRAME_SIZE;
                paging_unmap_from(pml4, v, 1);
            }
            spin_unlock_irqrestore(&p->mm_lock, f);
            return -1;
        }
        zero_frames(frame, 0);
        uint64_t vaddr = top - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }
    // The guard page immediately below this stack is simply never mapped.

    p->stack_slots |= (uint16_t)(1u << slot);
    *out_top = top;
    spin_unlock_irqrestore(&p->mm_lock, f);
    return slot;
}

void thread_stack_free(struct process *p, int slot) {
    if (slot < 0) { return; }
    uint64_t top = thread_stack_top_for(slot);
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t v = top - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_unmap_from(pml4, v, 1);
    }
    p->stack_slots &= (uint16_t)~(1u << slot);
    spin_unlock_irqrestore(&p->mm_lock, f);
}

// Builds a complete, freshly-loaded user address space from the ELF
// image at `path`: a new PML4 with the shared kernel entries, the
// loaded ELF segments, and a fresh user stack. On success, returns 1
// with *out_pml4_phys/*out_entry set; the caller (spawn() for a new
// task, exec_task() for an existing one) is responsible for wiring
// the result into a process. On failure, returns 0 having freed
// any partial address space it built -- the caller's own state (if
// any) is untouched.
static int build_user_address_space(const char *path, uint64_t *out_pml4_phys,
                                   struct elf_info *out_info) {
    int err = 0;
    struct vnode *vn = vfs_resolve(path, &err);
    if (!vn) {
        serial_write_string("[process] FAILED: file not found: ");
        serial_write_string(path);
        serial_write_string("\n");
        return 0;
    }
    uint32_t size = vn->size;

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) {
        serial_write_string("[process] FAILED: kmalloc failed for ELF image\n");
        vnode_put(vn);
        return 0;
    }
    vn->mount->ops->read(vn, 0, image, size);
    vnode_put(vn);

    uint64_t pml4_phys = paging_alloc_pml4();
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    pml4[0] = p4_table[0];     // low identity map -- pmm.c/paging.c internals rely on it
    pml4[256] = p4_table[256]; // physmap
    pml4[511] = p4_table[511]; // kernel higher-half alias

    if (!elf_load(image, size, pml4, out_info)) {
        kfree(image);
        free_address_space(pml4_phys);
        return 0;
    }
    kfree(image);

    // The user stack is NOT mapped here: spawn() and exec_task() call
    // thread_stack_alloc() once the process exists, so slot 0 gets its
    // stack -- and its guard page -- from the same path every other
    // thread uses.

    *out_pml4_phys = pml4_phys;
    return 1;
}

// ---------------------------------------------------------- initial stack
//
// Builds the SysV/Linux process entry stack in the NEW address space,
// which is not the one currently in CR3 -- so every write goes through
// the physmap alias of the stack's own frames rather than through the
// user virtual address.
//
// The layout at _start is fixed by the ABI, bottom-up from RSP:
//
//     argc
//     argv[0] .. argv[argc-1], NULL
//     envp[0] .. NULL
//     auxv pairs, terminated by AT_NULL
//     ... the strings and AT_RANDOM bytes those point at ...
//
// and RSP must be 16-byte aligned. Nothing depended on any of this
// until now -- crt0.asm simply passed argc = 0 -- but TLS does: a
// static executable cannot find its own PT_TLS template without
// AT_PHDR, because it has no dynamic section to look it up in. musl's
// __libc_start_main needs the same vector, which is why building it
// properly now is worth more than a NeoOS-specific "where is my TLS"
// syscall would be.

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9
#define AT_RANDOM 25

// Writes `val` at user virtual address `uva` in the address space
// rooted at pml4_phys, via the physmap. Returns 0 if the address is
// not mapped, which for a stack page the caller just created would be
// a bug rather than a user error.
static int poke_user_u64(uint64_t pml4_phys, uint64_t uva, uint64_t val) {
    uint64_t phys = paging_translate_in(pml4_phys, uva);
    if (!phys) { return 0; }
    *(uint64_t *)phys_to_virt(phys) = val;
    return 1;
}

static int poke_user_bytes(uint64_t pml4_phys, uint64_t uva,
                           const uint8_t *src, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        uint64_t phys = paging_translate_in(pml4_phys, uva + i);
        if (!phys) { return 0; }
        *(uint8_t *)phys_to_virt(phys) = src[i];
    }
    return 1;
}

// Lays out the entry stack and returns the RSP the program should
// start on, or 0 on failure. `path` becomes argv[0].
static uint64_t build_initial_stack(uint64_t pml4_phys, uint64_t stack_top,
                                    const struct spawn_args *args,
                                    const struct elf_info *info) {
    uint64_t sp = stack_top;

    // The argument STRINGS, then AT_RANDOM's sixteen bytes. All of it
    // lives ABOVE the vector that points at it, since the vector is
    // built downward from here.
    uint64_t argv_ptr[SPAWN_MAX_ARGS];
    int argc = args->argc;
    if (argc > SPAWN_MAX_ARGS) { argc = SPAWN_MAX_ARGS; }

    for (int i = argc - 1; i >= 0; i--) {
        const char *s = args->argv[i];
        uint64_t len = 0;
        while (s[len]) { len++; }
        len++;                        // the NUL
        sp -= len;
        argv_ptr[i] = sp;
        if (!poke_user_bytes(pml4_phys, sp, (const uint8_t *)s, len)) { return 0; }
    }

    // AT_RANDOM: sixteen bytes musl uses to seed its stack guard and
    // its malloc. Drawn from a seeded CSPRNG (xoshiro256**) rather than
    // the tick counter. Still not a real entropy pool -- no reseeding, no
    // /dev/random -- but adequate for the stack guard canary.
    sp -= 16;
    sp &= ~0xFULL;
    uint64_t at_random = sp;
    uint8_t at_random_bytes[16];
    rand_bytes(at_random_bytes, 16);
    if (!poke_user_bytes(pml4_phys, at_random, at_random_bytes, 16)) { return 0; }

    // The vector itself: argc(1) + argv(argc) + NULL(1) + envp NULL(1)
    // + 7 auxv pairs, in qwords.
    const int aux_pairs = 7;
    uint64_t words = 1 + (uint64_t)argc + 1 + 1 + (uint64_t)aux_pairs * 2;
    // RSP must be 16-byte aligned AT _start. The vector occupies
    // `words` qwords above it, so the base is aligned after rounding
    // the whole block down to 16.
    sp -= words * 8;
    sp &= ~0xFULL;

    uint64_t at = sp;
    #define PUSH(v) do { if (!poke_user_u64(pml4_phys, at, (v))) { return 0; } at += 8; } while (0)
    PUSH((uint64_t)argc);
    for (int i = 0; i < argc; i++) { PUSH(argv_ptr[i]); }
    PUSH(0);            // argv terminator
    PUSH(0);            // envp terminator (no environment yet)
    PUSH(AT_PHDR);   PUSH(info->phdr);
    PUSH(AT_PHENT);  PUSH(info->phentsize);
    PUSH(AT_PHNUM);  PUSH(info->phnum);
    PUSH(AT_PAGESZ); PUSH(PMM_FRAME_SIZE);
    PUSH(AT_ENTRY);  PUSH(info->entry);
    PUSH(AT_RANDOM); PUSH(at_random);
    PUSH(AT_NULL);   PUSH(0);
    #undef PUSH

    return sp;
}

// Fills `out` with a single argument, the path -- what a spawn with no
// argument vector gets, and what execve(path, {path, NULL}, ...) would
// build.
static void args_from_path(struct spawn_args *out, const char *path) {
    out->argc = 1;
    int i = 0;
    while (path[i] && i < SPAWN_ARG_MAX - 1) { out->argv[0][i] = path[i]; i++; }
    out->argv[0][i] = '\0';
}

struct process *spawn(const char *path) { return spawn_argv(path, 0); }

struct process *spawn_argv(const char *path, const struct spawn_args *args) {
    struct spawn_args local;
    if (!args) { args_from_path(&local, path); args = &local; }
    if (args->argc < 1) { return 0; }

    uint64_t pml4_phys;
    struct elf_info info;
    if (!build_user_address_space(path, &pml4_phys, &info)) {
        return 0;
    }

    struct process *p = proc_alloc();
    if (!p) {
        serial_write_string("[process] spawn FAILED: out of memory for process\n");
        free_address_space(pml4_phys);
        return 0;
    }
    p->pml4_phys   = pml4_phys;
    p->elf          = info;   // the auxv and every thread's TLS come from here
    // A spawn from a running process inherits its cwd, as a fork does.
    // The FIRST process has no caller and keeps proc_alloc's "/".
    struct process *spawner = current_proc();
    if (spawner) { cwd_copy(p->cwd, spawner->cwd); }
    p->parent_pid  = current_proc() ? current_proc()->pid : 0;
    if (current_proc()) { p->pgid = current_proc()->pgid; p->sid = current_proc()->sid; }

    uint64_t user_stack_top;
    if (thread_stack_alloc(p, &user_stack_top) != 0) {
        serial_write_string("[process] spawn FAILED: user stack\n");
        free_address_space(pml4_phys);
        return 0;
    }

    struct thread *t = thread_alloc(p);
    if (!t) {
        serial_write_string("[process] spawn FAILED: out of memory for thread\n");
        free_address_space(pml4_phys);
        return 0;
    }
    t->stack_slot = 0;

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t entry_sp = build_initial_stack(pml4_phys, user_stack_top, args, &info);
    if (!entry_sp) {
        serial_write_string("[process] spawn FAILED: could not build the entry stack\n");
        free_address_space(pml4_phys);
        return 0;
    }

    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = 0;                                  // arg (unused for a main thread)
    *(--sp) = entry_sp;                           // user_rsp, popped by kernel_thread_trampoline
    *(--sp) = info.entry;                         // entry_rip, popped by kernel_thread_trampoline
    *(--sp) = (uint64_t)kernel_thread_trampoline; // context_switch's `ret` lands here
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->saved_rsp         = (uint64_t)sp;
    t->kernel_stack_top  = kstack_top;
    t->kernel_stack_phys = kstack_phys;

    // Standard streams as real /dev/CONSOLE vnodes. stdin is opened
    // read-only and always returns EOF; stdout and stderr both write
    // to the console. The table belongs to the process, so every
    // thread of it shares these.
    vfs_open_into("/dev/CONSOLE", p, 0, 0);
    vfs_open_into("/dev/CONSOLE", p, 1, 1);
    vfs_open_into("/dev/CONSOLE", p, 2, 1);

    user_proc_started();
    enqueue_ready(t);
    return p;
}

// Replaces the calling task's address space in place with the ELF
// image at `path`. Open files, pid, and parent_pid are preserved
// (POSIX default: exec() does not close file descriptors). Returns 1
// on success (the syscall path never actually returns to the old
// program -- frame's saved RIP/RSP are overwritten so the ordinary
// sysret lands in the new one instead), or 0 on failure, leaving the
// calling task completely unchanged and still runnable -- the new
// address space is built and validated to completion before the old
// one is freed, so a bad path or OOM never destroys the caller.
int exec_task(const char *path, struct syscall_frame *frame) {
    uint64_t new_pml4_phys;
    struct elf_info info;
    if (!build_user_address_space(path, &new_pml4_phys, &info)) {
        return 0;
    }

    struct process *p = current_proc();

    // Switch to the new address space BEFORE freeing the old one, for
    // the same reason task_exit() does: pmm stores each free block's
    // links inside the block itself, so freeing the old PML4 while it
    // is still live in CR3 overwrites pml4[0] -- the identity map pmm
    // dereferences those links through. free_address_space() walks via
    // the physmap (PML4[256]), which the new address space shares, so
    // the old space stays reachable after the switch.
    uint64_t old_pml4_phys = p->pml4_phys;
    p->pml4_phys = new_pml4_phys;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
    free_address_space(old_pml4_phys);

    cpu_state_init(current_thread()->xstate);

    // The new address space has no stacks at all: reset the slot bitmap
    // and lay down slot 0 for the (now only) thread.
    p->stack_slots = 0;
    uint64_t user_stack_top;
    if (thread_stack_alloc(p, &user_stack_top) != 0) {
        return 0; // caller is left running its old program
    }

    p->elf = info;

    // exec has no argument vector of its own yet, so the new image gets
    // argv[0] = path and nothing else. execve's argv is the obvious
    // next step and is recorded as a gap in docs/abi-compatibility.md.
    struct spawn_args exec_args;
    args_from_path(&exec_args, path);
    uint64_t entry_sp = build_initial_stack(new_pml4_phys, user_stack_top,
                                            &exec_args, &info);
    if (!entry_sp) { return 0; }

    // The new program starts with no thread pointer. Not resetting it
    // would leave FS pointing into the address space that was just
    // freed, and the first __thread access in the new image would read
    // whatever now occupies that physical page.
    current_thread()->fs_base = 0;

    frame->rcx = info.entry;      // user RIP the ordinary sysret epilogue will return to
    frame->user_rsp = entry_sp;

    return 1;
}

// Walks every present user-mode page in `parent_pml4`, clears its
// PAGE_WRITABLE bit (marking it copy-on-write), shares the frame via
// pmm_frame_share(), and maps the same frame at the same virtual
// address into `child_pml4`, also read-only. Returns 0 and leaves
// child_pml4 in a to-be-discarded state on out-of-memory (caller frees
// it via free_address_space); parent_pml4's PTEs already flipped
// read-only before the failure stay that way -- harmless, since the
// next write to any of them just takes the (correctly handled,
// refcount-1, no-copy-needed) COW fault path.
static int fork_duplicate_user_pages(uint64_t *parent_pml4, uint64_t *child_pml4) {
    for (unsigned i4 = 0; i4 < 512; i4++) {
        if (i4 == 0 || i4 == 256 || i4 == 511) {
            continue; // shared kernel entries, already copied by the caller
        }
        if (!(parent_pml4[i4] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t *parent_pdpt = (uint64_t *)phys_to_virt(parent_pml4[i4] & PAGE_ADDR_MASK);

        for (unsigned i3 = 0; i3 < 512; i3++) {
            if (!(parent_pdpt[i3] & PAGE_PRESENT)) {
                continue;
            }
            uint64_t *parent_pd = (uint64_t *)phys_to_virt(parent_pdpt[i3] & PAGE_ADDR_MASK);

            for (unsigned i2 = 0; i2 < 512; i2++) {
                if (!(parent_pd[i2] & PAGE_PRESENT)) {
                    continue;
                }
                uint64_t *parent_pt = (uint64_t *)phys_to_virt(parent_pd[i2] & PAGE_ADDR_MASK);

                for (unsigned i1 = 0; i1 < 512; i1++) {
                    if (!(parent_pt[i1] & PAGE_PRESENT)) {
                        continue;
                    }
                    uint64_t virt = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                     ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);

                    parent_pt[i1] &= ~PAGE_WRITABLE;
                    // The parent's TLB may still cache a stale writable
                    // translation for this page from before the PTE
                    // change -- without this invlpg, a write from the
                    // parent right after fork() could silently succeed
                    // via the stale entry instead of taking the COW
                    // fault, corrupting the frame the child now shares.
                    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");

                    uint64_t phys = parent_pt[i1] & PAGE_ADDR_MASK;
                    pmm_frame_share(phys);

                    // Low 12 bits (permission/type flags) plus bit 63
                    // (PAGE_NO_EXECUTE) -- NOT just `& 0xFFF`, which
                    // would silently drop NX and make a non-executable
                    // page executable in the child.
                    uint64_t flags = parent_pt[i1] & (0xFFFULL | PAGE_NO_EXECUTE) & ~PAGE_PRESENT;
                    if (paging_map_into(child_pml4, virt, phys, flags) != 0) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

// Duplicates the calling task into a new child, sharing physical
// frames read-only between the two (see paging_handle_cow_fault for
// the lazy-copy side). Returns the child task on success (the parent
// syscall path returns its pid), or 0 on failure -- leaving the
// parent completely unaffected (nothing is left partially modified).
struct thread *fork_task(struct syscall_frame *frame) {
    struct process *parent = current_proc();

    struct process *child_proc = proc_alloc();
    if (!child_proc) {
        serial_write_string("[process] fork FAILED: out of memory for process\n");
        return 0;
    }
    cwd_copy(child_proc->cwd, parent->cwd);

    uint64_t child_pml4_phys = paging_alloc_pml4();
    uint64_t *child_pml4 = (uint64_t *)phys_to_virt(child_pml4_phys);
    uint64_t *parent_pml4 = (uint64_t *)phys_to_virt(parent->pml4_phys);
    child_pml4[0] = parent_pml4[0];
    child_pml4[256] = parent_pml4[256];
    child_pml4[511] = parent_pml4[511];

    if (!fork_duplicate_user_pages(parent_pml4, child_pml4)) {
        serial_write_string("[process] fork FAILED: out of memory duplicating page tables\n");
        free_address_space(child_pml4_phys);
        return 0;
    }

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    if (!kstack_phys) {
        serial_write_string("[process] fork FAILED: out of memory for kernel stack\n");
        free_address_space(child_pml4_phys);
        return 0;
    }
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys) + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    // Memory layout, lowest address first (i.e. pop order):
    //   r15, r14, r13, r12, rbx, rbp, fork_trampoline, rcx, r11, user_rsp
    // The first six are consumed by context_switch's own epilogue, the
    // seventh by its `ret`, and only the last three by fork_trampoline.
    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = frame->user_rsp;
    *(--sp) = frame->r11;   // user RFLAGS
    *(--sp) = frame->rcx;   // user RIP
    *(--sp) = (uint64_t)fork_trampoline; // context_switch's `ret` lands here
    *(--sp) = frame->rbp;
    *(--sp) = frame->rbx;
    *(--sp) = frame->r12;
    *(--sp) = frame->r13;
    *(--sp) = frame->r14;
    *(--sp) = frame->r15;

    child_proc->pml4_phys   = child_pml4_phys;
    child_proc->parent_pid  = parent->pid;
    child_proc->pgid        = parent->pgid;
    child_proc->sid         = parent->sid;
    child_proc->stack_slots = parent->stack_slots;
    // Copied by value, references included -- see docs/stdlib.md.
    if (!fd_table_dup(child_proc->fd_table, parent->fd_table)) {
        serial_write_string("[process] fork FAILED: out of memory for fd table\n");
        fd_table_free(child_proc->fd_table);
        pmm_free(kstack_phys, KERNEL_STACK_ORDER);
        free_address_space(child_pml4_phys);
        return 0;
    }

    // POSIX: only the CALLING thread is duplicated. The child starts
    // single-threaded no matter how many threads the parent had.
    struct thread *child = thread_alloc(child_proc);
    if (!child) {
        serial_write_string("[process] fork FAILED: out of memory for thread\n");
        pmm_free(kstack_phys, KERNEL_STACK_ORDER);
        free_address_space(child_pml4_phys);
        return 0;
    }
    child->saved_rsp = (uint64_t)sp;
    child->kernel_stack_top = kstack_top;
    child->kernel_stack_phys = kstack_phys;
    child->stack_slot = current_thread()->stack_slot;
    for (uint32_t i = 0; i < cpu_state_size(); i++) {
        ((uint8_t *)child->xstate)[i] = ((uint8_t *)current_thread()->xstate)[i];
    }

    user_proc_started();
    enqueue_ready(child);
    return child;
}

struct kill_ctx {
    int pid;          // the original pid arg (-1 means "everyone")
    int target_pgid;
    int sig;
    struct siginfo *info;
    int found;
    int rc;
};

static void kill_one(struct process *p, void *v) {
    struct kill_ctx *c = (struct kill_ctx *)v;
    if (p->state == PROC_ZOMBIE) { return; }
    if (c->pid != -1 && p->pgid != c->target_pgid) { return; }
    c->found = 1;
    if (c->sig) {
        int r = signal_send_process(p, c->sig, c->info);
        if (r != 0) { c->rc = r; }
    }
}

// pid  > 0   that process
// pid == 0   every process in the caller's group
// pid <  -1  every process in group -pid
// pid == -1  every process the caller may signal
// sig == 0   existence probe: permission checked, nothing sent
int signal_kill(int pid, int sig, struct siginfo *info) {
    if (sig < 0 || sig >= NSIG) { return -EINVAL; }

    if (pid > 0) {
        struct process *p = proc_find(pid);
        if (!p) { return -ESRCH; }
        int rc = (p->state == PROC_ZOMBIE) ? -ESRCH
               : (sig ? signal_send_process(p, sig, info) : 0);
        proc_put(p);
        return rc;
    }

    struct kill_ctx c = {
        .pid = pid,
        .target_pgid = (pid == 0) ? current_proc()->pgid : -pid,
        .sig = sig, .info = info, .found = 0, .rc = 0,
    };
    proc_table_for_each_ref(kill_one, &c);
    return c.found ? c.rc : -ESRCH;
}

struct tkill_ctx {
    int tgid;
    int tid;
    struct thread *victim;   // ref'd via thread_get
};

static void tkill_scan(struct process *p, void *v) {
    struct tkill_ctx *c = (struct tkill_ctx *)v;
    if (c->victim) { return; }
    if (c->tgid > 0 && p->pid != c->tgid) { return; }
    uint64_t pf = spin_lock_irqsave(&p->lock);
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        if (t->tid == c->tid) { thread_get(t); c->victim = t; break; }
    }
    spin_unlock_irqrestore(&p->lock, pf);
}

int signal_tkill(int tgid, int tid, int sig, struct siginfo *info) {
    if (sig < 0 || sig >= NSIG) { return -EINVAL; }

    // Find the target thread under its process's p->lock (which guards
    // p->threads) and take a reference on it, so it cannot be freed by
    // a concurrent thread_join between here and the delivery. Then
    // deliver through the normal signal_send_thread -- it takes p->lock
    // itself, so there is no lock held across it and no recursion.
    struct tkill_ctx c = { .tgid = tgid, .tid = tid, .victim = 0 };
    proc_table_for_each_ref(tkill_scan, &c);

    if (!c.victim) { return -ESRCH; }
    int rc = sig ? signal_send_thread(c.victim, sig, info) : 0;
    thread_put(c.victim);
    return rc;
}

void proc_get(struct process *p) {
    __atomic_add_fetch(&p->ref, 1, __ATOMIC_ACQ_REL);
}

void proc_put(struct process *p) {
    if (__atomic_sub_fetch(&p->ref, 1, __ATOMIC_ACQ_REL) != 0) { return; }
    // Last reference. The address space, fds and per-process tables are
    // already gone (proc_put_live + proc_reap); only the struct and the
    // pid remain.
    pid_free(&global_proc_table.pid_alloc, p->pid);
    kfree(p);
}

void proc_get_live(struct process *p) { p->live_threads++; }

// Drops one live-thread count. On the last one, frees the address
// space and the file descriptors, and turns the process into a zombie
// carrying only its exit code -- the struct itself outlives its
// address space and is freed by proc_reap dropping the last reference.
void proc_put_live(struct process *p) {
    if (--p->live_threads > 0) { return; }

    if (p->pml4_phys) {
        // Leave the dying address space BEFORE freeing it. A freed frame
        // stops being page-table data the instant pmm_free() takes it:
        // the buddy allocator stores each free block's next/prev links in
        // the block's own first 16 bytes, so freeing the PML4 frame
        // writes a pointer pair straight over pml4[0] and pml4[1] -- and
        // pml4[0] is the low identity map that pmm itself dereferences
        // free blocks through. Freeing while this table is still in CR3
        // therefore unmaps the identity map out from under the allocator
        // mid-call (observed: `free_lists[order]->prev = block` faulting
        // on the head pointer written one statement earlier). Switching
        // to the kernel's own never-freed p4_table is safe here for the
        // same reason schedule() falls back to it for kernel-only
        // threads: kernel text (PML4[511]) and the physmap (PML4[256])
        // live there too, and nothing below runs through user mappings.
        __asm__ volatile ("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)p4_table) : "memory");
        free_address_space(p->pml4_phys);
        p->pml4_phys = 0;
    }

    // Release the process's file descriptors. With refcounted vnodes,
    // leaving these open would pin them permanently and make umount
    // report -EBUSY forever.
    fd_table_free(p->fd_table);

    p->state = PROC_ZOMBIE;
    waitq_wake_all(&p->exit_waiters);

    // SIGCHLD's default action is ignore, so this changes nothing for a
    // process with no handler -- but it is what makes wait4(WNOHANG)
    // polling and sigsuspend-based reaping work.
    struct process *parent = proc_find(p->parent_pid);
    if (parent) {
        struct siginfo info;
        for (unsigned i = 0; i < sizeof(info); i++) { ((uint8_t *)&info)[i] = 0; }
        info.si_signo = SIGCHLD;
        info.si_code  = p->exit_signal ? CLD_KILLED : CLD_EXITED;
        info.fields.chld.si_pid    = p->pid;
        info.fields.chld.si_status = p->exit_signal ? p->exit_signal : p->exit_code;
        signal_send_process(parent, SIGCHLD, &info);
        waitq_wake_all(&parent->child_waiters);
        proc_put(parent);
    }
}

// Idempotent. Every thread killed here delivers SIGKILL, whose default
// action re-enters this function -- without the guard the second caller
// would overwrite exit_code and reprint the exit line, breaking both the
// reported status and the boot-log gate.
void process_exit(int code) {
    struct process *p = current_proc();
    struct thread *self = current_thread();

    if (!p->exiting) {
        p->exiting   = 1;
        p->exit_code = code;

        serial_write_string("[process] task exited, pid=");
        serial_write_hex64((uint64_t)p->pid);
        serial_write_string(" code=");
        serial_write_hex64((uint64_t)(int64_t)code);
        serial_write_string("\n");

        // Every sibling dies too. The LAST thread to actually leave --
        // which may be a killed sibling rather than this one -- drops
        // live_threads to zero and frees the address space.
        //
        // Snapshot the sibling pointers under p->lock -- the list
        // mutates on other CPUs as threads exit -- then deliver SIGKILL
        // with the lock released: thread_kill -> signal_send_thread can
        // reach thread_wait_off_cpu(), which spins on a sibling that may
        // itself be blocked on p->lock in thread_exit_self.
        // MAX_THREADS_PER_PROC bounds the snapshot.
        struct thread *victims[MAX_THREADS_PER_PROC];
        int nv = 0;
        uint64_t sf = spin_lock_irqsave(&p->lock);
        for (struct thread *t = p->threads;
             t && nv < MAX_THREADS_PER_PROC; t = t->proc_next) {
            if (t != self) { thread_get(t); victims[nv++] = t; }
        }
        spin_unlock_irqrestore(&p->lock, sf);
        // Refs held across the SIGKILL close the window where a sibling
        // is freed by a concurrent same-process thread_join between the
        // snapshot and the kill.
        for (int i = 0; i < nv; i++) { thread_kill(victims[i]); thread_put(victims[i]); }

        // Runs once per process (guarded by p->exiting). If this was the
        // last live user process, kernel_shutdown() powers the machine
        // off here and never returns; otherwise teardown continues.
        user_proc_exited();
    }

    thread_exit_self(code);
}

// Linux-compatible wait status encoding.
static int encode_status(struct process *p) {
    if (p->exit_signal) { return p->exit_signal & 0x7f; }   // signalled
    return (p->exit_code & 0xff) << 8;                      // exited
}
#define STATUS_STOPPED(sig)  (0x7f | ((sig) << 8))

// Frees a zombie's threads and drops the process table's reference.
// Safe only once nothing is running on any of the threads. Returns 1 if
// this call did the reaping, 0 if another waiter got there first (two
// threads can both see the zombie in the table before either unlinks
// it) -- the caller must not report a pid it did not reap.
static int proc_reap(struct process *p) {
    // Detach the whole zombie list under p->lock, then walk it with the
    // lock released -- thread_wait_off_cpu() spins on another CPU.
    // Callers (wait4, wait_for_pid) do not hold p->lock.
    uint64_t zf = spin_lock_irqsave(&p->lock);
    if (p->reaped) { spin_unlock_irqrestore(&p->lock, zf); return 0; }
    p->reaped = 1;
    struct thread *z = p->zombies;
    p->zombies = 0;
    spin_unlock_irqrestore(&p->lock, zf);
    while (z) {
        struct thread *next = z->proc_next;
        // thread_exit_self parks a thread on p->zombies before it
        // reaches schedule(), so it can still be executing on this
        // stack. See the same wait in idle_entry's kzombies drain.
        thread_wait_off_cpu(z);
        pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
        thread_put(z);   // the thread list's reference; frees struct + xstate
        z = next;
    }

    // Free the per-process tables before removing from the process
    // table. task_exit already released every fd; this drops the
    // level-2 arrays of anything opened after it (there is nothing) and
    // then the table struct itself.
    if (p->fd_table) {
        fd_table_free(p->fd_table);
        kfree(p->fd_table);
        p->fd_table = 0;
    }
    if (p->thread_table) {
        kfree(p->thread_table);
        p->thread_table = 0;
    }

    // Unlink from the process table and drop its reference. If a
    // lookup is mid-flight it holds its own ref and does the real free;
    // otherwise the struct + pid are freed right here.
    proc_table_remove(p);
    return 1;
}

struct wait_ctx {
    struct process *self;
    int pid;
    int options;
    int found;
    struct process *zombie;   // ref'd; caller reaps + proc_puts
    int stopped_pid;          // >0 once a WUNTRACED stop has been reported
};

static void wait_scan(struct process *p, void *v) {
    struct wait_ctx *c = (struct wait_ctx *)v;
    if (c->zombie || c->stopped_pid) { return; }   // already have a result
    if (p->parent_pid != c->self->pid) { return; }
    if (c->pid > 0  && p->pid  != c->pid)          { return; }
    if (c->pid == 0 && p->pgid != c->self->pgid)   { return; }
    if (c->pid < -1 && p->pgid != -c->pid)         { return; }
    c->found = 1;

    if (p->state == PROC_ZOMBIE) {
        proc_get(p);            // keep it alive past the iteration's own put
        c->zombie = p;
        return;
    }
    if ((c->options & WUNTRACED) && p->stopped_count > 0 && !p->stop_reported) {
        p->stop_reported = 1;
        c->stopped_pid = p->pid;
    }
}

// POSIX-shaped, and what musl's waitpid maps onto. NeoOS's own wait()
// is kept beside it rather than replaced -- see docs/stdlib.md.
int64_t wait4(int pid, int *status, int options) {
    struct process *self = current_proc();
    if (!self) { return -ECHILD; }   // a kernel thread has no children

    for (;;) {
        struct wait_ctx c = { .self = self, .pid = pid, .options = options,
                              .found = 0, .zombie = 0, .stopped_pid = 0 };
        proc_table_for_each_ref(wait_scan, &c);

        if (c.zombie) {
            int st = encode_status(c.zombie);
            int reaped = c.zombie->pid;
            int did = proc_reap(c.zombie);   // 0 if another waiter won
            proc_put(c.zombie);              // drops our iteration ref
            if (did) {
                if (status) { *status = st; }
                return reaped;
            }
            continue;   // lost the race; re-scan
        }
        if (c.stopped_pid) {
            if (status) { *status = STATUS_STOPPED(SIGSTOP); }
            return c.stopped_pid;
        }
        if (!c.found) { return -ECHILD; }
        if (options & WNOHANG) { return 0; }
        if (waitq_sleep(&self->child_waiters, 0) == -EINTR) { return -EINTR; }
    }
}

// NeoOS-native: waits for ONE SPECIFIC pid and returns a bare exit
// code. Deliberately NOT a wrapper over wait4: wait4 filters by
// parentage, and this call is documented as taking any pid, not just a
// child. Kernel threads rely on that -- they have no process, so every
// parentage test would fail. (Found by the leak gate: routing this
// through wait4 made five spawns run concurrently instead of in turn.)
int64_t wait_for_pid(int pid) {
    struct process *p = proc_find(pid);   // ref held
    if (!p) { return -1; }

    while (p->state != PROC_ZOMBIE) {
        if (waitq_sleep(&p->exit_waiters, 0) == -EINTR) { proc_put(p); return -EINTR; }
    }

    int st  = encode_status(p);
    int did = proc_reap(p);   // drops the proc_table's ref
    proc_put(p);              // drops ours
    if (!did) { return -1; }  // another waiter reaped it
    return ((st & 0x7f) == 0) ? ((st >> 8) & 0xff) : -(st & 0x7f);
}
