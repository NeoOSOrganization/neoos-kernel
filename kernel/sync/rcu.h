#ifndef NEOOS_RCU_H
#define NEOOS_RCU_H

#include <stdint.h>
#include "../lock.h"

/*
 * Read-Copy-Update (RCU) synchronization
 *
 * RCU allows multiple readers to access shared data without locks.
 * Writers must allocate new copies, update, and wait for a grace period
 * before freeing old copies.
 *
 * Grace period: All preemption points (schedule(), interrupt returns)
 * On a single-CPU system, a schedule() is a grace period.
 * On SMP, more sophisticated tracking is needed.
 *
 * Usage:
 *   Reader (no lock needed):
 *     rcu_read_lock();
 *     ptr = rcu_dereference(shared_ptr);
 *     // Use ptr
 *     rcu_read_unlock();
 *
 *   Writer:
 *     new_ptr = kmalloc(...);
 *     *new_ptr = *old_ptr;  // Copy
 *     modify(*new_ptr);
 *     rcu_assign_pointer(shared_ptr, new_ptr);
 *     synchronize_rcu();    // Wait for readers
 *     kfree(old_ptr);       // Now safe
 */

// RCU head for deferred callback invocation
struct rcu_head {
    void (*callback)(struct rcu_head *);
    struct rcu_head *next;
};

// Reader-side: Enter RCU read-side critical section
// On single-CPU, just disables preemption (via counter)
void rcu_read_lock(void);

// Reader-side: Exit RCU read-side critical section
void rcu_read_unlock(void);

// Writer-side: Wait for all readers to exit RCU sections
// This is a blocking call; do not hold spinlocks when calling
void synchronize_rcu(void);

// Writer-side: Queue callback to run after next grace period
// Callback will be invoked asynchronously
void call_rcu(struct rcu_head *head, void (*callback)(struct rcu_head *));

/*
 * Compiler barrier: Prevent compiler from caching pointer value
 * Used in rcu_dereference to ensure fresh read from memory
 */
#define rcu_dereference(ptr) ({                    \
    typeof(ptr) _p = (ptr);                        \
    __asm__ volatile("" ::: "memory");             \
    _p;                                            \
})

/*
 * Writer barrier: Publish pointer update; readers see new value
 * Pairs with rcu_dereference; ensures all prior stores visible
 */
#define rcu_assign_pointer(ptr, new_ptr) ({        \
    __asm__ volatile("" ::: "memory");             \
    (ptr) = (new_ptr);                             \
})

#endif
