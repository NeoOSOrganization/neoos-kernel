#include "mm/vma.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "smp/tlb.h"
#include "sched/proc.h"
#include "errno.h"
#include "drivers/char/serial.h"

// kernel/mm reaching into kernel/sched for struct process matches what
// kernel/fs/vfs.c already does; the alternative is threading four
// out-parameters through every call.

static uint64_t page_down(uint64_t a) { return a & ~0xFFFULL; }
static uint64_t page_up(uint64_t a)   { return (a + 0xFFF) & ~0xFFFULL; }

static struct vma *vma_find(struct process *p, uint64_t addr) {
    for (struct vma *v = p->vmas; v; v = v->next) {
        if (addr >= v->start && addr < v->end) { return v; }
        if (v->start > addr) { break; }   // sorted: no later one can match
    }
    return 0;
}

// Inserts [start,end) keeping the list sorted, merging with an adjacent
// neighbour that has identical prot and flags. Assumes the range is
// already free -- callers unmap first.
static int vma_insert(struct process *p, uint64_t start, uint64_t end,
                      uint32_t prot, uint32_t flags) {
    struct vma **pp = &p->vmas;
    while (*pp && (*pp)->end <= start) { pp = &(*pp)->next; }

    // Merge backwards.
    struct vma *prev = 0;
    for (struct vma *v = p->vmas; v && v != *pp; v = v->next) { prev = v; }
    if (prev && prev->end == start && prev->prot == prot && prev->flags == flags) {
        prev->end = end;
        struct vma *nx = prev->next;
        if (nx && nx->start == end && nx->prot == prot && nx->flags == flags) {
            prev->end = nx->end;
            prev->next = nx->next;
            kfree(nx);
        }
        return 1;
    }
    // Merge forwards.
    if (*pp && (*pp)->start == end && (*pp)->prot == prot && (*pp)->flags == flags) {
        (*pp)->start = start;
        return 1;
    }

    struct vma *v = (struct vma *)kmalloc(sizeof(struct vma));
    if (!v) { return 0; }
    v->start = start; v->end = end; v->prot = prot; v->flags = flags;
    v->next = *pp;
    *pp = v;
    return 1;
}

// Unmaps and frees every page of [start,end) that is actually present.
// Pages a mapping covers but that were never touched have no frame and
// must not be freed -- that distinction is what demand paging buys, and
// getting it wrong either leaks or double-frees.
static void unmap_range(struct process *p, uint64_t start, uint64_t end, int free_frames) {
    if (!p->pml4_phys) { return; }
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
    for (uint64_t v = start; v < end; v += PMM_FRAME_SIZE) {
        paging_unmap_from(pml4, v, free_frames);
    }
}

static int vma_munmap_locked(struct process *p, uint64_t addr, uint64_t len) {
    if (addr & 0xFFF) { return -EINVAL; }
    uint64_t start = addr, end = page_up(addr + len);
    if (end <= start) { return -EINVAL; }

    struct vma **pp = &p->vmas;
    while (*pp) {
        struct vma *v = *pp;
        if (v->end <= start) { pp = &v->next; continue; }
        if (v->start >= end) { break; }

        uint64_t lo = v->start > start ? v->start : start;
        uint64_t hi = v->end   < end   ? v->end   : end;
        unmap_range(p, lo, hi, !(v->flags & VMA_PHYS));

        if (v->start < start && v->end > end) {
            // Punching the middle: keep the head, add a tail.
            struct vma *tail = (struct vma *)kmalloc(sizeof(struct vma));
            if (!tail) { return -ENOMEM; }
            tail->start = end; tail->end = v->end;
            tail->prot = v->prot; tail->flags = v->flags;
            tail->next = v->next;
            v->end = start;
            v->next = tail;
            return 0;
        }
        if (v->start < start) {           // trim the tail
            v->end = start;
            pp = &v->next;
        } else if (v->end > end) {        // trim the head
            v->start = end;
            break;
        } else {                          // consumed entirely
            *pp = v->next;
            kfree(v);
        }
    }
    return 0;
}

