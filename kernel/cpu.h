#ifndef NEOOS_CPU_H
#define NEOOS_CPU_H

#include <stdint.h>

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

// Saves/restores the calling CPU's extended state to/from a
// cpu_state_size()-byte, 64-byte-aligned buffer. Used by schedule()
// around every context switch, and by signal delivery.
void cpu_state_save(void *buf);
void cpu_state_restore(void *buf);

// Initialises `buf` (cpu_state_size() bytes) to the default state a
// freshly created thread starts from.
void cpu_state_init(void *buf);

void cpu_state_selftest(void);

// TEMPORARY: deleted by the per-thread-state and signal-frame tasks,
// which convert the last callers. Present only so those conversions can
// be separate, separately-verified changes.
#define FPU_STATE_SIZE 512
static inline void fpu_save(void *b)    { __asm__ volatile ("fxsave (%0)"  :: "r"(b) : "memory"); }
static inline void fpu_restore(void *b) { __asm__ volatile ("fxrstor (%0)" :: "r"(b) : "memory"); }
void cpu_default_fpu_state(void *dest);

#endif
