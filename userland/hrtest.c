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

int main(void) {
    test_clocks();
    if (fails == 0) { printf("PASS hrtest\n"); } else { printf("hrtest: %d FAILED\n", fails); }
    return fails ? 1 : 0;
}
