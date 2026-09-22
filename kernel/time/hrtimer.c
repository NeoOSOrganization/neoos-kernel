// Per-CPU high-resolution timers: an rbtree of absolute-ns expiries per
// CPU, the device always programmed for the leftmost. Callbacks run in
// the timer interrupt, IRQs off, and never sleep. See the hrtimer spec,
// section 2.
#include "time/hrtimer.h"
#include "time/clockevent.h"
#include "time/ktime.h"
#include "arch/cpu_local.h"
#include "sync/lock.h"
#include "sched/proc.h"
#include "drivers/char/serial.h"

#define HRTIMER_INACTIVE 0
#define HRTIMER_QUEUED   1
#define HRTIMER_RUNNING  2

struct hrtimer_base {
    struct spinlock       lock;
    struct rb_root_cached root;
    uint64_t              programmed;   // expiry the device is armed for; UINT64_MAX none
    int                   in_interrupt; // defer device writes to the pass's end
    int                   resched;
};
static struct hrtimer_base bases[MAX_CPUS];

static struct hrtimer_base *this_base(void) { return &bases[this_cpu() - &cpus[0]]; }

void hrtimer_cpu_init(void) {
    struct hrtimer_base *b = this_base();
    spin_init(&b->lock, LOCK_RANK_TIMEOUT, "hrtimer-base");
    b->root = RB_ROOT_CACHED;
    b->programmed = UINT64_MAX;
}

void hrtimer_init(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *)) {
    RB_CLEAR_NODE(&t->node);
    t->fn = fn; t->state = HRTIMER_INACTIVE; t->expires_ns = 0; t->cpu = 0;
}

static void enqueue_locked(struct hrtimer_base *b, struct hrtimer *t) {
    struct rb_node **link = &b->root.rb_root.rb_node, *parent = 0;
    int leftmost = 1;
    while (*link) {
        parent = *link;
        struct hrtimer *e = rb_entry(parent, struct hrtimer, node);
        if (t->expires_ns < e->expires_ns) { link = &parent->rb_left; }
        else { link = &parent->rb_right; leftmost = 0; }
    }
    rb_link_node(&t->node, parent, link);
    rb_insert_color_cached(&t->node, &b->root, leftmost);
    t->state = HRTIMER_QUEUED;
}

static void dequeue_locked(struct hrtimer_base *b, struct hrtimer *t) {
    rb_erase_cached(&t->node, &b->root);
    RB_CLEAR_NODE(&t->node);
    t->state = HRTIMER_INACTIVE;
}

static uint64_t first_expiry_locked(struct hrtimer_base *b) {
    struct rb_node *n = rb_first_cached(&b->root);
    return n ? rb_entry(n, struct hrtimer, node)->expires_ns : UINT64_MAX;
}

// Re-program only when the leftmost moved earlier than what is armed:
// starting a later timer costs no device write. A cancelled leftmost
// just leaves one spurious interrupt, whose pass re-programs.
static void reprogram_locked(struct hrtimer_base *b) {
    if (b->in_interrupt) { return; }
    uint64_t first = first_expiry_locked(b);
    if (first < b->programmed) { b->programmed = first; clockevent_program(first); }
}

int hrtimer_try_cancel(struct hrtimer *t) {
    for (;;) {
        uint8_t s = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
        if (s == HRTIMER_INACTIVE) { return 0; }
        if (s == HRTIMER_RUNNING)  { return -1; }
        uint8_t cpu = t->cpu;
        struct hrtimer_base *b = &bases[cpu];
        uint64_t f = spin_lock_irqsave(&b->lock);
        if (t->state == HRTIMER_QUEUED && t->cpu == cpu) {
            dequeue_locked(b, t);
            spin_unlock_irqrestore(&b->lock, f);
            return 1;
        }
        spin_unlock_irqrestore(&b->lock, f);   // moved or changed; look again
    }
}

int hrtimer_cancel(struct hrtimer *t) {
    for (;;) {
        int r = hrtimer_try_cancel(t);
        if (r >= 0) { return r; }
        __asm__ volatile ("pause");
    }
}

void hrtimer_start(struct hrtimer *t, uint64_t expires_ns) {
    hrtimer_cancel(t);
    struct hrtimer_base *b = this_base();
    uint64_t f = spin_lock_irqsave(&b->lock);
    t->expires_ns = expires_ns;
    t->cpu = (uint8_t)(this_cpu() - &cpus[0]);
    enqueue_locked(b, t);
    reprogram_locked(b);
    spin_unlock_irqrestore(&b->lock, f);
}

