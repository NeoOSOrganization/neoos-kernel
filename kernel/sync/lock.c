#include "sync/lock.h"
#include "smp/smp.h"
#include "arch/cpu_local.h"
#include "drivers/char/serial.h"

#ifdef NEOOS_DEBUG_LOCKSTAT
// Ranks are sparse: 0..21 today, plus the leaf block 250..255. Fold
// them into a compact slot so the tables stay small. A naive
// [256 ranks][32 buckets] table per CPU is 64 KiB, and MAX_CPUS is 128
// -- nearly 9 MiB of .bss for a debug counter. This is ~5.5 KiB per
// CPU, and only in a DEBUG_LOCKSTAT build.
#define LOCKSTAT_LOW     32
#define LOCKSTAT_SLOTS   (LOCKSTAT_LOW + 6)
#define LOCKSTAT_BUCKETS 16

static inline int lockstat_slot(uint8_t rank) {
    if (rank < LOCKSTAT_LOW) { return rank; }
    if (rank >= 250) { return LOCKSTAT_LOW + (rank - 250); }
    return -1;                       // a rank added between 32 and 249
}

struct lockstat {
    uint64_t count[LOCKSTAT_SLOTS];
    uint64_t max[LOCKSTAT_SLOTS];
    uint64_t buckets[LOCKSTAT_SLOTS][LOCKSTAT_BUCKETS];
};
static struct lockstat lockstats[MAX_CPUS];

static inline uint64_t lockstat_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// O(1) via the index cached in the block. A scan would run on every
// single unlock, which is not acceptable even in a debug build.
static inline struct lockstat *lockstat_mine(struct cpu *c) {
    int i = c->lockstat_index;
    return &lockstats[(i >= 0 && i < MAX_CPUS) ? i : 0];
}
#endif

// Writes without taking the serial lock: this is reachable from inside
// a lock acquisition, and the panicking context may already hold it.
static void panic_puts(const char *s) {
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') { serial_putc('\r'); }
        serial_putc(s[i]);
    }
}

void lock_panic(const char *msg, const char *a, const char *b) {
    __asm__ volatile ("cli");
    // Freeze the other CPUs BEFORE printing, so none of them scribbles
    // over the evidence -- or panics concurrently and interleaves its
    // own message into this one.
    smp_panic_stop_others();
    panic_puts("[lock] PANIC: ");
    panic_puts(msg);
    panic_puts(" acquiring=");
    panic_puts(a ? a : "(null)");
    panic_puts(" holding=");
    panic_puts(b ? b : "(none)");
    panic_puts("\n[lock] held stack:");
    {
        struct cpu *c = this_cpu();
        for (int i = 0; i < c->held_depth; i++) {
            panic_puts(" ");
            panic_puts(c->held_names[i] ? c->held_names[i] : "?");
        }
    }
    panic_puts("\n");
    for (;;) { __asm__ volatile ("hlt"); }
}

// How long a spin may last before it is treated as a deadlock rather
// than as contention.
//
// Every spinlock in this kernel is held with interrupts disabled and
// for a bounded number of instructions, so a wait of this length is not
// slow -- it is a cycle that will never break. Without this the symptom
// is a CPU that simply stops: no output, no panic, and (since it is
// spinning with IF clear) not even a timer tick to show it is alive.
// That is exactly how a boot failure presented, and it took a log with
// no ticks in it at all to work out which CPU had stopped, let alone
// where.
//
// The bound is a heuristic and deliberately generous: roughly twenty
// seconds under QEMU's TCG, far longer than any real hold, so a false
// positive would need a genuinely pathological workload.
#define SPIN_DEADLOCK_LIMIT 1000000000ULL

// Spins, and panics by NAME if the wait never ends. Both locks are
// named in the message -- the one being waited for, and the one this
// CPU already holds -- because a deadlock is a property of the pair.
static void spin_acquire_watched(struct spinlock *l) {
    uint64_t spins = 0;
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
        if (++spins > SPIN_DEADLOCK_LIMIT) {
            struct cpu *c = this_cpu();
            const char *held = "(none)";
            if (c->held_depth > 0) { held = "see held_ranks"; }
            lock_panic("spin timed out; deadlock", l->name ? l->name : "(unnamed)", held);
        }
    }
}

