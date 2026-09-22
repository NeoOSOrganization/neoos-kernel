// A hierarchical timer wheel for coarse kernel timeouts (TCP, ARP):
// O(1) arm and cancel, fires within one bucket of its jiffy, never
// early. Callbacks run in the ktimerd kernel thread, NOT in an
// interrupt -- a TCP timer transmits. See the hrtimer spec, section 4.
//
// Deviation from the spec, recorded there: ONE global wheel and one
// ktimerd rather than one per CPU. NeoOS's timer counts do not need
// the per-CPU split, and one wheel keeps cancel trivially correct.
#include "time/wheel.h"
#include "time/hrtimer.h"
#include "time/ktime.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sched/proc.h"
#include "drivers/char/serial.h"
#include "errno.h"

#define LVL_BITS  6
#define LVL_SIZE  (1 << LVL_BITS)            // 64 buckets per level
#define LVL_SHIFT(l) (3 * (l))               // granularity 1, 8, 64, 512 jiffies
#define LEVELS    4

// The wheel proper. An instance rather than globals so the selftest can
// drive a private one on a simulated clock; the kernel has exactly one.
struct wheel_base {
    struct timer_list *buckets[LEVELS][LVL_SIZE];
    // Timers whose jiffy has come, waiting for ktimerd to run them.
    // Linked like a bucket, so del_timer can take one out before it runs.
    struct timer_list *due;
    uint64_t clk;                            // next jiffy to process
};
static struct wheel_base wb;
static struct spinlock wheel_lock;
static struct hrtimer wheel_hrt;
static struct waitq ktimerd_wait;
static volatile int ktimerd_kick;
static struct timer_list *volatile running;

uint64_t jiffies(void) { return ktime_get_ns() / JIFFY_NS; }
uint64_t msecs_to_jiffies(uint64_t ms) { return ms; }

static int level_for(uint64_t delta) {
    for (int l = 0; l < LEVELS - 1; l++) {
        if (delta < ((uint64_t)LVL_SIZE << LVL_SHIFT(l))) { return l; }
    }
    return LEVELS - 1;
}

static void link_locked(struct timer_list **head, struct timer_list *t) {
    t->next = *head;
    if (*head) { (*head)->pprev = &t->next; }
    *head = t;
    t->pprev = head;
}

// Files t in the bucket whose slot time is t's expiry rounded UP to the
// level's granularity, so it can only fire late, never early. Beyond
// the last level's horizon (~33 s) it is filed at the farthest slot and
// re-filed from there when that slot comes due.
static void enqueue_locked(struct wheel_base *b, struct timer_list *t) {
    uint64_t exp = t->expires < b->clk ? b->clk : t->expires;
    int l = level_for(exp - b->clk);
    uint64_t g = 1ULL << LVL_SHIFT(l);
    uint64_t slot_time = (exp + g - 1) & ~(g - 1);
    uint64_t max = b->clk + ((uint64_t)(LVL_SIZE - 1) << LVL_SHIFT(l));
    if (slot_time > max) { slot_time = max & ~(g - 1); }
    unsigned idx = (unsigned)((slot_time >> LVL_SHIFT(l)) & (LVL_SIZE - 1));
    link_locked(&b->buckets[l][idx], t);
}

static void detach_locked(struct timer_list *t) {
    *t->pprev = t->next;
    if (t->next) { t->next->pprev = t->pprev; }
    t->next = 0; t->pprev = 0;
}

int timer_pending(const struct timer_list *t) { return t->pprev != 0; }

