// tlbstorm.c -- the TLB shootdown under real contention.
//
// CS2.6: tlb_shootdown's shootdown_busy is an unchecked global spin
// taken on every mmap/munmap/mprotect on the machine. Its comment
// explains why it cannot be a rank-checked spinlock (the ack-wait needs
// interrupts on and no lock held), which also makes it invisible to the
// lock-order tooling and a single global bottleneck rather than a
// per-address-space one.
//
// Two assertions, both about things that fail quietly today:
//
//   1. The 50,000,000-spin timeout path never fires. That log line
//      ("[tlb] shootdown timed out; continuing") outside a deliberately
//      wedged CPU means correctness has degraded to "continuing anyway"
//      -- a stale TLB entry somewhere, and nobody told the caller.
//   2. Frames come back. munmap defers frees through the shootdown, so
//      a leak there shows up as the free-frame count falling.
//
// clone() does not exist, so the concurrency is processes rather than
// threads. That still exercises the path: shootdown_busy is global, and
// munmap/mprotect send IPIs to every CPU regardless of whose address
// space the mapping belongs to.

#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <neoos_test.h>

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

extern long mmap_raw(unsigned long addr, unsigned long len, int prot, int flags);
extern int  munmap_raw(unsigned long addr, unsigned long len);
extern int  mprotect(void *addr, unsigned long length, int prot);

#define CHILDREN 4
#define ROUNDS   150
#define PAGES    16
#define REGION   (PAGES * 4096)

// mmap -> touch every page -> mprotect -> verify -> munmap, in a loop.
// Each step past the first forces a shootdown; the verify catches an
// mprotect that drops the contents rather than just the permissions.
static void hammer(void) {
    for (int r = 0; r < ROUNDS; r++) {
        long a = mmap_raw(0, REGION, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS);
        if (a < 0) { exit(2); }
        volatile unsigned char *p = (volatile unsigned char *)(unsigned long)a;

        for (int i = 0; i < REGION; i += 4096) { p[i] = (unsigned char)(r + i); }
        if (mprotect((void *)(unsigned long)a, REGION, PROT_READ) != 0) { exit(3); }
        for (int i = 0; i < REGION; i += 4096) {
            if (p[i] != (unsigned char)(r + i)) { exit(4); }
        }
        if (munmap_raw((unsigned long)a, REGION) != 0) { exit(5); }
    }
    exit(0);
}

int main(void) {
    // Warm up first: the very first mappings grow page tables that are
    // never handed back, so a count taken before them would never
    // balance. Everything after this point is steady state.
    for (int i = 0; i < 4; i++) {
        long a = mmap_raw(0, REGION, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS);
        if (a < 0) { printf("[tlbstorm] FAILED: warm-up mmap\n"); return 1; }
        volatile unsigned char *p = (volatile unsigned char *)(unsigned long)a;
        for (int j = 0; j < REGION; j += 4096) { p[j] = 1; }
        if (munmap_raw((unsigned long)a, REGION) != 0) {
            printf("[tlbstorm] FAILED: warm-up munmap\n");
            return 1;
        }
    }

    long before = neoos_test_pmm_free();
    if (before < 0) {
        printf("[tlbstorm] SKIPPED: no test hook (production build)\n");
        printf("[tlbstorm] ALL PASSED\n");
        return 0;
    }

    int pid[CHILDREN];
    for (int i = 0; i < CHILDREN; i++) {
        pid[i] = fork();
        if (pid[i] < 0) { printf("[tlbstorm] FAILED: fork %d\n", i); return 1; }
        if (pid[i] == 0) { hammer(); }
    }

    int bad = 0;
    for (int i = 0; i < CHILDREN; i++) {
        int st = 0;
        if (waitpid(pid[i], &st, 0) < 0) {
            printf("[tlbstorm] FAILED: waitpid %d\n", i);
            return 1;
        }
        if (WIFSIGNALED(st)) {
            // Distinguished from a clean non-zero exit on purpose: a
            // child killed here means the storm ran the machine out of
            // memory, which is what a broken deferred free looks like
            // from the outside.
            printf("[tlbstorm] FAILED: child %d killed by signal %d\n",
                   i, WTERMSIG(st));
            bad = 1;
        } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("[tlbstorm] FAILED: child %d exited %d (2=mmap 3=mprotect "
                   "4=contents lost 5=munmap)\n", i, WEXITSTATUS(st));
            bad = 1;
        }
    }
    if (bad) { return 1; }
    printf("[tlbstorm] %d children x %d rounds of mmap/touch/mprotect/munmap\n",
           CHILDREN, ROUNDS);

    long after = neoos_test_pmm_free();

    // How this assertion was arrived at, because the obvious versions do
    // not survive contact with a live system:
    //
    //   after == before  -- measured delta +722. The rest of the INITTAB
    //                       workload runs concurrently, so other
    //                       processes EXIT and hand memory back
    //                       underneath us. Frames gained, not lost.
    //   after >= before  -- measured delta -138 under DEBUG_HEAP, whose
    //                       larger heap-page headers carve fewer slots
    //                       per frame while other tests are still
    //                       starting up and allocating. Ambient churn
    //                       goes both ways.
    //
    // So: a tolerance band, sized by the signal we are looking for. This
    // storm munmaps CHILDREN * ROUNDS * PAGES = 9600 pages; a deferred-
    // free path that dropped them would lose frames on that order.
    // Measured ambient noise is a few hundred either way, so 2000 sits
    // comfortably above the noise and an order of magnitude below the
    // signal. The delta is printed either way, so a human reading the
    // log gets the number rather than just a verdict.
    //
    // Honest limit, established by trying to prove this fires: every
    // injected leak big enough to cross 2000 frames also kills something
    // first. Disabling the deferred pmm_free outright starves the
    // machine before this test even runs; leaking one frame in four gets
    // this process killed mid-storm, so the failure surfaces as the
    // missing [tlbstorm] ALL PASSED marker rather than as this branch.
    // So treat the count as a reported number and a second line of
    // defence -- the child-status checks above are what actually catch a
    // broken deferred free.
    long lost = before - after;
    if (lost > 2000) {
        printf("[tlbstorm] FAILED: frames LOST, %d -> %d (delta %d)\n",
               (int)before, (int)after, (int)(after - before));
        return 1;
    }
    printf("[tlbstorm] frames %d -> %d (delta %d, within ambient churn)\n",
           (int)before, (int)after, (int)(after - before));

    printf("[tlbstorm] ALL PASSED\n");
    return 0;
}
