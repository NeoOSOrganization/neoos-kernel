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
#define CR4_OSXSAVE    (1ULL << 18)

#define CPUID_1_ECX_XSAVE     (1u << 26)
#define CPUID_1_ECX_OSXSAVE   (1u << 27)
#define CPUID_1_ECX_AVX       (1u << 28)
#define CPUID_7_EBX_AVX2      (1u << 5)
#define CPUID_D1_EAX_XSAVEOPT (1u << 0)

#define XCR0_X87 (1ULL << 0)
#define XCR0_SSE (1ULL << 1)
#define XCR0_AVX (1ULL << 2)

static uint32_t xstate_size = 512;
static uint64_t xstate_mask;
static int      have_avx, have_avx2;
static enum { SAVE_FXSAVE, SAVE_XSAVE, SAVE_XSAVEOPT } save_mode = SAVE_FXSAVE;

static uint8_t default_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                       : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                       : "a"(leaf));
}

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

static void xsetbv(uint32_t index, uint64_t value) {
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    __asm__ volatile ("xsetbv" :: "c"(index), "a"(lo), "d"(hi));
}

static void enable_xsave(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(c & CPUID_1_ECX_XSAVE)) {
        // QEMU's -cpu Nehalem model reports no XSAVE, and the whole
        // existing test suite runs on it, so this path is live -- not a
        // theoretical fallback.
        serial_write_string("[cpu] no XSAVE -- using FXSAVE, 512-byte state\n");
        return;                       // save_mode stays SAVE_FXSAVE
    }
    have_avx = (c & CPUID_1_ECX_AVX) != 0;

    cpuid_count(7, 0, &a, &b, &c, &d);
    have_avx2 = have_avx && (b & CPUID_7_EBX_AVX2) != 0;

    // ORDER IS LOAD-BEARING. CR4.OSXSAVE must be set before XSETBV is
    // legal, and CPUID.0Dh:EBX only reports the right size AFTER XCR0
    // is written -- it reflects what is ENABLED, not what is supported.
    // Measured on Haswell: EBX reads 576 beforehand and 832 afterwards,
    // so reading it early would size every buffer 256 bytes short and
    // corrupt silently.
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSXSAVE;
    write_cr4(cr4);

    // XCR0.AVX without XCR0.SSE #GPs, so those bits go on together or
    // not at all.
    xstate_mask = XCR0_X87 | XCR0_SSE;
    if (have_avx) { xstate_mask |= XCR0_AVX; }
    xsetbv(0, xstate_mask);

    cpuid_count(0xD, 0, &a, &b, &c, &d);
    xstate_size = b;
    if (xstate_size < 512 || xstate_size > CPU_STATE_MAX) {
        serial_write_string("[cpu] implausible xstate size -- falling back to FXSAVE\n");
        xstate_size = 512;
        xstate_mask = 0;
        save_mode = SAVE_FXSAVE;
        return;
    }

    cpuid_count(0xD, 1, &a, &b, &c, &d);
    save_mode = (a & CPUID_D1_EAX_XSAVEOPT) ? SAVE_XSAVEOPT : SAVE_XSAVE;
}

static uint8_t default_state[CPU_STATE_MAX] __attribute__((aligned(64)));

void cpu_state_save(void *buf) {
    uint32_t lo = (uint32_t)xstate_mask, hi = (uint32_t)(xstate_mask >> 32);
    switch (save_mode) {
    case SAVE_XSAVEOPT:
        __asm__ volatile ("xsaveopt (%0)" :: "r"(buf), "a"(lo), "d"(hi) : "memory");
        break;
    case SAVE_XSAVE:
        __asm__ volatile ("xsave (%0)" :: "r"(buf), "a"(lo), "d"(hi) : "memory");
        break;
    default:
        __asm__ volatile ("fxsave (%0)" :: "r"(buf) : "memory");
        break;
    }
}

void cpu_state_restore(void *buf) {
    uint32_t lo = (uint32_t)xstate_mask, hi = (uint32_t)(xstate_mask >> 32);
    if (save_mode == SAVE_FXSAVE) {
        __asm__ volatile ("fxrstor (%0)" :: "r"(buf) : "memory");
    } else {
        __asm__ volatile ("xrstor (%0)" :: "r"(buf), "a"(lo), "d"(hi) : "memory");
    }
}