// The jiffy at which ktimerd will next have work: the earliest slot
// time of any non-empty bucket (when a coarse bucket is emptied, not
// its timers' own expiries -- arming for those would wake ktimerd every
// jiffy until the bucket's slot came round). UINT64_MAX if empty.
static uint64_t next_expiry_locked(struct wheel_base *b) {
    if (b->due) { return b->clk; }
    uint64_t best = UINT64_MAX;
    for (int l = 0; l < LEVELS; l++) {
        uint64_t g = 1ULL << LVL_SHIFT(l);
        uint64_t base = (b->clk + g - 1) & ~(g - 1);           // next boundary of this level
        unsigned base_idx = (unsigned)((base >> LVL_SHIFT(l)) & (LVL_SIZE - 1));
        for (unsigned i = 0; i < LVL_SIZE; i++) {
            if (!b->buckets[l][i]) { continue; }
            uint64_t slot = base + (uint64_t)((i - base_idx) & (LVL_SIZE - 1)) * g;
            if (slot < best) { best = slot; }
        }
    }
    return best;
}

// Called with wheel_lock DROPPED: hrtimer_start takes the hrtimer base
// lock, which shares rank 16 with wheel_lock, and same-rank nesting is
// forbidden. A stale arm is harmless -- ktimerd recomputes after every
// pass, and an early wake just finds nothing due.
static void arm_for(uint64_t next) {
    if (next == UINT64_MAX) { hrtimer_try_cancel(&wheel_hrt); return; }
    hrtimer_start(&wheel_hrt, next * JIFFY_NS);
}

static void collect_expired_locked(struct wheel_base *b, uint64_t now);

// Brings the wheel clock up to now before a timer is filed against it.
// ktimerd only advances the clock when it runs, so after an idle
// stretch the clock can be seconds stale -- and a 3 ms timer filed
// against a stale clock lands in a coarse level and fires hundreds of
// ms late. An empty wheel just jumps; otherwise the elapsed jiffies are
// processed (anything due moves to the due list for ktimerd).
static void forward_locked(struct wheel_base *b, uint64_t now) {
    if (now < b->clk) { return; }
    if (next_expiry_locked(b) == UINT64_MAX) { b->clk = now; return; }
    collect_expired_locked(b, now);
}

void timer_setup(struct timer_list *t, void (*fn)(struct timer_list *)) {
    t->next = 0; t->pprev = 0; t->expires = 0; t->fn = fn;
}

void mod_timer(struct timer_list *t, uint64_t expires) {
    uint64_t f = spin_lock_irqsave(&wheel_lock);
    if (t->pprev) { detach_locked(t); }
    forward_locked(&wb, jiffies());
    t->expires = expires;
    enqueue_locked(&wb, t);
    uint64_t next = next_expiry_locked(&wb);
    spin_unlock_irqrestore(&wheel_lock, f);
    arm_for(next);
}

void timer_reduce(struct timer_list *t, uint64_t expires) {
    uint64_t f = spin_lock_irqsave(&wheel_lock);
    if (t->pprev && t->expires <= expires) { spin_unlock_irqrestore(&wheel_lock, f); return; }
    if (t->pprev) { detach_locked(t); }
    forward_locked(&wb, jiffies());
    t->expires = expires;
    enqueue_locked(&wb, t);
    uint64_t next = next_expiry_locked(&wb);
    spin_unlock_irqrestore(&wheel_lock, f);
    arm_for(next);
}

int del_timer(struct timer_list *t) {
    uint64_t f = spin_lock_irqsave(&wheel_lock);
    int was = t->pprev != 0;
    if (was) { detach_locked(t); }
    spin_unlock_irqrestore(&wheel_lock, f);
    return was;
}

int del_timer_sync(struct timer_list *t) {
    int was = del_timer(t);
    while (__atomic_load_n(&running, __ATOMIC_ACQUIRE) == t) { __asm__ volatile ("pause"); }
    return was;
}

// The hrtimer only wakes ktimerd: callbacks never run in the interrupt.
static enum hrtimer_restart wheel_hrt_fn(struct hrtimer *h) {
    (void)h;
    ktimerd_kick = 1;
    waitq_wake_one(&ktimerd_wait);
    return HRTIMER_NORESTART;
}

