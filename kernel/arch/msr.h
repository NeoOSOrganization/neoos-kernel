#ifndef NEOOS_ARCH_MSR_H
#define NEOOS_ARCH_MSR_H

#include <stdint.h>

// Model-specific register access, in one place.
//
// This used to exist three times over: a rdmsr/wrmsr pair in
// syscall.c, a wrmsr64 in cpu_local.c, and a bare asm with a hardcoded
// 0xC0000100 in sched.c's context switch. Splitting syscall.c made the
// duplication load-bearing -- two of the new files needed the pair --
// which was the moment to collapse it rather than add a fourth copy.

#define MSR_EFER           0xC0000080
#define MSR_STAR           0xC0000081
#define MSR_LSTAR          0xC0000082
#define MSR_SFMASK         0xC0000084
#define MSR_FS_BASE        0xC0000100
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

#define EFER_SCE (1ULL << 0)
#define EFER_NXE (1ULL << 11)

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

#endif
