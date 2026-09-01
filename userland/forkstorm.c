// forkstorm.c -- the pid, proc and fd tables under creation/destruction churn.
//
// CS3. Three assertions, all about tables rather than throughput:
//
//   - Every child's pid is distinct from every other LIVE child's.
//     pid_alloc hands out ids from a free list of recently-freed ones,
//     so a reuse bug shows up as two live children sharing a pid.
//   - Every child is reaped exactly once. A second waitpid for a pid
//     already reaped must fail rather than hang or succeed, which is
//     what a stale proc_table entry would do.
//   - Frames come back across the whole storm. Same tolerance-band
//     reasoning as userland/tlbstorm.c -- see the long comment there
//     for why bit-equality is not assertable on a live system.
//
// Each child opens a couple of descriptors and writes through a pipe
// before exiting, so the fd table is exercised on the way past rather
// than just allocated and dropped.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <neoos_test.h>

#define GENERATIONS 40
#define WIDTH        8

// What a child does before exiting: touch the fd table, then go.
static void child_body(void) {
    int p[2];
    if (pipe(p) != 0) { exit(11); }
    if (write(p[1], "x", 1) != 1) { exit(12); }
    char c = 0;
    if (read(p[0], &c, 1) != 1 || c != 'x') { exit(13); }
    int f = open("/ETC/INITTAB", O_RDONLY);
    if (f >= 0) { close(f); }
    close(p[0]);
    close(p[1]);
    exit(0);
}

int main(void) {
    long before = neoos_test_pmm_free();
    int have_hook = (before >= 0);

    int reaped = 0;
    for (int g = 0; g < GENERATIONS; g++) {
        int pids[WIDTH];

        for (int i = 0; i < WIDTH; i++) {
            pids[i] = fork();
            if (pids[i] < 0) {
                printf("[forkstorm] FAILED: fork gen %d child %d\n", g, i);
                return 1;
            }
            if (pids[i] == 0) { child_body(); }
        }

        // Every pid in this generation is live right now: none has been
        // waited for yet. Two the same means pid_alloc reissued a pid
        // that was still in use.
        for (int i = 0; i < WIDTH; i++) {
            for (int j = 0; j < i; j++) {
                if (pids[i] == pids[j]) {
                    printf("[forkstorm] FAILED: gen %d live pid %d issued twice\n",
                           g, pids[i]);
                    return 1;
                }
            }
        }

        for (int i = 0; i < WIDTH; i++) {
            int st = 0;
            if (waitpid(pids[i], &st, 0) < 0) {
                printf("[forkstorm] FAILED: waitpid gen %d pid %d\n", g, pids[i]);
                return 1;
            }
            if (WIFSIGNALED(st)) {
                printf("[forkstorm] FAILED: gen %d pid %d killed by signal %d\n",
                       g, pids[i], WTERMSIG(st));
                return 1;
            }
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                printf("[forkstorm] FAILED: gen %d pid %d exited %d "
                       "(11=pipe 12=write 13=read)\n",
                       g, pids[i], WEXITSTATUS(st));
                return 1;
            }
            reaped++;

            // Reaping the same pid twice must fail, not hang and not
            // succeed: the proc_table entry is gone by now.
            int st2 = 0;
            if (waitpid(pids[i], &st2, 0) >= 0) {
                printf("[forkstorm] FAILED: gen %d pid %d reaped twice\n",
                       g, pids[i]);
                return 1;
            }
        }
    }

    printf("[forkstorm] %d generations x %d children, %d reaped exactly once\n",
           GENERATIONS, WIDTH, reaped);

    if (have_hook) {
        long after = neoos_test_pmm_free();
        long lost = before - after;
        // Band and reasoning as in tlbstorm.c: ambient churn from the
        // rest of the INITTAB workload moves this a few hundred either
        // way, while a table or address-space leak across 320 processes
        // would be far larger.
        if (lost > 2000) {
            printf("[forkstorm] FAILED: frames LOST, %d -> %d (delta %d)\n",
                   (int)before, (int)after, (int)(after - before));
            return 1;
        }
        printf("[forkstorm] frames %d -> %d (delta %d, within ambient churn)\n",
               (int)before, (int)after, (int)(after - before));
    }

    printf("[forkstorm] ALL PASSED\n");
    return 0;
}
