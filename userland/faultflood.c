// faultflood.c -- demand paging when the frames run out.
//
// CS3. mmap records a region and maps nothing; the frame arrives on
// first touch, in vma_fault_locked. That function has an
// `if (!frame) return 0;` path which becomes a SIGSEGV, and on a machine
// with more memory than the tests use it never runs.
//
// This touches far more distinct pages than the machine can back, so it
// runs for real. DEBUG_PMMFAIL exercises the same paths from the other
// direction (failures everywhere, at any size); this one exercises
// genuine exhaustion, which also drains the buddy allocator's larger
// orders rather than just failing one request.
//
// WHAT IS ASSERTED, and what deliberately is not:
//
//   - The KERNEL survives. No panic, no fault, and every later test in
//     the boot still runs. The gauntlet checks that independently.
//   - This process either completes the walk or dies by SIGSEGV. Both
//     are correct: SIGSEGV is what vma_fault_locked's failure path is
//     specified to produce.
//   - mmap refusing outright is also correct, and is not a failure.
//
// It does NOT assert that the walk completes, and must not: on a machine
// smaller than the mapping that is the wrong answer. A test that
// demanded success here would fail on exactly the configuration it is
// meant to cover.

#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <neoos_test.h>

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

extern long mmap_raw(unsigned long addr, unsigned long len, int prot, int flags);
extern int  munmap_raw(unsigned long addr, unsigned long len);

// Well past the 120 MiB this machine reports, so the walk cannot finish
// by simply fitting.
#define CHUNK_PAGES 4096                       // 16 MiB per mapping
#define CHUNK_BYTES (CHUNK_PAGES * 4096UL)
#define CHUNKS      24                         // 384 MiB attempted

static volatile int segv_seen;
static volatile unsigned pages_touched;

static void segv_handler(int sig) {
    (void)sig;
    segv_seen = 1;
    // Report from the handler: the counters are what the run is for, and
    // returning from a SIGSEGV would just re-fault forever.
    printf("[faultflood] SIGSEGV after %u pages -- vma_fault's out-of-frames path, as specified\n",
           pages_touched);
    printf("[faultflood] ALL PASSED\n");
    exit(0);
}

int main(void) {
    long free_before = neoos_test_pmm_free();

    struct sigaction sa;
    sa.sa_handler = segv_handler; sa.sa_flags = 0; sa.sa_mask = 0;
    sigaction(SIGSEGV, &sa, 0);

    unsigned long mapped_chunks = 0;
    for (int c = 0; c < CHUNKS; c++) {
        long a = mmap_raw(0, CHUNK_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS);
        if (a < 0) {
            // Refusing to map is a correct answer once memory is gone.
            printf("[faultflood] mmap refused at chunk %d after %u pages touched\n",
                   c, pages_touched);
            break;
        }
        mapped_chunks++;

        volatile unsigned char *p = (volatile unsigned char *)(unsigned long)a;
        for (unsigned long i = 0; i < CHUNK_BYTES; i += 4096) {
            p[i] = (unsigned char)(i >> 12);   // first touch -> demand fault
            pages_touched++;
        }
    }

    // Reaching here means the machine backed everything asked of it.
    printf("[faultflood] touched %u pages across %lu chunks without exhausting memory\n",
           pages_touched, mapped_chunks);

    long free_after = neoos_test_pmm_free();
    if (free_before >= 0 && free_after >= 0) {
        printf("[faultflood] free frames %d -> %d\n", (int)free_before, (int)free_after);
    }

    printf("[faultflood] ALL PASSED\n");
    return 0;
}
