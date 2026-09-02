#include "sched/pid_alloc.h"

// This allocator hands out PID *numbers* only. Process lookup by pid is
// proc_table_lookup()'s bucketed hash (kernel/sched/proc_table.c), which
// has per-bucket locks and refcounted results. A radix tree with
// insert/remove/lookup lived here until CS2 and was deleted: nothing
// ever called those three functions, so the tree was never populated,
// and its lookup path read shared nodes with no lock at all -- a race
// that was only ever unreachable by accident.
#include "drivers/char/serial.h"

static inline int bit_test(const uint64_t *bm, int pid) {
    return (bm[pid >> 6] >> (pid & 63)) & 1ULL;
}
static inline void bit_set(uint64_t *bm, int pid) {
    bm[pid >> 6] |= 1ULL << (pid & 63);
}
static inline void bit_clear(uint64_t *bm, int pid) {
    bm[pid >> 6] &= ~(1ULL << (pid & 63));
}

void pid_allocator_init(struct pid_allocator *alloc) {
    for (int i = 0; i < PID_WORDS; i++) { alloc->bitmap[i] = 0; }
    bit_set(alloc->bitmap, 0);   // pid 0 is the idle task's, never handed out
    alloc->cursor  = 1;
    alloc->wrapped = 0;
    spin_init(&alloc->lock, LOCK_RANK_PROCTABLE, "pid_alloc");
}

// Scans [from, to) for a free pid, a word at a time. Returns 0 if the
// range is fully allocated.
static int scan(struct pid_allocator *alloc, int from, int to) {
    int pid = from;
    while (pid < to) {
        int w = pid >> 6;
        if (alloc->bitmap[w] == ~0ULL) {
            // Whole word taken; skip to the next one rather than testing
            // 64 bits individually. With a million pids and a busy
            // machine this is the difference between a scan and a crawl.
            pid = (w + 1) << 6;
            continue;
        }
        if (!bit_test(alloc->bitmap, pid)) { return pid; }
        pid++;
    }
    return 0;
}

int pid_alloc(struct pid_allocator *alloc) {
    uint64_t f = spin_lock_irqsave(&alloc->lock);

    int pid = scan(alloc, alloc->cursor, MAX_PIDS);
    if (!pid) {
        // Past the end: wrap and scan the low range. This is the case
        // the old allocator did not have -- it returned 0 here, for
        // good, and every later fork and spawn failed.
        alloc->wrapped = 1;
        pid = scan(alloc, 1, alloc->cursor);
    }
    if (!pid) {
        spin_unlock_irqrestore(&alloc->lock, f);
        return 0;                      // genuinely all 2^20 in use
    }

    bit_set(alloc->bitmap, pid);
    // Two ways round the end, and BOTH are a wrap. The scan above
    // failing is one; the cursor simply advancing off the top after
    // allocating the last pid is the other, and it is the common one --
    // it is how the space is normally traversed. Recording only the
    // first left `wrapped` clear on a machine that had plainly been all
    // the way round, which the selftest caught.
    if (pid + 1 >= MAX_PIDS) {
        alloc->cursor  = 1;
        alloc->wrapped = 1;
    } else {
        alloc->cursor = pid + 1;
    }

    spin_unlock_irqrestore(&alloc->lock, f);
    return pid;
}

void pid_free(struct pid_allocator *alloc, int pid) {
    if (pid <= 0 || pid >= MAX_PIDS) { return; }

    uint64_t f = spin_lock_irqsave(&alloc->lock);
    bit_clear(alloc->bitmap, pid);
    spin_unlock_irqrestore(&alloc->lock, f);
    // The cursor is deliberately NOT moved back to `pid`. Reuse distance
    // is the point; see the header.
}

int pid_alloc_has_wrapped(struct pid_allocator *alloc) {
    uint64_t f = spin_lock_irqsave(&alloc->lock);
    int w = alloc->wrapped;
    spin_unlock_irqrestore(&alloc->lock, f);
    return w;
}