static int64_t vma_mmap_locked(struct process *p, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags) {
    if (len == 0) { return -EINVAL; }
    // W^X: NeoOS refuses a mapping that is both writable and executable.
    // Linux permits it (subject to lockdown / SELinux); a JIT that needs
    // both must map twice. Recorded in docs/stdlib.md.
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) { return -EINVAL; }
    len = page_up(len);

    if (flags & MAP_FIXED) {
        if (addr & 0xFFF) { return -EINVAL; }
        // Overlapping an existing mapping REPLACES it, as POSIX
        // requires and as the dynamic linker will rely on.
        //
        // The *_locked form: we already hold p->mm_lock, and the public
        // vma_munmap would retake it and self-deadlock.
        vma_munmap_locked(p, addr, len);
    } else {
        addr = p->mmap_next;
        if (addr + len > MMAP_LIMIT || addr + len < addr) { return -ENOMEM; }
        p->mmap_next = addr + len;
    }
    if (!vma_insert(p, addr, addr + len, prot, flags)) { return -ENOMEM; }
    // Nothing is mapped yet: the fault handler populates on first touch.
    return (int64_t)addr;
}

static int64_t vma_map_phys_locked(struct process *p, uint64_t phys, uint64_t len,
                uint32_t prot) {
    if (len == 0) { return -EINVAL; }
    if (prot & PROT_EXEC) { return -EINVAL; }          // W^X: no executable device map
    len = page_up(len);
    uint64_t addr = p->mmap_next;
    if (addr + len > MMAP_LIMIT || addr + len < addr) { return -ENOMEM; }
    if (!vma_insert(p, addr, addr + len, prot, MAP_SHARED | VMA_PHYS)) { return -ENOMEM; }
    p->mmap_next = addr + len;

    uint64_t pf = PAGE_USER | PAGE_NO_EXECUTE | PAGE_NOFREE;
    if (prot & PROT_WRITE) { pf |= PAGE_WRITABLE; }
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
    for (uint64_t off = 0; off < len; off += PMM_FRAME_SIZE) {
        if (paging_map_into(pml4, addr + off, phys + off, pf) != 0) {
            // Roll back the pages mapped so far, then the vma. Frames are
            // device-owned, so don't free them.
            for (uint64_t b = 0; b < off; b += PMM_FRAME_SIZE) {
                paging_unmap_from(pml4, addr + b, 0);
            }
            vma_munmap_locked(p, addr, len);
            return -ENOMEM;
        }
    }
    return (int64_t)addr;
}

// Splits `v` at `at`, leaving `v` as [v->start, at) and inserting a new
// vma [at, old_end) after it with the same prot and flags. Returns 0 on
// allocation failure, in which case nothing has changed.
static int vma_split_at(struct vma *v, uint64_t at) {
    struct vma *tail = (struct vma *)kmalloc(sizeof(struct vma));
    if (!tail) { return 0; }
    tail->start = at;
    tail->end   = v->end;
    tail->prot  = v->prot;
    tail->flags = v->flags;
    tail->next  = v->next;
    v->end  = at;
    v->next = tail;
    return 1;
}

