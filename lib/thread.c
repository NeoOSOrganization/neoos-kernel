#include <thread.h>
#include <errno.h>

// The kernel starts a thread at RIP=entry with RDI=arg, but a raw `fn`
// that simply returned would fall off the end of its stack. So the
// kernel always enters this trampoline, which calls fn and then exits
// the thread properly.
//
// lib/ has no malloc, so the (fn, arg) pairs live in a static table
// sized to the kernel's MAX_THREADS_PER_PROC.
#define MAX_THREAD_SLOTS 16

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
    s->in_use = 0;   // the slot is only needed until the thread starts
    fn(arg);
    __sys_thread_exit(0);
}

int thread_create(thread_t *out, void (*fn)(void *), void *arg) {
    struct thread_start *s = 0;
    for (int i = 0; i < MAX_THREAD_SLOTS; i++) {
        if (!slots[i].in_use) { s = &slots[i]; break; }
    }
    if (!s) { return -EAGAIN; }
    s->in_use = 1;
    s->fn     = fn;
    s->arg    = arg;

    int tid = __sys_thread_create((unsigned long)(void *)thread_trampoline,
                                  (unsigned long)(void *)s);
    if (tid < 0) { s->in_use = 0; return tid; }
    if (out) { *out = tid; }
    return 0;
}

void thread_exit(int code) { __sys_thread_exit(code); }
int  thread_join(thread_t t, int *exit_code) { return __sys_thread_join(t, exit_code); }
thread_t thread_self(void) { return __sys_thread_self(); }
