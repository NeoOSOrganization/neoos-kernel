#ifndef NEOOS_SPINLOCK_TYPES_H
#define NEOOS_SPINLOCK_TYPES_H

#include <stdint.h>

// Split out of lock.h purely to break the lock.h <-> waitq.h include
// cycle: struct waitq embeds a spinlock by value, and lock.h's struct
// mutex embeds a waitq by value. A forward declaration was enough while
// waitq only *pointed* at spinlocks; embedding one needs the definition.
struct spinlock {
    volatile uint32_t locked;
    uint8_t     rank;
    const char *name;
};

#endif
