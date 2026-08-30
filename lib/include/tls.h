#ifndef NEOOS_TLS_H
#define NEOOS_TLS_H

// Thread-local storage.
//
// There is no API here to call: declaring a variable `__thread` is the
// whole interface, and the C runtime does the rest. What this header
// exposes is the two pieces underneath, for the runtime itself and for
// anything that needs to reason about the thread pointer.

// x86-64 arch_prctl codes. Linux's values.
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

// Sets or reads this thread's pointer (the %fs base). Returns 0, or a
// negative errno.h code. ARCH_SET_GS and ARCH_GET_GS are NOT supported
// and return -EINVAL: NeoOS uses %gs for its own per-CPU block. See
// docs/stdlib.md.
int arch_prctl(int code, unsigned long addr);

// Allocates this thread's TLS block from the image's PT_TLS template
// and installs it. Called by the C runtime for the main thread and by
// thread_create for every other; a program should not need it.
// Returns 0, or a negative errno.h code.
int __tls_setup_self(void);

#endif