static int vma_mprotect_locked(struct process *p, uint64_t addr, uint64_t len, uint32_t prot) {
    if (addr & 0xFFF) { return -EINVAL; }
    if (len == 0) { return 0; }
    // W^X, same rule as vma_mmap: no page becomes W and X at once.
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) { return -EINVAL; }
    uint64_t start = addr, end = page_up(addr + len);
    if (end <= start) { return -EINVAL; }

    // A partial mprotect SPLITS the mapping it lands in.
    //
    // This used to return -EINVAL, on the reasoning that "musl only
    // protects whole mappings". That is not true of musl's allocator:
    // mallocng reserves a large region PROT_NONE and then mprotects
    // PIECES of it read-write as it hands them out. Refusing the split
    // made every malloc() fail with EINVAL -- which is what a real musl
    // program hit first, before it could allocate a single byte.
    for (struct vma *v = p->vmas; v; v = v->next) {
        if (v->end <= start) { continue; }
        if (v->start >= end) { break; }

        // Trim a leading piece that keeps its old protection.
        if (v->start < start) {
            if (!vma_split_at(v, start)) { return -ENOMEM; }
            continue;   // the tail is now v->next; revisit it
        }
        // Trim a trailing piece that keeps its old protection.
        if (v->end > end) {
            if (!vma_split_at(v, end)) { return -ENOMEM; }
        }

        v->prot = prot;

        // Rewrite the PTEs of everything already populated, keeping the
        // frames and their contents.
        //
        // This used to unmap-and-free the range whenever the new prot
        // was not writable -- which includes plain PROT_READ, so
        // mprotect(p, n, PROT_READ) silently threw the data away. A page
        // that has been written must survive being made read-only; that
        // is most of what mprotect is for.
        //
        // Widening is applied here too rather than left to the next
        // fault: a page parked PROT_NONE is not present, but a page
        // being made writable again IS, so no fault would ever arrive to
        // re-evaluate it. paging_setprot_from is the one that refuses to
        // hand PAGE_WRITABLE to a copy-on-write page.
        if (p->pml4_phys) {
            uint64_t pf = 0;
            if (prot != PROT_NONE) {
                pf = PAGE_PRESENT | PAGE_USER;
                if (prot & PROT_WRITE) { pf |= PAGE_WRITABLE; }
                if (!(prot & PROT_EXEC)) { pf |= PAGE_NO_EXECUTE; }
            }
            uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
            for (uint64_t a = v->start; a < v->end; a += PMM_FRAME_SIZE) {
                paging_setprot_from(pml4, a, pf);
            }
        }
    }
    return 0;
}

static int vma_fault_locked(struct process *p, uint64_t addr, int write) {
    struct vma *v = vma_find(p, addr);
    if (!v) { return 0; }                                  // -> SIGSEGV
    if (v->prot == PROT_NONE) { return 0; }
    if (write && !(v->prot & PROT_WRITE)) { return 0; }
    // A device mapping is populated eagerly at creation; a not-present
    // fault inside one means the page was unmapped under it -> SIGSEGV.
    if (v->flags & VMA_PHYS) { return 0; }

    uint64_t frame = pmm_alloc(0);
    if (!frame) { return 0; }
    // Zero here rather than calling sched/'s zero_frames: kernel/mm must
    // not reach into kernel/sched for code, and a fresh frame handed to
    // userland must never carry another process's bytes.
    uint64_t *z = (uint64_t *)phys_to_virt(frame);
    for (unsigned i = 0; i < PMM_FRAME_SIZE / sizeof(uint64_t); i++) { z[i] = 0; }

    uint64_t pf = PAGE_USER;
    if (v->prot & PROT_WRITE) { pf |= PAGE_WRITABLE; }
    if (!(v->prot & PROT_EXEC)) { pf |= PAGE_NO_EXECUTE; }

    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
    paging_map_into(pml4, page_down(addr), frame, pf);
    return 1;
}

static void vma_destroy_all_locked(struct process *p) {
    struct vma *v = p->vmas;
    while (v) {
        struct vma *next = v->next;
        unmap_range(p, v->start, v->end, !(v->flags & VMA_PHYS));
        kfree(v);
        v = next;
    }
    p->vmas = 0;
}

// ---------------------------------------------------------------- selftest


// ---- locked public entry points -------------------------------------
//
// Only these take p->mm_lock; the *_locked bodies above are called from
// within an already-locked region. The lock ranks at LOCK_RANK_MM (3),
// beneath which the allocator (HEAP 12, PMM 13) is legally reachable,
// and none of these paths sleep or reach schedule().
//
// They are also where the TLB is settled, because it can only be
// settled with mm_lock RELEASED: tlb_shootdown panics if any lock is
// held, since a target CPU spinning on that same mm_lock with
// interrupts off could never acknowledge the IPI.
//
// Skipping this was a permanent frame leak, not just a stale-TLB bug.
// paging_unmap_from does not call pmm_free at all -- it calls
// tlb_defer_free, and the deferred queue is drained by exactly one
// thing, the tail of tlb_shootdown. With no shootdown after boot, every
// frame ever munmap'd, mprotect'd away or freed with a thread stack sat
// in that queue forever.
//
// Targeted at this process's own pml4, which is both correct and the
// difference between an munmap costing a local CR3 reload and an munmap
// broadcasting an IPI to every CPU in the machine: only a CPU running a
// thread of THIS process can hold a translation for the pages it just
// unmapped, and tlb_defer_free tagged the frames with the same pml4 so
// the drain releases exactly them.

