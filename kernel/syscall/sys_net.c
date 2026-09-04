// kernel/syscall/sys_net.c -- BSD sockets.
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
#include "smp/smp.h"
#include "net/socket.h"

int64_t sys_socket(struct syscall_args *a) {
    return socket_create((int)a->a1, (int)a->a2, (int)a->a3);
}

int64_t sys_bind(struct syscall_args *a) {
    return socket_bind((int)a->a1, (const struct k_sockaddr *)(uintptr_t)a->a2,
                       (uint32_t)a->a3);
}

int64_t sys_connect(struct syscall_args *a) {
    return socket_connect((int)a->a1, (const struct k_sockaddr *)(uintptr_t)a->a2,
                          (uint32_t)a->a3);
}

int64_t sys_listen(struct syscall_args *a) {
    return socket_listen((int)a->a1, (int)a->a2);
}

// accept(fd, addr, len) is accept4 with no flags, which is how Linux
// implements it too -- so only the four-argument form exists here and
// the shim passes 0.
int64_t sys_accept4(struct syscall_args *a) {
    return socket_accept4((int)a->a1, (struct k_sockaddr *)(uintptr_t)a->a2,
                          (uint32_t *)(uintptr_t)a->a3, (int)a->a4);
}

int64_t sys_shutdown(struct syscall_args *a) {
    return socket_shutdown((int)a->a1, (int)a->a2);
}

int64_t sys_getpeername(struct syscall_args *a) {
    return socket_getpeername((int)a->a1, (struct k_sockaddr *)(uintptr_t)a->a2,
                              (uint32_t *)(uintptr_t)a->a3);
}

int64_t sys_setsockopt(struct syscall_args *a) {
    return socket_setsockopt((int)a->a1, (int)a->a2, (int)a->a3,
                             (const void *)(uintptr_t)a->a4,
                             (uint32_t)a->frame->r8);
}

int64_t sys_getsockopt(struct syscall_args *a) {
    return socket_getsockopt((int)a->a1, (int)a->a2, (int)a->a3,
                             (void *)(uintptr_t)a->a4,
                             (uint32_t *)(uintptr_t)a->frame->r8);
}

int64_t sys_getsockname(struct syscall_args *a) {
    return socket_getsockname((int)a->a1, (struct k_sockaddr *)(uintptr_t)a->a2,
                              (uint32_t *)(uintptr_t)a->a3);
}

// sendto and recvfrom take SIX arguments, like mmap: the fifth and
// sixth arrive in r8 and r9, which syscall_entry.asm has already pushed
// into the frame before its argument shuffle.
int64_t sys_sendto(struct syscall_args *a) {
    return socket_sendto((int)a->a1, (const void *)(uintptr_t)a->a2,
                         (uint64_t)a->a3, (int)a->a4,
                         (const struct k_sockaddr *)(uintptr_t)a->frame->r8,
                         (uint32_t)a->frame->r9);
}

int64_t sys_recvfrom(struct syscall_args *a) {
    return socket_recvfrom((int)a->a1, (void *)(uintptr_t)a->a2,
                           (uint64_t)a->a3, (int)a->a4,
                           (struct k_sockaddr *)(uintptr_t)a->frame->r8,
                           (uint32_t *)(uintptr_t)a->frame->r9);
}