uint32_t cpu_state_size(void) { return xstate_size; }
uint64_t cpu_state_xcr0(void) { return xstate_mask; }
int cpu_has_avx(void)  { return have_avx; }
int cpu_has_avx2(void) { return have_avx2; }

void cpu_state_init(void *buf) {
    uint8_t *out = (uint8_t *)buf;
    for (uint32_t i = 0; i < xstate_size; i++) { out[i] = default_state[i]; }
}

static void capture_default_state(void) {
    // XSAVEOPT decides what to skip by comparing against the buffer's
    // existing contents, so every area it will ever write must start
    // zeroed -- including this template, which is copied into every new
    // thread's area.
    for (uint32_t i = 0; i < CPU_STATE_MAX; i++) { default_state[i] = 0; }

    __asm__ volatile ("fninit");
    uint32_t mxcsr_default = 0x1F80;
    __asm__ volatile ("ldmxcsr %0" :: "m"(mxcsr_default));
    cpu_state_save(default_state);
}

void cpu_state_selftest(void) {
    if (xstate_size < 512) {
        serial_write_string("[cpu] selftest FAILED: state size below 512\n");
        return;
    }
    if (save_mode != SAVE_FXSAVE &&
        (xstate_mask & (XCR0_X87 | XCR0_SSE)) != (XCR0_X87 | XCR0_SSE)) {
        serial_write_string("[cpu] selftest FAILED: XCR0 missing x87/SSE\n");
        return;
    }
    if (have_avx && !(xstate_mask & XCR0_AVX)) {
        serial_write_string("[cpu] selftest FAILED: AVX detected but not enabled\n");
        return;
    }
    if (have_avx && xstate_size <= 512) {
        serial_write_string("[cpu] selftest FAILED: AVX enabled but state still 512\n");
        return;
    }

    // Save/restore round trip: the state after a restore must save back
    // identically. With XSAVEOPT the header's xstate_bv can legitimately
    // differ (a component may be skipped), so compare the legacy area
    // and the masked feature bits rather than raw bytes.
    static uint8_t a[CPU_STATE_MAX] __attribute__((aligned(64)));
    static uint8_t b[CPU_STATE_MAX] __attribute__((aligned(64)));
    for (uint32_t i = 0; i < CPU_STATE_MAX; i++) { a[i] = 0; b[i] = 0; }

    cpu_state_save(a);
    cpu_state_restore(a);
    cpu_state_save(b);
    for (uint32_t i = 0; i < 512; i++) {
        if (a[i] != b[i]) {
            serial_write_string("[cpu] selftest FAILED: legacy area not stable\n");
            return;
        }
    }
    if (save_mode != SAVE_FXSAVE) {
        uint64_t bv_a = *(uint64_t *)&a[512] & xstate_mask;
        uint64_t bv_b = *(uint64_t *)&b[512] & xstate_mask;
        if (bv_a != bv_b) {
            serial_write_string("[cpu] selftest FAILED: xstate_bv not stable\n");
            return;
        }
    }
    serial_write_string("[cpu] state selftest passed\n");
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
    enable_xsave();
    capture_default_state();
    capture_default_fpu_state();
    serial_write_string("[cpu] SSE enabled, default FPU/SSE state captured\n");

    serial_write_string("[cpu] state: size=");
    serial_write_hex64(xstate_size);
    serial_write_string(" xcr0=");
    serial_write_hex64(xstate_mask);
    serial_write_string(save_mode == SAVE_XSAVEOPT ? " mode=xsaveopt"
                      : save_mode == SAVE_XSAVE    ? " mode=xsave"
                                                   : " mode=fxsave");
    serial_write_string(have_avx2 ? " avx2\n" : have_avx ? " avx\n" : "\n");
}

void cpu_default_fpu_state(void *dest) {
    uint8_t *out = (uint8_t *)dest;
    for (int i = 0; i < FPU_STATE_SIZE; i++) {
        out[i] = default_fpu_state[i];
    }
}