static void vma_tlb_settle(struct process *p) {
    // A scratch process with no address space (vma_selftest) never
    // mapped anything and never deferred anything.
    if (p->pml4_phys) { tlb_shootdown(p->pml4_phys); }
}

int vma_munmap(struct process *p, uint64_t addr, uint64_t len) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    int rc = vma_munmap_locked(p, addr, len);
    spin_unlock_irqrestore(&p->mm_lock, f);
    vma_tlb_settle(p);
    return rc;
}

int64_t vma_mmap(struct process *p, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    int64_t rc = vma_mmap_locked(p, addr, len, prot, flags);
    spin_unlock_irqrestore(&p->mm_lock, f);
    // MAP_FIXED over a live mapping unmaps it first; a plain mmap maps
    // nothing and has nothing to settle.
    if (flags & MAP_FIXED) { vma_tlb_settle(p); }
    return rc;
}

int64_t vma_map_phys(struct process *p, uint64_t phys, uint64_t len, uint32_t prot) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    int64_t rc = vma_map_phys_locked(p, phys, len, prot);
    spin_unlock_irqrestore(&p->mm_lock, f);
    // Only the failure path unmaps anything (its own rollback).
    if (rc < 0) { vma_tlb_settle(p); }
    return rc;
}

int vma_mprotect(struct process *p, uint64_t addr, uint64_t len, uint32_t prot) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    int rc = vma_mprotect_locked(p, addr, len, prot);
    spin_unlock_irqrestore(&p->mm_lock, f);
    // Unconditional: the PTEs were rewritten in place, so another CPU's
    // TLB can still hold the permission this call just took away.
    vma_tlb_settle(p);
    return rc;
}

int vma_fault(struct process *p, uint64_t addr, int write) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    int rc = vma_fault_locked(p, addr, write);
    spin_unlock_irqrestore(&p->mm_lock, f);
    return rc;
}

void vma_destroy_all(struct process *p) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    vma_destroy_all_locked(p);
    spin_unlock_irqrestore(&p->mm_lock, f);
    vma_tlb_settle(p);
}

// fork's other half. See the header for why the page tables alone are
// not enough.
//
// Only `src`'s lock is taken. Two locks of equal rank at once is exactly
// what the rank checker forbids, and `dst` does not need one: fork_task
// has not created its thread yet, so no CPU but this one can reach the
// child's vma list. (proc_alloc does publish the child in the process
// table before this runs, but the only callbacks that walk it -- kill,
// tkill, reparent -- touch signals and parent pids, never mappings.)
//
// The list is built in order with a tail pointer rather than by
// repeatedly walking to the end: vma_insert's merge logic is wrong here
// anyway, since the parent's list may legitimately hold adjacent
// entries that must NOT be merged (a PROT_NONE reservation next to the
// RW piece mprotect split out of it is the shape musl's allocator
// makes), and copying is not the place to re-run placement policy.
int vma_copy_all(struct process *dst, struct process *src) {
    uint64_t f = spin_lock_irqsave(&src->mm_lock);

    struct vma *head = 0, *tail = 0;
    for (struct vma *v = src->vmas; v; v = v->next) {
        struct vma *c = (struct vma *)kmalloc(sizeof(struct vma));
        if (!c) {
            spin_unlock_irqrestore(&src->mm_lock, f);
            while (head) { struct vma *n = head->next; kfree(head); head = n; }
            dst->vmas = 0;
            return 0;
        }
        c->start = v->start; c->end = v->end;
        c->prot  = v->prot;  c->flags = v->flags;
        c->next  = 0;
        if (tail) { tail->next = c; } else { head = c; }
        tail = c;
    }

    // The cursor comes too. Without it the child's next mmap restarts at
    // MMAP_BASE and hands back an address it already holds.
    dst->mmap_next = src->mmap_next;
    spin_unlock_irqrestore(&src->mm_lock, f);

    dst->vmas = head;
    return 1;
}

