// netrx.c -- the queue between the network interrupt and the stack.

#include "netrx.h"
#include "net.h"
#include "../sync/lock.h"
#include "../sync/waitq.h"
#include "net/arp.h"
#include "drivers/char/timer.h"
#include "../sched/proc.h"
#include "../drivers/char/serial.h"

struct netrx_slot {
    struct netdev *dev;
    uint32_t       len;
    uint8_t        data[NETRX_FRAME_MAX];
};

static struct netrx_slot queue[NETRX_QUEUE_LEN];
static uint32_t head, tail;          // head == tail means empty
static struct spinlock  rx_lock;     // LOCK_RANK_NETRX
static struct waitq     rx_wait;
static netrx_handler    handler;

static uint64_t stat_queued, stat_dropped, stat_delivered;

void netrx_init(void) {
    spin_init(&rx_lock, LOCK_RANK_NETRX, "netrx");
    waitq_init(&rx_wait);
    head = tail = 0;
    handler = 0;
}

netrx_handler netrx_set_handler(netrx_handler fn) {
    netrx_handler prev = handler;
    handler = fn;
    return prev;
}

uint64_t netrx_boot_window_open(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; sti" : "=r"(rflags) :: "memory");
    return rflags;
}

void netrx_boot_window_close(uint64_t saved) {
    if (!(saved & (1u << 9))) { __asm__ volatile ("cli"); }
}

void netrx_post(struct netdev *dev, const uint8_t *frame, uint32_t len) {
    if (len > NETRX_FRAME_MAX) { len = NETRX_FRAME_MAX; }

    uint64_t f = spin_lock_irqsave(&rx_lock);
    uint32_t next = (tail + 1) % NETRX_QUEUE_LEN;
    if (next == head) {
        // Full. Drop, and say so in the counters rather than in the log:
        // a driver that logs per dropped frame under load spends the
        // whole interrupt in the serial port and drops far more.
        stat_dropped++;
        spin_unlock_irqrestore(&rx_lock, f);
        return;
    }

    struct netrx_slot *s = &queue[tail];
    s->dev = dev;
    s->len = len;
    for (uint32_t i = 0; i < len; i++) { s->data[i] = frame[i]; }
    tail = next;
    stat_queued++;
    spin_unlock_irqrestore(&rx_lock, f);

    // OUTSIDE the lock: waitq_wake_all takes the waitq's own lock, which
    // ranks below this one, and this runs in interrupt context.
    waitq_wake_all(&rx_wait);
}

// One frame off the queue, into `out`. Returns its length, or 0.
static uint32_t netrx_take(struct netdev **dev_out, uint8_t *out) {
    uint64_t f = spin_lock_irqsave(&rx_lock);
    if (head == tail) {
        spin_unlock_irqrestore(&rx_lock, f);
        return 0;
    }
    struct netrx_slot *s = &queue[head];
    uint32_t len = s->len;
    *dev_out = s->dev;
    for (uint32_t i = 0; i < len; i++) { out[i] = s->data[i]; }
    head = (head + 1) % NETRX_QUEUE_LEN;
    spin_unlock_irqrestore(&rx_lock, f);
    return len;
}

static void netrx_thread(void) {
    // Its own copy, off the queue, so the handler can be slow without
    // holding a slot the interrupt handler needs.
    static uint8_t frame[NETRX_FRAME_MAX];

    for (;;) {
        struct netdev *dev = 0;
        uint32_t len = netrx_take(&dev, frame);
        if (len == 0) {
            // Re-check under the lock before sleeping, or a frame posted
            // between the take above and the sleep below is never
            // noticed until the next one arrives.
            uint64_t f = spin_lock_irqsave(&rx_lock);
            if (head == tail) {
                // waitq_sleep hands the lock over: it enqueues under the
                // wait queue's own lock and only then releases this one,
                // so a frame posted in between cannot wake an empty
                // queue. It returns with the lock held again.
                //
                // D2: while an ARP request is outstanding, sleep on a
                // TIMEOUT instead. A retry that only fires when a frame
                // happens to arrive is not a retry -- and the frame it
                // is waiting for is the one that is not coming. The
                // timeout is armed only while something is pending, so
                // an idle machine still sleeps until the next frame.
                if (arp_pending()) {
                    waitq_sleep_timeout(&rx_wait, &rx_lock, timer_ticks() + 1);
                } else {
                    waitq_sleep(&rx_wait, &rx_lock);
                }
            }
            spin_unlock_irqrestore(&rx_lock, f);
            arp_tick();
            continue;
        }

        stat_delivered++;
        if (handler) { handler(dev, frame, len); }
        arp_tick();
    }
}

void netrx_start(void) {
    thread_alloc_kernel(netrx_thread);
}

void netrx_stats(uint64_t *queued, uint64_t *dropped, uint64_t *delivered) {
    if (queued)    { *queued = stat_queued; }
    if (dropped)   { *dropped = stat_dropped; }
    if (delivered) { *delivered = stat_delivered; }
}