void hrtimer_forward_now(struct hrtimer *t, uint64_t interval_ns) {
    uint64_t now = ktime_get_ns();
    if (t->expires_ns > now) { return; }
    uint64_t missed = (now - t->expires_ns) / interval_ns + 1;
    t->expires_ns += missed * interval_ns;
}

int hrtimer_active(const struct hrtimer *t) { return t->state != HRTIMER_INACTIVE; }
void hrtimer_request_resched(void) { this_base()->resched = 1; }

void hrtimer_interrupt(void) {
    struct hrtimer_base *b = this_base();
    uint64_t f = spin_lock_irqsave(&b->lock);
    b->in_interrupt = 1;
    b->programmed = UINT64_MAX;
    for (;;) {
        struct rb_node *n = rb_first_cached(&b->root);
        if (!n) { break; }
        struct hrtimer *t = rb_entry(n, struct hrtimer, node);
        if (t->expires_ns > ktime_get_ns()) { break; }
        dequeue_locked(b, t);
        __atomic_store_n(&t->state, HRTIMER_RUNNING, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&b->lock, f);
        enum hrtimer_restart r = t->fn(t);
        f = spin_lock_irqsave(&b->lock);
        if (r == HRTIMER_RESTART) {
            enqueue_locked(b, t);                 // fn moved expires_ns forward
        } else {
            __atomic_store_n(&t->state, HRTIMER_INACTIVE, __ATOMIC_RELEASE);
        }
    }
    b->in_interrupt = 0;
    // ARM BEFORE any schedule(): schedule() switches stacks, and this
    // frame resumes only when the preempted task is picked again. A CPU
    // that switches away with its device unarmed stops taking timer
    // interrupts for good.
    reprogram_locked(b);
    int resched = b->resched; b->resched = 0;
    spin_unlock_irqrestore(&b->lock, f);
    if (resched) { schedule(); }
}

// ---- selftest -------------------------------------------------------

static volatile int order_seen[3], order_n;
static enum hrtimer_restart st_fn(struct hrtimer *t) {
    order_seen[order_n++] = (int)(t->expires_ns & 3);   // tag in low bits
    return HRTIMER_NORESTART;
}
static volatile int restart_left;
static enum hrtimer_restart st_restart(struct hrtimer *t) {
    if (--restart_left > 0) { hrtimer_forward_now(t, 200000); return HRTIMER_RESTART; }
    return HRTIMER_NORESTART;
}

// Runs on kmain's boot path, where interrupts are otherwise off: each
// wait enables them only while halted ("sti; hlt" takes the interrupt
// that ends the hlt, "cli" closes the window again).
void hrtimer_selftest(void) {
    struct hrtimer a, b, c, d, r;
    hrtimer_init(&a, st_fn); hrtimer_init(&b, st_fn); hrtimer_init(&c, st_fn); hrtimer_init(&d, st_fn);
    uint64_t base = (ktime_get_ns() + 2000000) & ~3ULL;
    order_n = 0;
    hrtimer_start(&c, base + 3000000 + 2);   // out of order on purpose
    hrtimer_start(&a, base + 1000000 + 0);
    hrtimer_start(&b, base + 2000000 + 1);
    hrtimer_start(&d, base + 1500000 + 3);
    if (hrtimer_cancel(&d) != 1) { serial_write_string("[hrtimer] selftest FAILED: cancel\n"); return; }
    hrtimer_start(&b, base + 500000 + 1);    // re-key earlier: b now first
    uint64_t until = ktime_get_ns() + 50000000;
    while (order_n < 3 && ktime_get_ns() < until) { __asm__ volatile ("sti; hlt; cli" ::: "memory"); }
    if (order_n != 3 || order_seen[0] != 1 || order_seen[1] != 0 || order_seen[2] != 2) {
        serial_write_string("[hrtimer] selftest FAILED: order\n"); return;
    }
    hrtimer_init(&r, st_restart); restart_left = 5;
    hrtimer_start(&r, ktime_get_ns() + 200000);
    until = ktime_get_ns() + 50000000;
    while (restart_left > 0 && ktime_get_ns() < until) { __asm__ volatile ("sti; hlt; cli" ::: "memory"); }
    if (restart_left != 0) { serial_write_string("[hrtimer] selftest FAILED: restart\n"); return; }
    serial_write_string("[hrtimer] selftest passed, clockevent=");
    serial_write_string(clockevent_name());
    serial_write_string("\n");
}