// Advances the wheel clock to now. At every jiffy that is a slot boundary of
// a level, that level's bucket is emptied: timers that are due move to
// the due list, the rest (filed coarsely) are re-filed finer.
static void collect_expired_locked(struct wheel_base *b, uint64_t now) {
    while (b->clk <= now) {
        for (int l = LEVELS - 1; l >= 0; l--) {
            uint64_t g = 1ULL << LVL_SHIFT(l);
            if (b->clk & (g - 1)) { continue; }               // not a slot boundary
            unsigned idx = (unsigned)((b->clk >> LVL_SHIFT(l)) & (LVL_SIZE - 1));
            struct timer_list *t = b->buckets[l][idx];
            b->buckets[l][idx] = 0;
            while (t) {
                struct timer_list *n = t->next;
                t->next = 0; t->pprev = 0;
                if (t->expires <= b->clk) { link_locked(&b->due, t); }
                else { enqueue_locked(b, t); }                // cascade down
                t = n;
            }
        }
        b->clk++;
    }
}

static void ktimerd(void) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&wheel_lock);
        collect_expired_locked(&wb, jiffies());
        // One at a time, each taken off the due list under the lock, so
        // a del_timer that wins the race really does stop it running.
        while (wb.due) {
            struct timer_list *t = wb.due;
            detach_locked(t);
            __atomic_store_n(&running, t, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&wheel_lock, f);
            t->fn(t);
            __atomic_store_n(&running, (struct timer_list *)0, __ATOMIC_RELEASE);
            f = spin_lock_irqsave(&wheel_lock);
        }
        uint64_t next = next_expiry_locked(&wb);
        spin_unlock_irqrestore(&wheel_lock, f);
        arm_for(next);
        // waitq_sleep_unless checks the kick under the queue lock, and
        // wheel_hrt_fn sets it before taking that lock to wake us: a
        // kick that lands after the pass above is never lost.
        waitq_sleep_unless(&ktimerd_wait, 0, &ktimerd_kick);
        ktimerd_kick = 0;
    }
}

void wheel_init(void) {
    spin_init(&wheel_lock, LOCK_RANK_TIMEOUT, "wheel");
    waitq_init(&ktimerd_wait);
    hrtimer_init(&wheel_hrt, wheel_hrt_fn);
    wb.clk = jiffies();
    thread_alloc_kernel(ktimerd);
}

// ---- selftest ---------------------------------------------------------
//
// Two parts.
//
// wheel_selftest(), synchronous from kmain: a PRIVATE wheel on a
// simulated clock -- the level-3 geometry is a >4 s horizon, and a boot
// is over sooner than that, so real time cannot exercise it. One timer
// per level, one beyond the horizon (clamped, then re-cascaded), and one
// cancelled timer per level; the clock is stepped a jiffy at a time.
// Every timer must fire exactly once, never before its jiffy and no
// later than its first level's granularity; no cancelled one may fire.
//
// wheel_selftest_start(), in a thread: two real timers through the real
// hrtimer + ktimerd path, waited on through the callbacks themselves.

#define SIM_N 5
static const uint64_t sim_delay[SIM_N] = { 3, 100, 1000, 5000, 40000 };  // levels 0,1,2,3, beyond
static const uint64_t sim_gone[4]      = { 5, 200, 2000, 8000 };        // one per level
static struct wheel_base sim;
static struct timer_list sim_t[SIM_N], sim_g[4];
static uint64_t sim_fired_at[SIM_N];
static int sim_fired_n[SIM_N];

static void sim_nop(struct timer_list *t) { (void)t; }