// ---------------------------------------------------------------- test
//
// Two seams exist for the selftest alone. Walking the cursor round the
// real way costs a million allocations at every boot, and filling the
// space to check exhaustion costs another; both are seconds of boot
// time to prove something the allocator does in one branch. These place
// the allocator in the state to be tested and then let the REAL
// pid_alloc run against it, so the paths under test are the shipping
// ones.

static void pid_test_seek(struct pid_allocator *alloc, int cursor) {
    uint64_t f = spin_lock_irqsave(&alloc->lock);
    alloc->cursor = cursor;
    spin_unlock_irqrestore(&alloc->lock, f);
}

// Marks every pid in use except `keep`.
static void pid_test_fill_except(struct pid_allocator *alloc, int keep) {
    uint64_t f = spin_lock_irqsave(&alloc->lock);
    for (int i = 0; i < PID_WORDS; i++) { alloc->bitmap[i] = ~0ULL; }
    bit_clear(alloc->bitmap, keep);
    spin_unlock_irqrestore(&alloc->lock, f);
}

// The allocator is 128 KiB of bitmap, too large for the kernel stack, so
// the selftest works on a static one rather than a local copy.
static struct pid_allocator selftest_alloc;

void pid_alloc_selftest(void) {
    struct pid_allocator *a = &selftest_alloc;
    pid_allocator_init(a);

    // Distinctness: a run of allocations with nothing freed must never
    // repeat a pid.
    int ids[64];
    for (int i = 0; i < 64; i++) {
        ids[i] = pid_alloc(a);
        if (ids[i] <= 0) {
            serial_write_string("[pid] selftest FAILED: allocation returned 0\n");
            return;
        }
        for (int j = 0; j < i; j++) {
            if (ids[j] == ids[i]) {
                serial_write_string("[pid] selftest FAILED: duplicate live pid\n");
                return;
            }
        }
    }

    // Reuse DISTANCE. The old allocator kept a LIFO free list and handed
    // a freed pid straight back on the next call, which is what let a
    // signal aimed at a process that had just exited land on an
    // unrelated new one (userland/sigstorm.c has a caught instance).
    int freed = ids[10];
    pid_free(a, freed);
    int next = pid_alloc(a);
    if (next == freed) {
        serial_write_string("[pid] selftest FAILED: freed pid reused immediately\n");
        return;
    }
    pid_free(a, next);

    // WRAPAROUND, which the old allocator did not have at all: at
    // MAX_PIDS it returned 0 for good, and every later fork failed. With
    // the cursor near the top, allocation must run off the end, wrap,
    // and come back with a low pid -- including `freed`, so the space is
    // not leaked either.
    pid_test_seek(a, MAX_PIDS - 4);
    int wrapped_to = 0;
    for (int i = 0; i < 32; i++) {
        int p = pid_alloc(a);
        if (p <= 0) {
            serial_write_string("[pid] selftest FAILED: allocation failed at the wrap\n");
            return;
        }
        if (p < MAX_PIDS - 4) { wrapped_to = p; break; }
    }
    if (!wrapped_to) {
        serial_write_string("[pid] selftest FAILED: cursor never wrapped\n");
        return;
    }
    if (!pid_alloc_has_wrapped(a)) {
        serial_write_string("[pid] selftest FAILED: wrap not recorded\n");
        return;
    }

    // Exhaustion must be REPORTED, not wrapped into a duplicate: with
    // one pid left, alloc must return exactly that one, and the call
    // after it must return 0 rather than a pid already in use.
    pid_test_fill_except(a, 4242);
    int last = pid_alloc(a);
    if (last != 4242) {
        serial_write_string("[pid] selftest FAILED: did not find the last free pid\n");
        return;
    }
    if (pid_alloc(a) != 0) {
        serial_write_string("[pid] selftest FAILED: allocated past exhaustion\n");
        return;
    }

    // Leave the static allocator empty rather than full: it is .bss that
    // nothing else uses, but a future caller finding it full would be a
    // confusing failure.
    pid_allocator_init(a);

    serial_write_string("[pid] selftest passed: wraps at MAX_PIDS, reports "
                        "exhaustion, does not reuse a pid immediately\n");
}
