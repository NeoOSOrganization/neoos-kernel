// kernel/syscall/sys_mem.c -- Address space: mapping, protection, and the thread pointer.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "lib/rand.h"
#include "drivers/char/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "errno.h"
#include "sync/lock.h"
#include "ipc/signal.h"
#include "ipc/futex.h"
#include "ipc/pipe.h"
#include "drivers/char/timer.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "arch/cpu_local.h"
#include "arch/msr.h"
#include "smp/smp.h"
#include "net/socket.h"
#include "ipc/memfd.h"

int64_t sys_mmap(struct syscall_args *a) {
    // mmap takes SIX arguments. A handler receives only a1-a4, but
    // struct syscall_frame begins r9, r8 and syscall_entry.asm pushes
    // those BEFORE its argument shuffle -- so args 5 and 6 are
    // frame->r8 and frame->r9, and the ABI needs no extending.
    uint64_t addr = a->a1, len = a->a2;
    uint32_t prot = (uint32_t)a->a3, flags = (uint32_t)a->a4;
    int64_t  fd   = (int64_t)a->frame->r8;

    // A device fd whose file_ops implements mmap (only /dev/fb0 in M1a)
    // handles its own placement and backing.
    if (fd >= 0) {
        struct file_descriptor *f = fd_get(current_proc(), (int)fd);
        if (!f) { return -EBADF; }
        uint64_t off = (uint64_t)a->frame->r9;

        // A device with its own mmap (/dev/fb0, memfd) places and backs
        // itself.
        if (f->ops && f->ops->mmap) {
            struct mmap_req req = { addr, len, prot, flags, off, 0 };
            int64_t rc = file_mmap(f, &req);
            return rc < 0 ? rc : (int64_t)req.out_addr;
        }

        // An ordinary file. MAP_PRIVATE is what a dynamic linker uses
        // and what this supports; MAP_SHARED needs a page cache that
        // does not exist, and answering -ENOSYS is better than
        // pretending a shared mapping is shared when it is not.
        if (!f->vn) { return -ENODEV; }
        if (flags & MAP_SHARED) { return -ENOSYS; }
        if (off & (PMM_FRAME_SIZE - 1)) { return -EINVAL; }
        return vma_mmap_file(current_proc(), addr, len, prot, flags, f->vn, off);
    }

    // Anonymous only otherwise; the dynamic linker adds file-backed
    // mappings when it needs them.
    if (!(flags & MAP_ANONYMOUS)) { return -ENOSYS; }
    return vma_mmap(current_proc(), addr, len, prot, flags);
}

int64_t sys_munmap(struct syscall_args *a) {
    return vma_munmap(current_proc(), a->a1, a->a2);
}

int64_t sys_mprotect(struct syscall_args *a) {
    return vma_mprotect(current_proc(), a->a1, a->a2, (uint32_t)a->a3);
}

// mlock(addr, len) -- a genuine no-op success, not a lie dressed up as
// one: NeoOS has no swap and never pages out anonymous memory, so
// every resident page already satisfies mlock(2)'s promise ("this
// memory will not be paged out") before this function does anything.
// The one check kept is the one real Linux would also make -- that
// the range names actual mapped memory (EFAULT/ENOMEM on Linux for a
// range that does not) -- rounded to whole pages first, matching
// mlock(2)'s own documented page-alignment behaviour.
//
// Found missing via dotnet NativeAOT's CoreCLR startup (GC card
// table / write-barrier metadata pinning during RhInitialize). See
// docs/stdlib.md. No munlock companion yet -- add one the same way,
// if and when something is found calling it.
// madvise(addr, len, advice) -- see the constant's own comment in
// syscall_nr.h for why this is a genuine no-op success rather than
// -ENOSYS or a lie: every advice value is purely a hint NeoOS has
// nothing to act on, and the one real failure mode Linux has (a range
// that names no mapping at all) is the one check kept.
int64_t sys_madvise(struct syscall_args *a) {
    uint64_t addr = a->a1, len = a->a2;
    if (len == 0) { return 0; }

    uint64_t start = addr & ~(uint64_t)0xFFF;
    uint64_t end   = (addr + len + 0xFFF) & ~(uint64_t)0xFFF;
    if (end <= start) { return -EINVAL; }   // overflow

    struct process *p = current_proc();
    if (!p) { return -ESRCH; }
    if (!vma_range_mapped(p, start, end - start)) { return -ENOMEM; }
    return 0;
}

// mremap(old_addr, old_size, new_size, flags) -- see vma_mremap's own
// comment for the deliberately narrow semantics. Found missing (and
// its absence a real, confirmed cause of a GC crash, not just a
// tidiness gap) chasing a real .NET GC.Collect() FailFast: musl's
// pthread_getattr_np() probes the MAIN thread's own stack size via
// repeated mremap() calls expecting -ENOMEM exactly at the real
// boundary; with mremap() ENOSYS'd, the probe's own retry loop
// (`while (mremap(...) == MAP_FAILED && errno==ENOMEM)`) never even
// entered -- ENOSYS isn't ENOMEM -- so it reported the main thread's
// stack as 1 page instead of its real size, and the GC's own
// conservative stack scan, bounded by that lie, walked past its
// self-imposed limit and FailFast'd resolving a return address it
// had no business reading yet.
int64_t sys_mremap(struct syscall_args *a) {
    struct process *p = current_proc();
    if (!p) { return -ESRCH; }
    return vma_mremap(p, a->a1, a->a2, a->a3, (uint32_t)a->a4);
}

