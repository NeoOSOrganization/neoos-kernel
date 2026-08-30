// lib/start.c -- what runs between _start and main.
//
// Two jobs: remember the process entry information the kernel put on
// the stack, and set up thread-local storage before any of it can be
// touched.
//
// TLS is the reason the auxiliary vector had to exist at all. A static
// executable has no dynamic section, so nothing in the image says where
// its own PT_TLS template is; the only way to find it is to walk the
// program headers, and the only way to find THOSE is AT_PHDR. That is
// why the kernel now builds a real SysV entry stack instead of passing
// argc = 0.

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <auxv.h>
#include <elf.h>
#include <tls.h>
#include <sys/mman.h>

extern int main(int argc, char **argv);

static unsigned long *auxv_table;
char **environ;

unsigned long getauxval(unsigned long type) {
    if (!auxv_table) { return 0; }
    for (unsigned long *p = auxv_table; p[0] != AT_NULL; p += 2) {
        if (p[0] == type) { return p[1]; }
    }
    return 0;
}

// --------------------------------------------------------------- TLS
//
// x86-64 uses "variant II": the thread-control block sits AT the thread
// pointer and the TLS block immediately BELOW it, so a thread-local
// variable is reached at a NEGATIVE offset from %fs. The layout is:
//
//     [ .tdata | .tbss ][ TCB ]
//     ^ tp - tls_size   ^ tp == %fs base
//
// and %fs:0 holds a pointer to itself. Nothing in this library reads
// that self-pointer, but it is part of the ABI -- glibc and musl both
// rely on it, and code compiled against their headers will.
//
// The block is mmap'd rather than taken from a static pool so that the
// size is the program's business rather than a constant here, and so
// that a thread's TLS is freed with its mapping.

static uint64_t tls_size;      // aligned size of the template
static uint64_t tls_align;
static const char *tls_image;  // .tdata in the loaded image
static uint64_t tls_filesz;

static uint64_t align_up(uint64_t v, uint64_t a) {
    if (a < 8) { a = 8; }       // the TCB self-pointer needs 8 either way
    return (v + a - 1) & ~(a - 1);
}

// Finds the PT_TLS template by walking the program headers the auxv
// points at. Returns 0 and leaves tls_size zero if the image has no
// thread-local storage, which is the common case and not an error.
static void tls_find_template(void) {
    unsigned long phdr = getauxval(AT_PHDR);
    unsigned long phnum = getauxval(AT_PHNUM);
    unsigned long phent = getauxval(AT_PHENT);
    if (!phdr || !phnum || !phent) { return; }

    for (unsigned long i = 0; i < phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(phdr + i * phent);
        if (ph->p_type != PT_TLS) { continue; }
        tls_image  = (const char *)ph->p_vaddr;
        tls_filesz = ph->p_filesz;
        tls_align  = ph->p_align ? ph->p_align : 8;
        tls_size   = align_up(ph->p_memsz, tls_align);
        return;
    }
}

// Builds one thread's TLS block and installs it as this thread's
// pointer. Safe to call when the image has no TLS: it then installs
// nothing, and %fs stays zero.
int __tls_setup_self(void) {
    if (tls_size == 0) { return 0; }

    // The block, plus room for the TCB above it, plus slack so the
    // base can be aligned inside the mapping.
    uint64_t need = tls_size + tls_align + 64;
    long raw = mmap_raw(0, need, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS);
    if (raw < 0) { return (int)raw; }

    // The TLS block's BASE must satisfy the segment's alignment, and
    // tls_size is a multiple of it, so the thread pointer above it is
    // aligned too.
    uint64_t base = align_up((uint64_t)raw, tls_align);
    uint64_t tp   = base + tls_size;

    // .tdata is copied, .tbss zeroed -- mmap already zeroed everything,
    // so only the copy is needed.
    if (tls_filesz) { memcpy((void *)base, tls_image, tls_filesz); }

    // %fs:0 must point at itself. Part of the ABI even though nothing
    // here reads it.
    *(uint64_t *)tp = tp;

    return arch_prctl(ARCH_SET_FS, tp);
}

void __libc_start(int argc, char **argv, char **envp, unsigned long *auxv) {
    auxv_table = auxv;
    environ    = envp;

    tls_find_template();
    // A failure here is not survivable: every __thread access in the
    // program would read through a null %fs. Better to die at startup,
    // where the exit code says so, than to fault at an arbitrary later
    // point.
    if (__tls_setup_self() != 0) { exit(127); }

    exit(main(argc, argv));
}
