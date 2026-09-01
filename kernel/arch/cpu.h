#ifndef NEOOS_CPU_H
#define NEOOS_CPU_H

#include <stdint.h>
#include <stdbool.h>

// Upper bound on the extended state this kernel will ever enable:
// x87 + SSE + AVX, which CPUID reports as 832 bytes on Haswell.
// AVX-512 is explicitly out of scope (see the roadmap spec), so no
// configuration can exceed this. Used only to size the static default
// template; per-thread areas are exactly cpu_state_size() bytes.
#define CPU_STATE_MAX 1024

// Detects CPU features via CPUID, enables SSE and (where available)
// XSAVE with the widest XCR0 this kernel supports, sizes the extended
// state area, and captures the default template new threads start
// from. Halts with a diagnostic if a required SSE feature is missing --
// the kernel binary is compiled assuming their presence. AVX is
// OPTIONAL and detected, never required: QEMU's -cpu Nehalem model has
// no XSAVE at all, and the whole existing test suite runs on it.
void cpu_init(void);

// Size in bytes of one thread's extended-state area. Valid only after
// cpu_init(). 512 when the CPU has no XSAVE.
uint32_t cpu_state_size(void);

// The XCR0 actually enabled. Signal delivery records it in the frame,
// and rt_sigreturn masks a user-supplied xstate header against it.
uint64_t cpu_state_xcr0(void);

// 1 if AVX / AVX2 was detected and enabled.
int cpu_has_avx(void);
int cpu_has_avx2(void);

// The 48-byte CPUID processor brand string, NUL-terminated (out >= 49).
void cpu_brand_string(char out[49]);
// Space-separated feature names ("sse2 sse4.2 avx2 ..."); returns length.
int  cpu_feature_string(char *out, int max);

// Saves/restores the calling CPU's extended state to/from a
// cpu_state_size()-byte, 64-byte-aligned buffer. Used by schedule()
// around every context switch, and by signal delivery.
void cpu_state_save(void *buf);
void cpu_state_restore(void *buf);

// Initialises `buf` (cpu_state_size() bytes) to the default state a
// freshly created thread starts from.
void cpu_state_init(void *buf);

void cpu_state_selftest(void);

// CPUID instruction: query CPU features.
// Declared here but implemented in arch/cpu.c (used by rand_init).
void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

// Read the TSC (time stamp counter).
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Try to read a random 64-bit value via RDRAND. Returns true if successful.
static inline bool rdrand64(uint64_t *out) {
    uint8_t ok;
    __asm__ volatile ("rdrand %%rax; setc %1" : "=a"(*out), "=q"(ok) :: "cc");
    return (bool)ok;
}

// Disable interrupts
static inline void cli(void) {
    __asm__ volatile ("cli");
}

// Enable interrupts
static inline void sti(void) {
    __asm__ volatile ("sti");
}

#endif
