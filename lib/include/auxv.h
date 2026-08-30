#ifndef NEOOS_AUXV_H
#define NEOOS_AUXV_H

// The auxiliary vector: what the kernel tells a program about itself at
// startup, beyond argc and argv. Types and values are Linux's, because
// this is the interface musl's __libc_start_main reads and it cannot be
// shimmed -- the vector is already on the stack by the time any code
// runs.

#define AT_NULL   0
#define AT_PHDR   3    // virtual address of the program headers
#define AT_PHENT  4    // size of one program header
#define AT_PHNUM  5    // number of program headers
#define AT_PAGESZ 6    // 4096
#define AT_BASE   7    // interpreter base; absent -- no dynamic linker yet
#define AT_ENTRY  9    // the image's entry point
#define AT_RANDOM 25   // sixteen bytes; see docs/abi-compatibility.md

// Returns the value for `type`, or 0 if the kernel did not supply it.
// Linux's getauxval(3) signature and semantics, minus errno.
unsigned long getauxval(unsigned long type);

// The environment. Empty for now -- the kernel supplies no environment
// strings -- but present and NULL-terminated, so code that walks it
// works rather than dereferencing nothing.
extern char **environ;

#endif