void spin_init(struct spinlock *l, uint8_t rank, const char *name) {
    l->locked = 0;
    l->rank   = rank;
    l->name   = name;
}

int lock_held_depth(void) { return this_cpu()->held_depth; }

// The innermost held lock's name, for assertions that fire BECAUSE
// something is held and whose whole value is saying what.
const char *lock_held_top_name(void) {
    struct cpu *c = this_cpu();
    if (c->held_depth <= 0) { return "(none)"; }
    const char *n = c->held_names[c->held_depth - 1];
    return n ? n : "(unnamed)";
}

int lock_rank_ok(uint8_t rank) {
    struct cpu *c = this_cpu();
    if (c->held_depth == 0) { return 1; }
    return rank > c->held_ranks[c->held_depth - 1];
}

uint64_t spin_lock_raw(struct spinlock *l) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
    return flags;
}

void spin_unlock_raw(struct spinlock *l, uint64_t flags) {
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

uint64_t spin_lock_irqsave(struct spinlock *l) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    if (!lock_rank_ok(l->rank)) {
        lock_panic("rank inversion", l->name, "see previous acquire");
    }
    struct cpu *c = this_cpu();
    if (c->held_depth >= LOCK_MAX_HELD) {
        lock_panic("held-lock stack overflow", l->name, 0);
    }

    // Uncontended on one CPU, but a real atomic so the SMP milestone
    // changes nothing here.
    spin_acquire_watched(l);

    c->held_names[c->held_depth] = l->name;
    c->held_ranks[c->held_depth++] = l->rank;
#ifdef NEOOS_DEBUG_LOCKSTAT
    c->lockstat_acquire_tsc[c->held_depth - 1] = lockstat_now();
#endif
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    struct cpu *c = this_cpu();
    if (c->held_depth <= 0) {
        lock_panic("unlock with nothing held", l->name, 0);
    }
#ifdef NEOOS_DEBUG_LOCKSTAT
    {
        int sl = lockstat_slot(l->rank);
        if (sl >= 0) {
            uint64_t held = lockstat_now() - c->lockstat_acquire_tsc[c->held_depth - 1];
            struct lockstat *ls = lockstat_mine(c);
            ls->count[sl]++;
            if (held > ls->max[sl]) { ls->max[sl] = held; }
            unsigned b = 0;
            while (b < LOCKSTAT_BUCKETS - 1 && (held >> (b + 1))) { b++; }
            ls->buckets[sl][b]++;
        }
    }
#endif
    c->held_depth--;
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

// Address order is the total order. The per-CPU run queue locks live
// inside cpus[], so address order and CPU-index order are the same
// order -- and address order also stays correct for any other same-rank
// pair.
uint64_t spin_lock_ordered_pair(struct spinlock *a, struct spinlock *b) {
    if (a == b) { lock_panic("ordered pair given one lock twice", a->name, 0); }
    struct spinlock *first  = (a < b) ? a : b;
    struct spinlock *second = (a < b) ? b : a;

    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    if (!lock_rank_ok(first->rank)) {
        lock_panic("rank inversion", first->name, "see previous acquire");
    }
    struct cpu *c = this_cpu();
    if (c->held_depth + 2 > LOCK_MAX_HELD) {
        lock_panic("held-lock stack overflow", first->name, 0);
    }

    spin_acquire_watched(first);
    spin_acquire_watched(second);
    // Both ranks are recorded, so the checker still sees the true depth
    // and still rejects a third lock of this rank.
    c->held_names[c->held_depth]   = first->name;
    c->held_ranks[c->held_depth++] = first->rank;
    c->held_names[c->held_depth]   = second->name;
    c->held_ranks[c->held_depth++] = second->rank;
    return flags;
}

void spin_unlock_ordered_pair(struct spinlock *a, struct spinlock *b,
                              uint64_t flags) {
    struct cpu *c = this_cpu();
    if (c->held_depth < 2) {
        lock_panic("ordered-pair unlock with <2 held", a->name, 0);
    }
    c->held_depth -= 2;
    struct spinlock *first  = (a < b) ? a : b;
    struct spinlock *second = (a < b) ? b : a;
    // Reverse of acquisition, by convention; release order does not
    // affect correctness.
    __atomic_store_n(&second->locked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&first->locked,  0u, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

void lock_selftest(void) {
    struct spinlock outer, inner;
    spin_init(&outer, LOCK_RANK_PROCESS, "selftest-outer");
    spin_init(&inner, LOCK_RANK_RUNQUEUE, "selftest-inner");

    if (lock_held_depth() != 0) {
        serial_write_string("[lock] selftest FAILED: depth not 0 at entry\n");
        return;
    }

    uint64_t f1 = spin_lock_irqsave(&outer);
    if (lock_held_depth() != 1) {
        serial_write_string("[lock] selftest FAILED: depth after one acquire\n");
        return;
    }
    // Ascending is legal, descending is not -- checked WITHOUT
    // acquiring, so the panic path is never entered.
    if (!lock_rank_ok(LOCK_RANK_RUNQUEUE)) {
        serial_write_string("[lock] selftest FAILED: ascending rank rejected\n");
        return;
    }
    if (lock_rank_ok(LOCK_RANK_PROCTABLE)) {
        serial_write_string("[lock] selftest FAILED: inversion not detected\n");
        return;
    }
    if (lock_rank_ok(LOCK_RANK_PROCESS)) {
        serial_write_string("[lock] selftest FAILED: equal rank accepted\n");
        return;
    }

    uint64_t f2 = spin_lock_irqsave(&inner);
    if (lock_held_depth() != 2) {
        serial_write_string("[lock] selftest FAILED: depth after two acquires\n");
        return;
    }
    spin_unlock_irqrestore(&inner, f2);
    spin_unlock_irqrestore(&outer, f1);

    if (lock_held_depth() != 0) {
        serial_write_string("[lock] selftest FAILED: depth not 0 at exit\n");
        return;
    }
    if (outer.locked != 0 || inner.locked != 0) {
        serial_write_string("[lock] selftest FAILED: lock still held\n");
        return;
    }

    // Two locks of EQUAL rank are an inversion for the normal path, and
    // must stay one -- work stealing is the only caller allowed to hold
    // two run queues, and it goes through the ordered-pair helper.
    struct spinlock qa, qb;
    spin_init(&qa, LOCK_RANK_RUNQUEUE, "selftest-queue-a");
    spin_init(&qb, LOCK_RANK_RUNQUEUE, "selftest-queue-b");

    uint64_t fp = spin_lock_ordered_pair(&qa, &qb);
    if (lock_held_depth() != 2) {
        serial_write_string("[lock] selftest FAILED: ordered pair depth\n");
        return;
    }
    if (qa.locked != 1 || qb.locked != 1) {
        serial_write_string("[lock] selftest FAILED: ordered pair did not lock both\n");
        return;
    }
    spin_unlock_ordered_pair(&qa, &qb, fp);
    if (lock_held_depth() != 0 || qa.locked != 0 || qb.locked != 0) {
        serial_write_string("[lock] selftest FAILED: ordered pair not released\n");
        return;
    }

    // CS0: the leaf ranks must all be distinct. rand_lock shared
    // LOCK_RANK_SERIAL, which made a genuine serial/rand inversion
    // invisible -- the checker treats equal ranks as one class.
    if (LOCK_RANK_RAND == LOCK_RANK_SERIAL ||
        LOCK_RANK_VT   == LOCK_RANK_FBCON  ||
        LOCK_RANK_VT   == LOCK_RANK_PTY    ||
        LOCK_RANK_VT   == LOCK_RANK_DEVFS) {
        serial_write_string("[lock] selftest FAILED: leaf ranks collide\n");
        return;
    }
    // The VT lock is taken while a tty lock is held and takes fbcon
    // beneath it; that chain must be strictly ascending.
    if (!(LOCK_RANK_TTY < LOCK_RANK_VT && LOCK_RANK_VT < LOCK_RANK_FBCON)) {
        serial_write_string("[lock] selftest FAILED: VT rank not between TTY and FBCON\n");
        return;
    }

    // CS0: prove the checker rejects a descending acquire at each rank
    // converted from spin_lock_raw. Holding a high leaf rank, every
    // lower rank must be refused -- otherwise the conversions made those
    // locks *look* checked without the checking actually working there.
    {
        struct spinlock probe;
        spin_init(&probe, LOCK_RANK_FBCON, "rank-probe");
        uint64_t f = spin_lock_irqsave(&probe);
        int bad = 0;
        if (lock_rank_ok(LOCK_RANK_VT))    { bad = 1; }   // 251 under 254
        if (lock_rank_ok(LOCK_RANK_PTY))   { bad = 1; }   // 252 under 254
        if (lock_rank_ok(LOCK_RANK_DEVFS)) { bad = 1; }   // 253 under 254
        if (lock_rank_ok(LOCK_RANK_RAND))  { bad = 1; }   // 250 under 254
        if (lock_rank_ok(LOCK_RANK_FBCON)) { bad = 1; }   // equal rank is an inversion
        // and the legal direction is still legal
        if (!lock_rank_ok(LOCK_RANK_SERIAL)) { bad = 1; } // 255 above 254
        spin_unlock_irqrestore(&probe, f);
        if (bad) {
            serial_write_string("[lock] selftest FAILED: rank check wrong at a CS0 rank\n");
            return;
        }
    }

    serial_write_string("[lock] selftest passed\n");
}

void mutex_init(struct mutex *m, uint8_t rank, const char *name) {
    m->locked = 0;
    m->rank   = rank;
    m->name   = name;
    waitq_init(&m->waiters);
    spin_init(&m->guard, rank, name);
}

void mutex_lock(struct mutex *m) {
    // Sleeping with a spinlock held would deadlock every other CPU once
    // SMP lands, and hides an ordering bug even on one CPU.
    if (lock_held_depth() != 0) {
        lock_panic("mutex taken while holding a spinlock", m->name, 0);
    }
    uint64_t f = spin_lock_irqsave(&m->guard);
    while (m->locked) {
        waitq_sleep(&m->waiters, &m->guard);
    }
    m->locked = 1;
    spin_unlock_irqrestore(&m->guard, f);
}

void mutex_unlock(struct mutex *m) {
    uint64_t f = spin_lock_irqsave(&m->guard);
    m->locked = 0;
    waitq_wake_one(&m->waiters);
    spin_unlock_irqrestore(&m->guard, f);
}

void lock_stats_dump(void) {
#ifdef NEOOS_DEBUG_LOCKSTAT
    // rank, total acquisitions, longest hold in TSC ticks, and how many
    // holds landed in the top bucket (>= 2^15 ticks) -- the column that
    // says "this lock is held a long time", which is what CS5's
    // vfs_lock and poll_broadcast work needs to move.
    serial_write_string("[lockstat] rank count max_tsc long_holds\n");
    for (int sl = 0; sl < LOCKSTAT_SLOTS; sl++) {
        uint64_t count = 0, max = 0, longh = 0;
        for (int i = 0; i < MAX_CPUS; i++) {
            count += lockstats[i].count[sl];
            if (lockstats[i].max[sl] > max) { max = lockstats[i].max[sl]; }
            longh += lockstats[i].buckets[sl][LOCKSTAT_BUCKETS - 1];
        }
        if (!count) { continue; }
        uint64_t rank = (sl < LOCKSTAT_LOW) ? (uint64_t)sl
                                            : (uint64_t)(250 + (sl - LOCKSTAT_LOW));
        serial_write_string("[lockstat] ");
        serial_write_hex64(rank);
        serial_write_string(" ");
        serial_write_hex64(count);
        serial_write_string(" ");
        serial_write_hex64(max);
        serial_write_string(" ");
        serial_write_hex64(longh);
        serial_write_string("\n");
    }
#endif
}
