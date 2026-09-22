// hrtest -- userland checks for the high-resolution timer milestone.
// Prints "hrtest: ok <name>" / "hrtest: FAIL <name> ..." per check and
// "PASS hrtest" only if every check passed.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return a < b ? -1 : a > b;
}
static uint64_t median(uint64_t *v, int n) { qsort(v, n, sizeof *v, cmp_u64); return v[n / 2]; }

// Bounds are asserted on MEDIANS: hrtest shares the machine with the
// boot's own selftests, and one of them polls the ATA drive with
// interrupts off for tens of ms, delaying any timer queued on that
// CPU. A tick-rounded kernel would put EVERY sample past 10 ms, so a
// median still separates the two cleanly. The worst case is reported.
static void test_sleeps(void) {
    // 50 x 200us: never early.
    uint64_t v[50]; int early = 0; uint64_t worst = 0;
    for (int i = 0; i < 50; i++) {
        uint64_t t0 = now_ns(CLOCK_MONOTONIC);
        struct timespec d = { 0, 200000 }; nanosleep(&d, 0);
        v[i] = now_ns(CLOCK_MONOTONIC) - t0;
        if (v[i] < 200000) { early = 1; }
        if (v[i] > worst) { worst = v[i]; }
    }
    CHECK("nanosleep_not_early", !early, "a 200us sleep returned early");
    uint64_t med = median(v, 50);
    printf("hrtest: nanosleep 200us median=%lluus worst=%lluus\n",
           (unsigned long long)(med / 1000), (unsigned long long)(worst / 1000));
    CHECK("nanosleep_median", med < 2000000, "median %lluus", (unsigned long long)(med / 1000));

    uint64_t p[5], w[5]; int abs_early = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t t0 = now_ns(CLOCK_MONOTONIC);
        poll(0, 0, 3);
        p[i] = now_ns(CLOCK_MONOTONIC) - t0;

        struct timespec abs; clock_gettime(CLOCK_MONOTONIC, &abs);
        abs.tv_nsec += 1500000; if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
        uint64_t target = (uint64_t)abs.tv_sec * 1000000000ULL + (uint64_t)abs.tv_nsec;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, 0);
        uint64_t woke = now_ns(CLOCK_MONOTONIC);
        if (woke < target) { abs_early = 1; w[i] = 0; } else { w[i] = woke - target; }
    }
    uint64_t pm = median(p, 5), wm = median(w, 5);
    CHECK("poll_3ms", p[0] >= 3000000 && pm >= 3000000 && pm < 9000000,
          "poll(3ms) median %lluus", (unsigned long long)(pm / 1000));
    CHECK("abstime", !abs_early && wm < 9000000, "woke %lluus past the deadline (median)%s",
          (unsigned long long)(wm / 1000), abs_early ? ", and once early" : "");
}

int main(void) {
    test_clocks();
    test_slices();
    test_sleeps();
    if (fails == 0) { printf("PASS hrtest\n"); } else { printf("hrtest: %d FAILED\n", fails); }
    return fails ? 1 : 0;
}
