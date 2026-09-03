#ifndef NEOOS_NETRX_H
#define NEOOS_NETRX_H

#include <stdint.h>

// The receive queue, and the invariant this file exists to preserve.
//
// Until now every packet arrived through loopback, SYNCHRONOUSLY, in
// the context of whoever sent it. net_ipv4_input may therefore take
// locks and sleep, and the socket layer under it does exactly that.
//
// A NIC delivers in an INTERRUPT, where sleeping is a panic and taking
// a sleepable lock is an inversion waiting to be found by the gauntlet
// at three in the morning. So the interrupt handler copies frames in
// here and wakes a thread, and the THREAD runs the receive path.
//
// The alternative -- making the whole input path interrupt-safe -- was
// rejected. It would push spin_lock_irqsave down through the socket
// layer and into whatever a future protocol wants to do on receive, and
// receive is precisely where TCP will later want to do a great deal.

#define NETRX_FRAME_MAX 1536   // an Ethernet frame, rounded up
#define NETRX_QUEUE_LEN 64

struct netdev;

// Called by the RX thread, once per frame, in a context where sleeping
// is allowed. D1 registers a counter; D2 registers the Ethernet layer.
typedef void (*netrx_handler)(struct netdev *dev, const uint8_t *frame, uint32_t len);

void netrx_init(void);
void netrx_set_handler(netrx_handler fn);

// Starts the draining thread. Separate from netrx_init because the
// scheduler has to exist first.
void netrx_start(void);

// Called FROM THE INTERRUPT HANDLER. Copies the frame and returns
// immediately. Never sleeps, never calls into the stack. A full queue
// drops the frame and counts it, because a network stack that blocks
// its own interrupt handler is worse than one that drops.
void netrx_post(struct netdev *dev, const uint8_t *frame, uint32_t len);

void netrx_stats(uint64_t *queued, uint64_t *dropped, uint64_t *delivered);

#endif
