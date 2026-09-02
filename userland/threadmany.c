// threadmany.c -- more threads than the old ceiling allowed.
//
// CS4. MAX_THREADS_PER_PROC was 16, described in the header as a
// "Phase 2 placeholder", and it was a hard wall rather than a soft one:
// the bitmap of live thread stacks was a single uint16_t, so the
// seventeenth thread_create simply failed. Nothing in the suite ever
// asked for a seventeenth, so nothing noticed.
//
// This asks for 64. What is asserted:
//
//   - every one of them starts, gets its OWN stack, and finishes;
//   - the stacks are distinct and far enough apart to be real stacks
//     rather than aliases of one -- an off-by-one in the widened bitmap
//     would hand two threads the same slot, and the symptom would be
//     silent corruption rather than a failure;
//   - slots are RECLAIMED: a second wave of 64 must start too, which a
//     bitmap that sets bits without clearing them would fail.

#include <unistd.h>
#include <stdio.h>
#include <thread.h>

#define N 64
#define WAVES 2

static volatile int counter;
static volatile unsigned long stack_addr[N];

static void worker(void *arg) {
    int idx = (int)(long)arg;
    volatile int local = 0;
    stack_addr[idx] = (unsigned long)(void *)&local;
    for (int i = 0; i < 200; i++) {
        __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
    }
    local = idx;
    (void)local;
    thread_exit(0);
}

int main(void) {
    for (int wave = 0; wave < WAVES; wave++) {
        counter = 0;
        for (int i = 0; i < N; i++) { stack_addr[i] = 0; }

        thread_t t[N];
        int started = 0;
        for (; started < N; started++) {
            if (thread_create(&t[started], worker, (void *)(long)started) != 0) {
                break;
            }
        }
        if (started != N) {
            printf("[threadmany] FAILED: wave %d created only %d of %d threads "
                   "(16 is the old ceiling)\n", wave, started, N);
            return 1;
        }
        for (int i = 0; i < N; i++) { thread_join(t[i], 0); }

        if (counter != N * 200) {
            printf("[threadmany] FAILED: wave %d counter %d, want %d\n",
                   wave, counter, N * 200);
            return 1;
        }

        // Distinct stacks. Two threads sharing a slot would have local
        // addresses within a page of each other; real slots are a whole
        // stride apart.
        for (int i = 0; i < N; i++) {
            if (!stack_addr[i]) {
                printf("[threadmany] FAILED: wave %d thread %d never ran\n", wave, i);
                return 1;
            }
            for (int j = 0; j < i; j++) {
                unsigned long a = stack_addr[i], b = stack_addr[j];
                unsigned long d = a > b ? a - b : b - a;
                if (d < 4096) {
                    printf("[threadmany] FAILED: wave %d threads %d and %d share "
                           "a stack (%lx vs %lx)\n", wave, j, i, b, a);
                    return 1;
                }
            }
        }
    }

    printf("[threadmany] %d waves of %d threads: all started, distinct stacks, "
           "slots reclaimed\n", WAVES, N);
    printf("[threadmany] ALL PASSED\n");
    return 0;
}
