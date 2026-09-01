// kernel/syscall/sys_mem.c -- Address space: mapping, protection, and the thread pointer.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
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
#include "arch/cpu_local.h"
#include "arch/msr.h"
#include "smp/smp.h"
#include "net/socket.h"

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
        struct mmap_req req = { addr, len, prot, flags, (uint64_t)a->frame->r9, 0 };
        int64_t rc = file_mmap(f, &req);
        return rc < 0 ? rc : (int64_t)req.out_addr;
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
        return 0;
    }
    if ((int)a->a1 == ARCH_GET_FS) {
        uint64_t *out = (uint64_t *)(uintptr_t)a->a2;
        if (!user_range_writable((uint64_t)(uintptr_t)out, sizeof(uint64_t))) {
            return -EFAULT;
        }
        *out = t->fs_base;
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
