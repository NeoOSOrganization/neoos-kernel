#include "mm/uaccess.h"

// Copies one byte at a time, matching this kernel's existing "no
// vector-store machinery available" style precedent (see the AC97
// driver's blit_row for the same reasoning) -- correctness first,
// widening left as a follow-up if profiling ever shows a syscall
// bottlenecked here.
//
// Each iteration is ONE self-contained asm block: the copy
// instruction (label "1", the address EX_TABLE_ENTRY records as
// `fault_addr`), an unconditional jump PAST the fixup on the normal
// path, then the fixup itself (label "2", `fixup_addr`) which sets
// `err` and falls through to "3" (no jump needed -- "2" and "3" are
// adjacent in program order). This needs no asm-goto (avoiding any
// GCC-version dependence on that feature) and no separate section:
// the fixup lives inline, right where it is emitted, and is reached
// ONLY by the page-fault dispatcher setting regs->rip to it directly
// -- never by falling into it from normal control flow, since the
// unconditional jump on the success path skips over it.
uint64_t copy_to_user(void *dst, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        long err = 0;
        uint8_t byte = s[i];
        __asm__ volatile (
            "1: movb %2, (%1)\n\t"
            "   jmp 3f\n\t"
            "2: movq $1, %0\n\t"
            "3:\n\t"
            EX_TABLE_ENTRY(1, 2)
            : "+r"(err)
            : "r"(d + i), "r"(byte)
            : "memory"
        );
        if (err) { return n - i; }
    }
    return 0;
}

uint64_t copy_from_user(void *dst, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        long err = 0;
        uint8_t byte = 0;
        __asm__ volatile (
            "1: movb (%2), %1\n\t"
            "   jmp 3f\n\t"
            "2: movq $1, %0\n\t"
            "3:\n\t"
            EX_TABLE_ENTRY(1, 2)
            : "+r"(err), "+r"(byte)
            : "r"(s + i)
            : "memory"
        );
        if (err) { return n - i; }
        d[i] = byte;
    }
    return 0;
}