int64_t sys_mlock(struct syscall_args *a) {
    uint64_t addr = a->a1, len = a->a2;
    if (len == 0) { return 0; }

    uint64_t start = addr & ~(uint64_t)0xFFF;
    uint64_t end   = (addr + len + 0xFFF) & ~(uint64_t)0xFFF;
    if (end <= start) { return -EINVAL; }   // overflow

    struct process *p = current_proc();
    if (!p) { return -ESRCH; }
    if (!vma_range_mapped(p, start, end - start)) { return -ENOMEM; }
    return 0;
}

int64_t sys_arch_prctl(struct syscall_args *a) {
    struct thread *t = current_thread();
    if (!t || !t->proc) { return -ESRCH; }

    if ((int)a->a1 == ARCH_SET_FS) {
        uint64_t addr = (uint64_t)a->a2;
        // Must be a canonical user address. A non-canonical value makes
        // the WRMSR below #GP inside the kernel, which is a user-
        // triggerable fault rather than a user error, so it is rejected
        // here. Linux does the same check for the same reason.
        if (addr >= USER_ADDR_LIMIT) { return -EPERM; }
        t->fs_base = addr;
        // Written immediately as well as recorded: this thread is
        // running right now and expects the change to take effect
        // before the syscall returns, not at its next context switch.
        wrmsr(MSR_FS_BASE, addr);
        // AND keep schedule()'s per-CPU FS_BASE cache coherent. Without
        // this the cache holds a value the MSR no longer has, and the
        // next thread switched onto this CPU whose fs_base happens to
        // equal the stale cache is skipped by the "nothing changed"
        // fast path -- and runs with THIS thread's TLS base. Two
        // threads then read/write one __thread storage area: the
        // concurrent-request-crash investigation's suspect #3, made
        // real by the comment in sched.c that said this could not
        // happen.
        this_cpu()->fs_base_loaded = addr;
        return 0;
    }
    if ((int)a->a1 == ARCH_GET_FS) {
        uint64_t out = a->a2;
        uint64_t missed = copy_to_user((void *)(uintptr_t)out, &t->fs_base, sizeof t->fs_base);
        if (missed > 0) { return -EFAULT; }
        return 0;
    }
    // ARCH_SET_GS / ARCH_GET_GS are deliberately absent. NeoOS uses GS
    // for its own per-CPU block: on kernel entry GS_BASE holds the
    // per-CPU pointer and KERNEL_GS_BASE holds userland's, so setting
    // "the user GS base" from a syscall means writing the swapped MSR,
    // and getting that subtly wrong corrupts this_cpu() for every
    // thread on the CPU. No libc uses it on x86-64. Recorded in
    // docs/stdlib.md.
    return -EINVAL;
}

// getrandom(buf, len, flags) -- Linux's shape exactly.
//
// Always fills the whole buffer and never blocks: NeoOS's CSPRNG is
// seeded once at boot and is always ready, so GRND_RANDOM and
// GRND_NONBLOCK have nothing to select between. Unknown flags are
// REJECTED rather than ignored, because a caller passing a flag this
// kernel does not implement is asking for a guarantee it would not be
// getting.
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

#define GETRANDOM_STAGE_MAX 256

int64_t sys_getrandom(struct syscall_args *a) {
    uint64_t uptr  = a->a1;
    uint64_t len   = a->a2;
    unsigned flags = (unsigned)a->a3;

    if (flags & ~(unsigned)(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) {
        return -EINVAL;
    }
    if (len == 0) { return 0; }
    if (!uptr) { return -EFAULT; }

    // Staged through a small kernel buffer, same reasoning as
    // sys_file.c's read_to_user: rand_bytes fills its destination
    // directly with no user-copy awareness, so the safe copy happens
    // here. The stage buffer is small (256 bytes, not a full page) --
    // getrandom() calls are typically for a key or a seed, never a bulk
    // transfer, so there is no reason to reserve a page of stack for it.
    uint8_t stage[GETRANDOM_STAGE_MAX];
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done < GETRANDOM_STAGE_MAX ? len - done : GETRANDOM_STAGE_MAX;
        rand_bytes(stage, chunk);
        uint64_t missed = copy_to_user((void *)(uintptr_t)(uptr + done), stage, chunk);
        if (missed > 0) { return done > 0 ? (int64_t)done : -EFAULT; }
        done += chunk;
    }
    return (int64_t)len;
}

// memfd_create(name, name_len, flags). The name is diagnostic only --
// nothing is created in any filesystem -- but it carries a length like
// every other path-ish argument NeoOS takes, so the kernel never walks
// a user pointer looking for a NUL.
int64_t sys_memfd_create(struct syscall_args *a) {
    return memfd_create_fd((const char *)(uintptr_t)a->a1, a->a2, (unsigned)a->a3);
}
