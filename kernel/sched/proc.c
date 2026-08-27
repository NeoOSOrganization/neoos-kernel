// kernel/sched/proc.c -- process lifecycle: creation, fork/exec,
// teardown and reaping. Split out of the former kernel/process.c; the
// code is unchanged, only relocated.

#include "sched.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../mm/heap.h"
#include "../tss.h"
#include "../serial.h"
#include "../fs/vfs.h"
#include "../elf.h"
#include "../cpu.h"
#include "../cpu_local.h"
#include "../waitq.h"
#include "../errno.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);
extern void kernel_thread_trampoline(void);
extern void fork_trampoline(void);


struct process *proc_list;
struct spinlock proc_lock;


// Starts at 0 so the idle thread -- created first, by idle_init() --
// naturally takes id 0, which is reserved for idle threads and is
// never a valid pid. The first real process therefore gets pid 1.
static int next_id = 0;

int alloc_id(void) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    int id = next_id++;
    spin_unlock_irqrestore(&proc_lock, f);
    return id;
}

static struct process *proc_alloc(void) {
    struct process *p = (struct process *)kmalloc(sizeof(struct process));
    if (!p) { return 0; }
    for (unsigned i = 0; i < sizeof(struct process); i++) { ((uint8_t *)p)[i] = 0; }
    p->pid   = alloc_id();
    p->state = PROC_ALIVE;
    waitq_init(&p->exit_waiters);
    waitq_init(&p->join_waiters);
    waitq_init(&p->sig_waiters);
    waitq_init(&p->child_waiters);
    signal_init_process(p);
    // A newly created process leads its own group and session until
    // setpgid/setsid say otherwise; fork/spawn inherit these later.
    p->pgid = p->pid;
    p->sid  = p->pid;

    uint64_t f = spin_lock_irqsave(&proc_lock);
    p->next   = proc_list;
    proc_list = p;
    spin_unlock_irqrestore(&proc_lock, f);
    return p;
}

struct process *proc_find(int pid) {
    for (struct process *p = proc_list; p; p = p->next) {
        if (p->pid == pid) { return p; }
    }
    return 0;
}

void process_init(void) {
    spin_init(&proc_lock, LOCK_RANK_PROCTABLE, "proc_list");
    signal_queue_init();
    proc_list  = 0;
    ready_head = 0;
    ready_tail = 0;
    this_cpu()->current = 0;
    idle_init();
    serial_write_string("[process] initialized\n");
}

int thread_stack_alloc(struct process *p, uint64_t *out_top) {
    int slot = -1;
    for (int i = 0; i < MAX_THREADS_PER_PROC; i++) {
        if (!(p->stack_slots & (1u << i))) { slot = i; break; }
    }
    if (slot < 0) { return -1; }

    uint64_t top = thread_stack_top_for(slot);
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc(0);
        if (!frame) {
            for (int j = 0; j < i; j++) {
                uint64_t v = top - (uint64_t)(USER_STACK_PAGES - j) * PMM_FRAME_SIZE;
                paging_unmap_from(pml4, v, 1);
            }
            return -1;
        }
        zero_frames(frame, 0);
        uint64_t vaddr = top - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame, PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }
    // The guard page immediately below this stack is simply never mapped.

    p->stack_slots |= (uint16_t)(1u << slot);
    *out_top = top;
    return slot;
}

void thread_stack_free(struct process *p, int slot) {
    if (slot < 0) { return; }
    uint64_t top = thread_stack_top_for(slot);
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t v = top - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_unmap_from(pml4, v, 1);
    }
    p->stack_slots &= (uint16_t)~(1u << slot);
}

// Builds a complete, freshly-loaded user address space from the ELF
// image at `path`: a new PML4 with the shared kernel entries, the
// loaded ELF segments, and a fresh user stack. On success, returns 1
// with *out_pml4_phys/*out_entry set; the caller (spawn() for a new
// task, exec_task() for an existing one) is responsible for wiring
// the result into a process. On failure, returns 0 having freed
// any partial address space it built -- the caller's own state (if
// any) is untouched.
static int build_user_address_space(const char *path, uint64_t *out_pml4_phys, uint64_t *out_entry) {
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

    uint64_t entry;
    if (!elf_load(image, size, pml4, &entry)) {
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
    *out_entry = entry;
    return 1;
}

struct process *spawn(const char *path) {
    uint64_t pml4_phys, entry;
    if (!build_user_address_space(path, &pml4_phys, &entry)) {
        return 0;
    }

    struct process *p = proc_alloc();
    if (!p) {
        serial_write_string("[process] spawn FAILED: out of memory for process\n");
        free_address_space(pml4_phys);
        return 0;
    }
    p->pml4_phys   = pml4_phys;
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

    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = 0;                                  // arg (unused for a main thread)
    *(--sp) = user_stack_top;                     // user_rsp, popped by kernel_thread_trampoline
    *(--sp) = entry;                              // entry_rip, popped by kernel_thread_trampoline
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
    uint64_t new_pml4_phys, new_entry;
    if (!build_user_address_space(path, &new_pml4_phys, &new_entry)) {
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

    frame->rcx = new_entry;       // user RIP the ordinary sysret epilogue will return to
    frame->user_rsp = user_stack_top;

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
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        child_proc->files[i] = parent->files[i]; // copied by value -- see docs/stdlib.md
        // The copy duplicates the vnode POINTER, so the child owes the
        // cache its own reference; without this the first close on
        // either side would free a vnode the other still holds.
        if (child_proc->files[i].in_use && child_proc->files[i].vn) {
            child_proc->files[i].vn->refcount++;
        }
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

    enqueue_ready(child);
    return child;
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
        if (!p || p->state == PROC_ZOMBIE) { return -ESRCH; }
        return sig ? signal_send_process(p, sig, info) : 0;
    }

    int target_pgid = (pid == 0) ? current_proc()->pgid : -pid;
    int found = 0, rc = 0;
    // proc_lock (rank PROCTABLE=0) is held across sig_lock
    // (rank PROCESS=1). Ascending, so legal -- never reverse it.
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct process *p = proc_list; p; p = p->next) {
        if (p->state == PROC_ZOMBIE) { continue; }
        if (pid != -1 && p->pgid != target_pgid) { continue; }
        found = 1;
        if (sig) {
            int r = signal_send_process(p, sig, info);
            if (r != 0) { rc = r; }
        }
    }
    spin_unlock_irqrestore(&proc_lock, f);
    return found ? rc : -ESRCH;
}

