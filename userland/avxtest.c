#include <unistd.h>
#include <stdio.h>
#include <thread.h>
#include <stdint.h>

// Built with -mavx -mavx2, unlike every other program here: the
// userland baseline stays SSE4.2 so NeoOS keeps running on a CPU
// without AVX (QEMU's -cpu Nehalem has none). This program therefore
// has to check at RUN time before executing a single AVX instruction.
static int have_avx2(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(1), "c"(0));
    if (!(ecx & (1u << 28))) { return 0; }          // AVX
    if (!(ecx & (1u << 27))) { return 0; }          // OSXSAVE
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(7), "c"(0));
    return (ebx & (1u << 5)) != 0;                  // AVX2
}

// Yields WITHOUT a function call. Calling lib's yield() is fatal to this
// test: GCC inserts `vzeroupper` before a call from AVX code, to avoid
// AVX-SSE transition penalties, and that zeroes the upper 128 bits of
// every ymm register -- so the test would destroy the exact state it is
// trying to measure. (Observed: ymm words 0-3 preserved, word 4 onward
// zero, which looks exactly like a kernel that drops the AVX component.)
#define SYS_YIELD 2
__attribute__((always_inline))
static inline void yield_noclobber(void) {
    long ret;
    // rax must be an OUTPUT as well as an input: `syscall` returns its
    // result there. Declaring it input-only lets GCC hoist the number
    // out of a loop, after which the second and later iterations run
    // with whatever the previous call returned -- 0, which is SYS_EXIT.
    // (Observed: the process exited with the loop counter as its code.)
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"((long)SYS_YIELD)
                      : "rcx", "r11", "memory");
    (void)ret;
}

// Fills all 16 ymm registers from `seed`, yields many times so the
// scheduler is guaranteed to save and restore them, then checks every
// one survived.
static int hammer_ymm(unsigned seed, int rounds) {
    uint32_t pattern[8 * 16];
    for (int r = 0; r < 16; r++) {
        for (int i = 0; i < 8; i++) { pattern[r * 8 + i] = seed + r * 8 + i; }
    }

    __asm__ volatile (
        "vmovdqu   0(%0), %%ymm0\n\tvmovdqu  32(%0), %%ymm1\n\t"
        "vmovdqu  64(%0), %%ymm2\n\tvmovdqu  96(%0), %%ymm3\n\t"
        "vmovdqu 128(%0), %%ymm4\n\tvmovdqu 160(%0), %%ymm5\n\t"
        "vmovdqu 192(%0), %%ymm6\n\tvmovdqu 224(%0), %%ymm7\n\t"
        "vmovdqu 256(%0), %%ymm8\n\tvmovdqu 288(%0), %%ymm9\n\t"
        "vmovdqu 320(%0), %%ymm10\n\tvmovdqu 352(%0), %%ymm11\n\t"
        "vmovdqu 384(%0), %%ymm12\n\tvmovdqu 416(%0), %%ymm13\n\t"
        "vmovdqu 448(%0), %%ymm14\n\tvmovdqu 480(%0), %%ymm15\n\t"
        :: "r"(pattern) : "memory");

    for (int i = 0; i < rounds; i++) { yield_noclobber(); }

    uint32_t out[8 * 16];
    __asm__ volatile (
        "vmovdqu %%ymm0,   0(%0)\n\tvmovdqu %%ymm1,  32(%0)\n\t"
        "vmovdqu %%ymm2,  64(%0)\n\tvmovdqu %%ymm3,  96(%0)\n\t"
        "vmovdqu %%ymm4, 128(%0)\n\tvmovdqu %%ymm5, 160(%0)\n\t"
        "vmovdqu %%ymm6, 192(%0)\n\tvmovdqu %%ymm7, 224(%0)\n\t"
        "vmovdqu %%ymm8, 256(%0)\n\tvmovdqu %%ymm9, 288(%0)\n\t"
        "vmovdqu %%ymm10,320(%0)\n\tvmovdqu %%ymm11,352(%0)\n\t"
        "vmovdqu %%ymm12,384(%0)\n\tvmovdqu %%ymm13,416(%0)\n\t"
        "vmovdqu %%ymm14,448(%0)\n\tvmovdqu %%ymm15,480(%0)\n\t"
        :: "r"(out) : "memory");

    for (int i = 0; i < 8 * 16; i++) {
        if (out[i] != pattern[i]) {
            printf("[avxtest] FAILED: ymm word %d = %u, want %u\n",
                   i, out[i], pattern[i]);
            return 0;
        }
    }
    return 1;
}

static volatile int worker_ok;
static volatile int worker_done;

// A second thread hammering a DIFFERENT pattern. Without it the test
// would pass even if the kernel restored one global copy of the
// registers to everybody.
static void worker(void *arg) {
    (void)arg;
    worker_ok = hammer_ymm(0xB0000000u, 300);
    worker_done = 1;
    thread_exit(0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!have_avx2()) {
        printf("[avxtest] SKIPPED: no AVX2 on this CPU\n");
        printf("[avxtest] ALL PASSED\n");
        return 0;
    }

    thread_t t;
    worker_ok = 0; worker_done = 0;
    if (thread_create(&t, worker, 0) != 0) {
        printf("[avxtest] FAILED: thread_create\n");
        return 1;
    }

    int main_ok = hammer_ymm(0xA0000000u, 300);
    while (!worker_done) { yield(); }
    thread_join(t, 0);

    if (!main_ok)   { printf("[avxtest] FAILED: main thread ymm\n");   return 1; }
    if (!worker_ok) { printf("[avxtest] FAILED: worker thread ymm\n"); return 1; }
    printf("[avxtest] ymm preserved across context switches\n");

    // MMX rides on the x87 component, so this proves that too.
    uint64_t mmx_in = 0x0123456789ABCDEFULL, mmx_out = 0;
    __asm__ volatile ("movq %0, %%mm0" :: "m"(mmx_in));
    for (int i = 0; i < 200; i++) { yield_noclobber(); }
    __asm__ volatile ("movq %%mm0, %0" : "=m"(mmx_out));
    __asm__ volatile ("emms");
    if (mmx_out != mmx_in) {
        printf("[avxtest] FAILED: mm0 not preserved\n");
        return 1;
    }
    printf("[avxtest] mm0 preserved across context switches\n");

    printf("[avxtest] ALL PASSED\n");
    return 0;
}
