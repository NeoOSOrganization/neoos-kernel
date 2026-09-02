#include "sync/poll_head.h"
#include "sync/waitq.h"
#include "sched/proc.h"

void poll_head_init(struct poll_head *h, const char *name) {
    h->list = 0;
    spin_init(&h->lock, LOCK_RANK_POLLHEAD, name);
}

// Never notified: see the header. It still takes registrations, which
// cost a list insert and a removal and keep poll_core's loop uniform.
static struct poll_head always_ready;
static int always_ready_ready;

struct poll_head *poll_head_always_ready(void) {
    if (!always_ready_ready) {
        poll_head_init(&always_ready, "poll-always-ready");
        always_ready_ready = 1;
    }
    return &always_ready;
}

void poll_head_register(struct poll_head *h, struct poll_reg *r, struct thread *t) {
    r->head = h;
    r->t    = t;
    uint64_t f = spin_lock_irqsave(&h->lock);
    r->next = h->list;
    h->list = r;
    spin_unlock_irqrestore(&h->lock, f);
}

void poll_head_unregister(struct poll_reg *r) {
    struct poll_head *h = r->head;
    if (!h) { return; }
    uint64_t f = spin_lock_irqsave(&h->lock);
    struct poll_reg **pp = &h->list;
    while (*pp && *pp != r) { pp = &(*pp)->next; }
    if (*pp) { *pp = r->next; }
    spin_unlock_irqrestore(&h->lock, f);
    r->head = 0;
    r->next = 0;
}

void poll_head_notify(struct poll_head *h) {
    // Everything happens under h->lock, including the wakes. That is
    // what the rank is for: LOCK_RANK_POLLHEAD sits below the waitq and
    // run-queue locks the wake reaches, and above every object lock, so
    // a driver may notify with its own lock still held.
    //
    // The flag is set on every registered poller before any of them is
    // woken. That ordering closes the lost-wakeup window: a poller that
    // has not queued itself yet sees the flag under the broadcast
    // queue's lock and declines to sleep. See waitq_poll_wait.
    uint64_t f = spin_lock_irqsave(&h->lock);
    for (struct poll_reg *r = h->list; r; r = r->next) {
        __atomic_store_n(&r->t->poll_notified, 1, __ATOMIC_RELEASE);
    }
    for (struct poll_reg *r = h->list; r; r = r->next) {
        waitq_poll_wake_thread(r->t);
    }
    spin_unlock_irqrestore(&h->lock, f);
}