void wheel_selftest(void) {
    for (int l = 0; l < LEVELS; l++) { for (int i = 0; i < LVL_SIZE; i++) { sim.buckets[l][i] = 0; } }
    sim.due = 0;
    sim.clk = 1003;                                   // deliberately not level-aligned
    uint64_t start = sim.clk;
    for (int i = 0; i < SIM_N; i++) {
        timer_setup(&sim_t[i], sim_nop);
        sim_t[i].expires = start + sim_delay[i];
        enqueue_locked(&sim, &sim_t[i]);
    }
    for (int i = 0; i < 4; i++) {
        timer_setup(&sim_g[i], sim_nop);
        sim_g[i].expires = start + sim_gone[i];
        enqueue_locked(&sim, &sim_g[i]);
        detach_locked(&sim_g[i]);                     // del_timer, at each level
    }
    uint64_t end = start + sim_delay[SIM_N - 1] + 1024;
    for (uint64_t now = start; now <= end; now++) {
        uint64_t nx = next_expiry_locked(&sim);
        collect_expired_locked(&sim, now);
        while (sim.due) {
            struct timer_list *t = sim.due;
            detach_locked(t);
            if (now < nx) { serial_write_string("[wheel] selftest FAILED: fired before next_expiry\n"); return; }
            int k = (int)(t - sim_t);
            if (k < 0 || k >= SIM_N) { serial_write_string("[wheel] selftest FAILED: a cancelled timer fired\n"); return; }
            sim_fired_at[k] = now; sim_fired_n[k]++;
        }
    }
    for (int i = 0; i < SIM_N; i++) {
        uint64_t exp = start + sim_delay[i];
        uint64_t g = 1ULL << LVL_SHIFT(level_for(sim_delay[i]));
        if (sim_fired_n[i] != 1) { serial_write_string("[wheel] selftest FAILED: a timer fired != once\n"); return; }
        if (sim_fired_at[i] < exp) { serial_write_string("[wheel] selftest FAILED: a timer fired early\n"); return; }
        if (sim_fired_at[i] - exp >= g) { serial_write_string("[wheel] selftest FAILED: a timer fired late\n"); return; }
    }
    serial_write_string("[wheel] selftest passed\n");
}

static struct timer_list rt_t[2], rt_gone;
static const uint64_t rt_delay[2] = { 3, 100 };
static volatile uint64_t rt_fired_at[2];
static volatile int rt_fired, rt_done, rt_gone_fired;
static struct waitq rt_wait;

static void rt_fn(struct timer_list *t) {
    rt_fired_at[t - rt_t] = jiffies();
    // rt_done is set BEFORE the wake: the waiter checks it under the
    // queue lock (waitq_sleep_timeout_unless), so the last wake is never lost.
    if (__atomic_add_fetch(&rt_fired, 1, __ATOMIC_ACQ_REL) == 2) { rt_done = 1; }
    waitq_wake_all(&rt_wait);
}
static void rt_gone_fn(struct timer_list *t) { (void)t; rt_gone_fired = 1; }

static void wheel_rt_thread(void) {
    waitq_init(&rt_wait);
    uint64_t start = jiffies();
    for (int i = 0; i < 2; i++) { timer_setup(&rt_t[i], rt_fn); mod_timer(&rt_t[i], start + rt_delay[i]); }
    timer_setup(&rt_gone, rt_gone_fn);
    mod_timer(&rt_gone, start + 50);
    if (!del_timer_sync(&rt_gone)) { serial_write_string("[wheel] ktimerd FAILED: del_timer\n"); thread_exit_self(1); }
    uint64_t limit = ktime_after_ns(10ULL * NSEC_PER_SEC);   // a hang bound for a starved VM
    while (!rt_done) {
        if (waitq_sleep_timeout_unless(&rt_wait, 0, limit, &rt_done) == -ETIMEDOUT) { break; }
    }
    if (!rt_done) { serial_write_string("[wheel] ktimerd FAILED: a timer never fired\n"); thread_exit_self(1); }
    for (int i = 0; i < 2; i++) {
        if (rt_fired_at[i] < start + rt_delay[i]) { serial_write_string("[wheel] ktimerd FAILED: early\n"); thread_exit_self(1); }
    }
    if (rt_gone_fired) { serial_write_string("[wheel] ktimerd FAILED: a cancelled timer fired\n"); thread_exit_self(1); }
    serial_write_string("[wheel] ktimerd selftest passed\n");
    thread_exit_self(0);
}

void wheel_selftest_start(void) { thread_alloc_kernel(wheel_rt_thread); }