int signal_tkill(int tgid, int tid, int sig, struct siginfo *info) {
    if (sig < 0 || sig >= NSIG) { return -EINVAL; }
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct process *p = proc_list; p; p = p->next) {
        if (tgid > 0 && p->pid != tgid) { continue; }
        for (struct thread *t = p->threads; t; t = t->proc_next) {
            if (t->tid != tid) { continue; }
            spin_unlock_irqrestore(&proc_lock, f);
            return sig ? signal_send_thread(t, sig, info) : 0;
        }
    }
    spin_unlock_irqrestore(&proc_lock, f);
    return -ESRCH;
}

void proc_get(struct process *p) { p->refcount++; }

// Drops one live-thread reference. On the last one, frees the address
// space and the file descriptors, and turns the process into a zombie
// carrying only its exit code -- the struct itself outlives its
// address space and is freed by wait_for_pid's reap.
void proc_put(struct process *p) {
    if (--p->refcount > 0) { return; }

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
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (p->files[i].in_use && p->files[i].vn) {
            vnode_put(p->files[i].vn);
            p->files[i].vn = 0;
            p->files[i].in_use = 0;
        }
    }

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
        // the refcount to zero and frees the address space.
        for (struct thread *t = p->threads; t; t = t->proc_next) {
            if (t != self) { thread_kill(t); }
        }
    }

    thread_exit_self(code);
}

// Linux-compatible wait status encoding.
static int encode_status(struct process *p) {
    if (p->exit_signal) { return p->exit_signal & 0x7f; }   // signalled
    return (p->exit_code & 0xff) << 8;                      // exited
}
#define STATUS_STOPPED(sig)  (0x7f | ((sig) << 8))

// Frees a zombie's threads and struct. Safe only once nothing is
// running on any of them.
static void proc_reap(struct process *p) {
    struct thread *z = p->zombies;
    while (z) {
        struct thread *next = z->proc_next;
        pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
        kfree(z->xstate);
        kfree(z);
        z = next;
    }
    p->zombies = 0;

    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct process **pp = &proc_list;
    while (*pp && *pp != p) { pp = &(*pp)->next; }
    if (*pp) { *pp = p->next; }
    spin_unlock_irqrestore(&proc_lock, f);

    kfree(p);
}

// POSIX-shaped, and what musl's waitpid maps onto. NeoOS's own wait()
// is kept beside it rather than replaced -- see docs/stdlib.md.
int64_t wait4(int pid, int *status, int options) {
    struct process *self = current_proc();
    if (!self) { return -ECHILD; }   // a kernel thread has no children

    for (;;) {
        int found = 0;
        for (struct process *p = proc_list; p; p = p->next) {
            if (p->parent_pid != self->pid) { continue; }
            if (pid > 0  && p->pid  != pid)        { continue; }
            if (pid == 0 && p->pgid != self->pgid) { continue; }
            if (pid < -1 && p->pgid != -pid)       { continue; }
            found = 1;

            if (p->state == PROC_ZOMBIE) {
                int st = encode_status(p);
                int reaped = p->pid;
                if (status) { *status = st; }
                proc_reap(p);
                return reaped;
            }
            if ((options & WUNTRACED) && p->stopped_count > 0 && !p->stop_reported) {
                p->stop_reported = 1;
                if (status) { *status = STATUS_STOPPED(SIGSTOP); }
                return p->pid;
            }
        }
        if (!found) { return -ECHILD; }
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
    struct process *p = proc_find(pid);
    if (!p) { return -1; }

    while (p->state != PROC_ZOMBIE) {
        if (waitq_sleep(&p->exit_waiters, 0) == -EINTR) { return -EINTR; }
    }

    int st = encode_status(p);
    proc_reap(p);
    return ((st & 0x7f) == 0) ? ((st >> 8) & 0xff) : -(st & 0x7f);
}
