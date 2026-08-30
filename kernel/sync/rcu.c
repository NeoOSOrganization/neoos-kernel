#include "rcu.h"
#include "../cpu_local.h"
#include "../serial.h"

/*
 * Single-CPU RCU implementation
 *
 * Grace period: A schedule() call where all preemption points are crossed.
 * Reader side: Increment per-CPU counter; schedule() acts as grace period.
 * Writer side: Wait for counter to reach 0 on all CPUs.
 *
 * Future SMP: Will need more sophisticated grace period detection
 * (e.g., quiescent state tracking per CPU, or callback batching).
 */

struct rcu_state {
    struct spinlock lock;
    struct rcu_head *callbacks;        // Queue of pending callbacks
    int grace_period_ongoing;
};

static struct rcu_state rcu_state;

void rcu_init(void) {
    spin_init(&rcu_state.lock, LOCK_RANK_SERIAL, "rcu");
    rcu_state.callbacks = 0;
    rcu_state.grace_period_ongoing = 0;
}

void rcu_read_lock(void) {
    // On single CPU, just prevent scheduling
    // On SMP, would increment per-CPU reader counter
    __asm__ volatile("cli");  // Disable interrupts (acts as read lock)
}

void rcu_read_unlock(void) {
    // On single CPU, re-enable interrupts
    // On SMP, would decrement per-CPU reader counter
    __asm__ volatile("sti");  // Re-enable interrupts
}

/*
 * Wait for grace period (all readers to exit RCU sections)
 *
 * Single-CPU approach: Invoke all callbacks; on a single CPU there
 * are no concurrent readers after all interrupts are disabled and
 * reenabled once.
 *
 * More correct single-CPU: Schedule to ensure all preemption points crossed.
 */
void synchronize_rcu(void) {
    uint64_t f = spin_lock_irqsave(&rcu_state.lock);

    // On a single CPU, we can invoke callbacks immediately
    // because there are no concurrent readers on other CPUs.
    // On SMP, would need to wait for quiescent state on all CPUs.

    struct rcu_head *h = rcu_state.callbacks;
    rcu_state.callbacks = 0;

    spin_unlock_irqrestore(&rcu_state.lock, f);

    // Invoke callbacks outside lock
    while (h) {
        struct rcu_head *next = h->next;
        h->callback(h);
        h = next;
    }
}

void call_rcu(struct rcu_head *head, void (*callback)(struct rcu_head *)) {
    if (!head || !callback) {
        return;
    }

    head->callback = callback;

    uint64_t f = spin_lock_irqsave(&rcu_state.lock);
    head->next = rcu_state.callbacks;
    rcu_state.callbacks = head;
    spin_unlock_irqrestore(&rcu_state.lock, f);
}
