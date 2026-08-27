#include "lock.h"
#include "cpu_local.h"
#include "serial.h"

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
    panic_puts("[lock] PANIC: ");
    panic_puts(msg);
    panic_puts(" acquiring=");
    panic_puts(a ? a : "(null)");
    panic_puts(" holding=");
    panic_puts(b ? b : "(none)");
    panic_puts("\n");
    for (;;) { __asm__ volatile ("hlt"); }
}

void spin_init(struct spinlock *l, uint8_t rank, const char *name) {
    l->locked = 0;
    l->rank   = rank;
    l->name   = name;
}

int lock_held_depth(void) { return this_cpu()->held_depth; }

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
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }

    c->held_ranks[c->held_depth++] = l->rank;
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    struct cpu *c = this_cpu();
    if (c->held_depth <= 0) {
        lock_panic("unlock with nothing held", l->name, 0);
    }
    c->held_depth--;
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
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