void vma_forget_all(struct process *p) {
    uint64_t f = spin_lock_irqsave(&p->mm_lock);
    struct vma *v = p->vmas;
    p->vmas = 0;
    spin_unlock_irqrestore(&p->mm_lock, f);
    while (v) { struct vma *n = v->next; kfree(v); v = n; }
}

static int vma_count(struct process *p) {
    int n = 0;
    for (struct vma *v = p->vmas; v; v = v->next) { n++; }
    return n;
}

void vma_selftest(void) {
    // Runs against a scratch process so the caller's own mappings are
    // untouched. pml4_phys is 0, so unmap_range is a no-op and nothing
    // real is mapped -- this exercises the LIST, which is the part with
    // the interesting edge cases.
    struct process t;
    for (unsigned i = 0; i < sizeof(t); i++) { ((uint8_t *)&t)[i] = 0; }
    t.mmap_next = MMAP_BASE;
    // The scratch process never went through proc_alloc, so its lock
    // needs initialising by hand -- the vma entry points take it.
    spin_init(&t.mm_lock, LOCK_RANK_MM, "mm-selftest");

    int64_t a = vma_mmap(&t, 0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS);
    if (a < 0 || (uint64_t)a < MMAP_BASE || (uint64_t)a >= MMAP_LIMIT) {
        serial_write_string("[vma] selftest FAILED: mmap address out of range\n");
        return;
    }
    int64_t b = vma_mmap(&t, 0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS);
    if (b == a) {
        serial_write_string("[vma] selftest FAILED: second mmap overlaps\n");
        return;
    }
    // Adjacent with identical prot/flags: the two must have merged.
    if (vma_count(&t) != 1) {
        serial_write_string("[vma] selftest FAILED: adjacent mappings not merged\n");
        return;
    }

    // Punch the middle of the merged region -> head + tail.
    if (vma_munmap(&t, (uint64_t)a, 4096) != 0) {
        serial_write_string("[vma] selftest FAILED: munmap head\n");
        return;
    }
    if (vma_count(&t) != 1 || t.vmas->start != (uint64_t)b) {
        serial_write_string("[vma] selftest FAILED: head trim wrong\n");
        return;
    }

    uint64_t big = (uint64_t)vma_mmap(&t, 0, 3 * 4096, PROT_READ, MAP_ANONYMOUS);
    int before = vma_count(&t);
    if (vma_munmap(&t, big + 4096, 4096) != 0) {
        serial_write_string("[vma] selftest FAILED: munmap middle\n");
        return;
    }
    if (vma_count(&t) != before + 1) {
        serial_write_string("[vma] selftest FAILED: middle punch did not split\n");
        return;
    }

    // A hole must not be found, the pages either side must be.
    if (vma_find(&t, big + 4096)) {
        serial_write_string("[vma] selftest FAILED: hole still mapped\n");
        return;
    }
    if (!vma_find(&t, big) || !vma_find(&t, big + 2 * 4096)) {
        serial_write_string("[vma] selftest FAILED: split lost a side\n");
        return;
    }

    vma_destroy_all(&t);

    // MM sits at rank 3: below the VFS ranks, because a demand-paging
    // fault takes it and then reads through the filesystem; and below
    // RUNQUEUE, because such a fault can sleep.
    uint64_t lf = spin_lock_irqsave(&t.mm_lock);
    int fs_ok   = lock_rank_ok(LOCK_RANK_MOUNTTABLE);
    int rq_ok   = lock_rank_ok(LOCK_RANK_RUNQUEUE);
    int heap_ok = lock_rank_ok(LOCK_RANK_HEAP);
    spin_unlock_irqrestore(&t.mm_lock, lf);
    if (!fs_ok || !rq_ok || !heap_ok) {
        serial_write_string("[vma] selftest FAILED: mm rank does not dominate fs/runqueue/heap\n");
        return;
    }
    if (vma_count(&t) != 0) {
        serial_write_string("[vma] selftest FAILED: destroy left mappings\n");
        return;
    }
    serial_write_string("[vma] selftest passed\n");
}
