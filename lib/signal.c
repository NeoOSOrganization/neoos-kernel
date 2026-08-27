#include <signal.h>
#include <thread.h>
#include <errno.h>

// SA_RESTORER is mandatory on x86-64, as on Linux: the kernel never
// injects a trampoline onto the user stack, so every sigaction() must
// supply one. It simply re-enters the kernel to restore the context the
// frame holds.
#define SA_RESTORER 0x04000000

__asm__(
    ".globl __restore_rt\n"
    "__restore_rt:\n"
    "   mov $23, %eax\n"        /* SYS_RT_SIGRETURN */
    "   syscall\n"
);
extern void __restore_rt(void);

// Mirrors kernel/signal.h's struct k_sigaction exactly.
struct k_sigaction {
    void        (*handler)(int);
    unsigned long flags;
    void        (*restorer)(void);
    unsigned long mask;
};

extern long __sys_rt_sigaction(int sig, const struct k_sigaction *act,
                               struct k_sigaction *old);

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    struct k_sigaction ka, ko;
    if (act) {
        ka.handler  = act->sa_handler;
        ka.flags    = act->sa_flags | SA_RESTORER;
        ka.restorer = __restore_rt;
        ka.mask     = act->sa_mask;
    }
    long rc = __sys_rt_sigaction(sig, act ? &ka : 0, old ? &ko : 0);
    if (rc == 0 && old) {
        old->sa_handler = ko.handler;
        old->sa_flags   = ko.flags & ~SA_RESTORER;
        old->sa_mask    = ko.mask;
    }
    return (int)rc;
}

int raise(int sig) { return tkill(thread_self(), sig); }
