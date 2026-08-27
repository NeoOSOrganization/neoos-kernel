#include "cpu.h"
#include "serial.h"

#define CPUID_LEAF_1_EDX_SSE   (1u << 25)
#define CPUID_LEAF_1_EDX_SSE2  (1u << 26)
#define CPUID_LEAF_1_ECX_SSE3  (1u << 0)
#define CPUID_LEAF_1_ECX_SSSE3 (1u << 9)
#define CPUID_LEAF_1_ECX_SSE41 (1u << 19)
#define CPUID_LEAF_1_ECX_SSE42 (1u << 20)

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)

static uint8_t default_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                       : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                       : "a"(leaf));
}

__attribute__((unused))
static void cpuid_count(uint32_t leaf, uint32_t sub,
                        uint32_t *eax, uint32_t *ebx,
                        uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(sub));
}

static void check_features(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);

    int missing = 0;
    if (!(edx & CPUID_LEAF_1_EDX_SSE)) {
        serial_write_string("[cpu] MISSING: SSE\n");
        missing = 1;
    }
    if (!(edx & CPUID_LEAF_1_EDX_SSE2)) {
        serial_write_string("[cpu] MISSING: SSE2\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSE3)) {
        serial_write_string("[cpu] MISSING: SSE3\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSSE3)) {
        serial_write_string("[cpu] MISSING: SSSE3\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSE41)) {
        serial_write_string("[cpu] MISSING: SSE4.1\n");
        missing = 1;
    }
    if (!(ecx & CPUID_LEAF_1_ECX_SSE42)) {
        serial_write_string("[cpu] MISSING: SSE4.2\n");
        missing = 1;
    }

    if (missing) {
        serial_write_string("[cpu] required SSE extension(s) missing -- halting\n");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    serial_write_string("[cpu] SSE/SSE2/SSE3/SSSE3/SSE4.1/SSE4.2 detected\n");
}

static uint64_t read_cr0(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static void write_cr0(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(value) : "memory");
}

static uint64_t read_cr4(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr4(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr4" :: "r"(value) : "memory");
}

static void enable_sse(void) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP;
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);
}

static void capture_default_fpu_state(void) {
    __asm__ volatile ("fninit");
    uint32_t mxcsr_default = 0x1F80;
    __asm__ volatile ("ldmxcsr %0" :: "m"(mxcsr_default));
    fpu_save(default_fpu_state);
}

void cpu_init(void) {
    check_features();
    enable_sse();
    capture_default_fpu_state();
    serial_write_string("[cpu] SSE enabled, default FPU/SSE state captured\n");
}

void cpu_default_fpu_state(void *dest) {
    uint8_t *out = (uint8_t *)dest;
    for (int i = 0; i < FPU_STATE_SIZE; i++) {
        out[i] = default_fpu_state[i];
    }
}
