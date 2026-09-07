// userland/mmstress.c -- concurrent address-space stress test.
//
// The fast, .NET-free oracle for the concurrent-request crash
// investigation (docs/superpowers/specs/2026-09-07-concurrent-request-
// crash-investigation.md, Phase 1a). Many threads of ONE process
// hammer mmap / munmap / mprotect / page faults on private anonymous
// memory while writing and verifying a per-thread pattern. If the
// kernel's mm is not SMP-safe for multiple threads of one process, a
// verify mismatch or a SIGSEGV falls out here in seconds -- with no GC
// and no runtime in the way.
//
// Run headless: `make mmstress` (its own boot, INITTAB runs only this).
// Green looks like: [mmstress] ALL PASSED
// Red looks like:   [mmstress] FAIL ... / a [fault-audit] line / a
//                   kernel [exception].

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

#define NTHREADS      8
#define ITERS         600      // per thread
#define PAGE          4096

static volatile int g_failed;

static void fail(const char *what, void *p, uint64_t a, uint64_t b) {
    g_failed = 1;
    printf("[mmstress] FAIL %s p=%p a=%lu b=%lu\n", what, p, a, b);
}

// Fill every page of [p, p+len) with a word derived from (tag, page
// index), then read it all back and check. `toggle` also flips the
// region to PROT_READ and back mid-way, exercising mprotect + the
// cross-CPU TLB flush against the concurrent writers on other threads.
static void bang(uint64_t tag, uint64_t len, int toggle) {
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) { fail("mmap", 0, len, 0); return; }

    uint64_t npages = len / PAGE;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t *w = (uint64_t *)(p + i * PAGE);
        *w = tag ^ (i * 0x9E3779B97F4A7C15ULL);
        p[i * PAGE + PAGE - 1] = (unsigned char)(tag + i);   // touch the last byte too
    }

    if (toggle) {
        if (mprotect(p, len, PROT_READ) != 0) { fail("mprotect ro", p, len, 0); }
        if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) { fail("mprotect rw", p, len, 0); }
    }

    for (uint64_t i = 0; i < npages; i++) {
        uint64_t *w = (uint64_t *)(p + i * PAGE);
        uint64_t want = tag ^ (i * 0x9E3779B97F4A7C15ULL);
        if (*w != want) { fail("verify word", p, *w, want); break; }
        unsigned char cw = (unsigned char)(tag + i);
        if (p[i * PAGE + PAGE - 1] != cw) { fail("verify byte", p, i, 0); break; }
    }

    if (munmap(p, len) != 0) { fail("munmap", p, len, 0); }
}

static void *worker(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    const uint64_t sizes[] = { PAGE, 16 * PAGE, 64 * PAGE, PAGE, 256 * PAGE };
    for (int it = 0; it < ITERS && !g_failed; it++) {
        uint64_t len = sizes[it % 5];
        uint64_t tag = (id << 40) ^ ((uint64_t)it << 8) ^ 0xC0FFEE;
        bang(tag, len, (it & 3) == 0);
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[mmstress] start: %d threads x %d iters, one address space\n",
           NTHREADS, ITERS);

    pthread_t th[NTHREADS];
    for (uint64_t i = 0; i < NTHREADS; i++) {
        if (pthread_create(&th[i], 0, worker, (void *)(uintptr_t)i) != 0) {
            printf("[mmstress] FAIL pthread_create %lu\n", i);
            return 1;
        }
    }
    for (int i = 0; i < NTHREADS; i++) { pthread_join(th[i], 0); }

    if (g_failed) { printf("[mmstress] FAILED\n"); return 1; }
    printf("[mmstress] ALL PASSED\n");
    return 0;
}
