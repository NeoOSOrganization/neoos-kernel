#ifndef NEOOS_THREAD_H
#define NEOOS_THREAD_H

typedef int thread_t;

// Starts `fn(arg)` on a new thread sharing this process's address
// space and file descriptors. Returns 0 and stores the tid in *out, or
// a negative <errno.h> code.
int thread_create(thread_t *out, void (*fn)(void *), void *arg);

// Ends the calling thread. When it is the last thread of the process,
// the process ends too. Never returns.
void thread_exit(int code) __attribute__((noreturn));

// Waits for `t` to exit and stores its exit code (if `exit_code` is
// non-null). Returns 0, -ESRCH if no such thread exists in this
// process, or -EDEADLK if `t` is the calling thread.
int thread_join(thread_t t, int *exit_code);

thread_t thread_self(void);

#endif
