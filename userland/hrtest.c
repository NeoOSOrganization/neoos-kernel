// hrtest -- userland checks for the high-resolution timer milestone.
// Prints "hrtest: ok <name>" / "hrtest: FAIL <name> ..." per check and
// "PASS hrtest" only if every check passed.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>

static int fails;
#define CHECK(name, cond, ...) do { if (cond) printf("hrtest: ok %s\n", name); \
    else { fails++; printf("hrtest: FAIL %s: ", name); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t now_ns(clockid_t c) {
    struct timespec ts; clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void test_clocks(void) {
    struct timespec r = { 9, 9 };
    int rc = clock_getres(CLOCK_MONOTONIC, &r);
    CHECK("clock_getres", rc == 0 && r.tv_sec == 0 && r.tv_nsec == 1,
          "rc=%d res=%ld.%09ld", rc, (long)r.tv_sec, r.tv_nsec);
    CHECK("clock_getres_einval", clock_getres(99, &r) == -1 && errno == EINVAL, "errno=%d", errno);

    // 1000 back-to-back reads: monotonic, and at least one step below
    // 1 ms (a 10 ms tick clock can only step by 0 or 10 ms).
    uint64_t prev = now_ns(CLOCK_MONOTONIC), min_step = UINT64_MAX; int back = 0;
    for (int i = 0; i < 1000; i++) {
        uint64_t n = now_ns(CLOCK_MONOTONIC);
        if (n < prev) { back = 1; }
        if (n > prev && n - prev < min_step) { min_step = n - prev; }
        prev = n;
    }
    CHECK("monotonic", !back, "went backwards");
    CHECK("sub_ms_steps", min_step < 1000000ULL, "smallest step %llu ns", (unsigned long long)min_step);

    // CPU time advances while spinning, in sub-tick amounts.
    uint64_t c0 = now_ns(CLOCK_THREAD_CPUTIME_ID);
    volatile uint64_t x = 0; for (int i = 0; i < 2000000; i++) { x += i; }
    uint64_t c1 = now_ns(CLOCK_THREAD_CPUTIME_ID);
    CHECK("thread_cputime", c1 > c0 && (c1 - c0) % 10000000ULL != 0,
          "delta %llu", (unsigned long long)(c1 - c0));
}

// Linux's struct sched_attr (uapi linux/sched/types.h, which clashes
// with musl's <sched.h> over struct sched_param).
struct sched_attr {
    uint32_t size, sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
};

// CPU-bound threads with a 100 us slice, twice as many as CPUs, so at
// least one CPU runs two of them. NeoOS records sched_setaffinity but
// does not enforce it yet, so instead of pinning, each spinner notes
// which CPU it is on: a change of last_runner[cpu] is exactly one
// context switch between spinners on that CPU.
#define MAXCPU 64
static volatile int stop;
static volatile int last_runner[MAXCPU];
static volatile uint64_t cpu_switches[MAXCPU];
static void *spin(void *arg) {
    int me = (int)(intptr_t)arg;
    struct sched_attr at = { .size = sizeof at, .sched_policy = SCHED_OTHER, .sched_runtime = 100000 };
    syscall(SYS_sched_setattr, 0, &at, 0);
    while (!stop) {
        int cpu = sched_getcpu();
        if (cpu < 0 || cpu >= MAXCPU) { continue; }
        if (last_runner[cpu] != me) { last_runner[cpu] = me; cpu_switches[cpu]++; }
    }
    return 0;
}
static void test_slices(void) {
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN), n = 2 * ncpu;
    if (ncpu < 1 || n > MAXCPU) { CHECK("slice_100us", 0, "ncpu=%d", ncpu); return; }
    pthread_t th[MAXCPU];
    for (int i = 0; i < MAXCPU; i++) { last_runner[i] = -1; }
    for (int i = 0; i < n; i++) { pthread_create(&th[i], 0, spin, (void *)(intptr_t)i); }
    uint64_t t0 = now_ns(CLOCK_MONOTONIC);
    struct timespec one = { 1, 0 }; nanosleep(&one, 0);
    stop = 1;
    uint64_t el = now_ns(CLOCK_MONOTONIC) - t0;
    for (int i = 0; i < n; i++) { pthread_join(th[i], 0); }
    uint64_t best = 0;
    for (int i = 0; i < ncpu; i++) { if (cpu_switches[i] > best) { best = cpu_switches[i]; } }
    uint64_t per_s = best * 1000000000ULL / (el ? el : 1);
    printf("hrtest: context switches/s=%llu (busiest of %d CPUs)\n", (unsigned long long)per_s, ncpu);
    CHECK("slice_100us", per_s > 2000, "%llu switches/s on one CPU (the 500us floor caps it near 2000)",
          (unsigned long long)per_s);
}

int main(void) {
    test_clocks();
    test_slices();
    if (fails == 0) { printf("PASS hrtest\n"); } else { printf("hrtest: %d FAILED\n", fails); }
    return fails ? 1 : 0;
}
