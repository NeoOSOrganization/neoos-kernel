// kernel/lib/rand.c -- a seeded CSPRNG for AT_RANDOM (stack-guard canary seed)
// NOT a real entropy pool: no reseeding, no /dev/random. It exists so AT_RANDOM
// is not the tick counter. docs/stdlib.md records the limitation.

#include "lib/rand.h"
#include "arch/cpu.h"
#include "drivers/char/rtc.h"
#include "drivers/char/serial.h"
#include "sync/lock.h"
#include <stdint.h>
#include <stdbool.h>

// splitmix64 to expand the seed, xoshiro256** as the stream.
static uint64_t s[4];
static struct spinlock rand_lock;

static uint64_t sm_next(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void rand_init(void) {
    // Seed from RTC + TSC + stack address + RDRAND (if available).
    // Called on the BSP before SMP start, so no lock is needed.
    uint64_t seed = (uint64_t)rtc_boot_epoch();
    seed ^= rdtsc();
    seed ^= (uint64_t)(uintptr_t)&seed;

    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (c & (1u << 30)) {           // RDRAND available (bit 30 of ECX)
        uint64_t r;
        if (rdrand64(&r)) { seed ^= r; }
    }

    // Initialize the spinlock (will be used later by rand_u64).
    spin_init(&rand_lock, LOCK_RANK_SERIAL, "rand");

    // Expand seed into the four state values using splitmix64.
    uint64_t sm = seed;
    for (int i = 0; i < 4; i++) { s[i] = sm_next(&sm); }

    serial_write_string("[rand] initialized\n");
}

uint64_t rand_u64(void) {
    // Take the lock since this is called from spawn on any CPU.
    uint64_t flags = spin_lock_raw(&rand_lock);

    // xoshiro256**: output and state update.
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 45);

    spin_unlock_raw(&rand_lock, flags);
    return result;
}

void rand_bytes(void *buf, uint64_t n) {
    uint8_t *p = buf;
    while (n) {
        uint64_t r = rand_u64();
        uint64_t k = n < 8 ? n : 8;
        for (uint64_t i = 0; i < k; i++) { p[i] = (uint8_t)(r >> (i * 8)); }
        p += k; n -= k;
    }
}

void rand_selftest(void) {
    uint8_t a[16], b[16];
    rand_bytes(a, 16);
    rand_bytes(b, 16);

    int all_zero = 1, sequential = 1, same = 1;
    for (int i = 0; i < 16; i++) {
        if (a[i] != 0) all_zero = 0;
        if (i && a[i] != (uint8_t)(a[i-1] + 1)) sequential = 0;
        if (a[i] != b[i]) same = 0;
    }

    if (all_zero || sequential || same) {
        serial_write_string("[rand] selftest FAILED\n");
        return;
    }
    serial_write_string("[rand] selftest passed\n");
}
