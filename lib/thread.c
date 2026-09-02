#include <thread.h>
#include <errno.h>
#include <tls.h>

// The kernel starts a thread at RIP=entry with RDI=arg, but a raw `fn`
// that simply returned would fall off the end of its stack. So the
// kernel always enters this trampoline, which calls fn and then exits
// the thread properly.
//
// lib/ has no malloc, so the (fn, arg) pairs live in a static table
// sized to the kernel's MAX_THREADS_PER_PROC (CS4 raised that from 16).
//
// A slot is held only until the new thread has copied fn and arg out of
// it, so this bounds concurrent thread_create CALLS in flight rather
// than live threads -- but a burst of creations hits it all the same,
// and at 16 that was any attempt to start more than sixteen at once.
#define MAX_THREAD_SLOTS 1024

struct thread_start {
    int    in_use;
    void (*fn)(void *);
    void  *arg;
};

static struct thread_start slots[MAX_THREAD_SLOTS];

extern int  __sys_thread_create(unsigned long entry, unsigned long arg);
extern void __sys_thread_exit(int code) __attribute__((noreturn));
extern int  __sys_thread_join(int tid, int *code);
extern int  __sys_thread_self(void);

static void thread_trampoline(void *raw) {
    struct thread_start *s = (struct thread_start *)raw;
    void (*fn)(void *) = s->fn;
    void  *arg = s->arg;
    // Release with a store-release: fn and arg were read above, and the
    // creator must not see the slot free until that has happened.
    __atomic_store_n(&s->in_use, 0, __ATOMIC_RELEASE);
    // (the slot is only needed until the thread starts)

    // Every thread needs its OWN TLS block: that is what thread-local
    // means. The kernel starts a new thread with %fs at zero, so this
    // must happen before fn touches any __thread variable -- and it has
    // to happen HERE, on the new thread, since arch_prctl acts on the
    // caller.
    if (__tls_setup_self() != 0) { __sys_thread_exit(127); }

    fn(arg);
    __sys_thread_exit(0);
}

int thread_create(thread_t *out, void (*fn)(void *), void *arg) {
    // Claim the slot ATOMICALLY. Testing in_use and then setting it is
    // two steps, and two threads calling thread_create at the same time
    // both passed the test and both took the same slot -- so one of them
    // started running the other's function, with the other's argument.
    struct thread_start *s = 0;
    for (int i = 0; i < MAX_THREAD_SLOTS; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&slots[i].in_use, &expected, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            s = &slots[i];
            break;
        }
    }
    if (!s) { return -EAGAIN; }
    s->fn     = fn;
    s->arg    = arg;

    int tid = __sys_thread_create((unsigned long)(void *)thread_trampoline,
                                  (unsigned long)(void *)s);
    if (tid < 0) { __atomic_store_n(&s->in_use, 0, __ATOMIC_RELEASE); return tid; }
    if (out) { *out = tid; }
    return 0;
}

void thread_exit(int code) { __sys_thread_exit(code); }
int  thread_join(thread_t t, int *exit_code) { return __sys_thread_join(t, exit_code); }
thread_t thread_self(void) { return __sys_thread_self(); }
