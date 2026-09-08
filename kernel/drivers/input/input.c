#include "drivers/input/input.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/keymap_us.h"
#include "tty/tty.h"
#include "tty/vt.h"
#include "drivers/char/timer.h"
#include "drivers/char/rtc.h"
#include "drivers/char/serial.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sync/poll_head.h"
#include "mm/heap.h"
#include "sched/proc.h"
#include "smp/smp.h"
#include "drivers/input/mouse.h"
#include <errno.h>

// Simple memset implementation for freestanding environment
static void *memset_local(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *)s;
    for (uint64_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
    return s;
}

// Client structure: represents a /dev/input/event0 file descriptor
struct evdev_client {
    struct evdev_client *next;
    struct input_dev *dev;             // the device this client reads
    // Set by input_post under dev->lock, cleared by a reader that is
    // about to sleep. See evdev_client_read for why a plain
    // waitq_sleep(&readers, &dev->lock) is not available here.
    volatile int readable;
    struct input_event ring[256];      // Ring buffer, power of 2 size
    uint32_t head;                     // Next write position
    uint32_t tail;                     // Oldest unread event
    int dropped;                       // Count of dropped events (overflow)
    struct waitq readers;              // Waiters blocked on this client's queue
    struct poll_head poll;             // CS5.2: pollers registered on THIS client
    int refcount;                      // Number of file descriptors referencing this
};

struct input_dev input_kbd;
struct input_dev input_mouse;

void input_dev_init(struct input_dev *d, const char *name) {
    spin_init(&d->lock, LOCK_RANK_INPUT, name);
    d->clients = NULL;
    d->grab = NULL;
    d->name = name;
    d->ev_bits = 0;
    d->rel_bits = 0;
    memset_local(d->keystate, 0, sizeof(d->keystate));
}

// Initialize the input subsystem
void input_init(void) {
    input_dev_init(&input_kbd, "NeoOS AT keyboard");
    input_kbd.ev_bits = (1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_MSC);
}

// Helper: ring buffer is full?
static int ring_full(struct evdev_client *c) {
    return ((c->head + 1) & 0xFF) == (c->tail & 0xFF);
}

// Helper: ring buffer is empty?
static int ring_empty(struct evdev_client *c) {
    return (c->head & 0xFF) == (c->tail & 0xFF);
}

// Helper: push an event onto a client's ring buffer
static void push_event(struct evdev_client *c, struct input_event *ev) {
    uint32_t idx = c->head & 0xFF;
    c->ring[idx] = *ev;

    if (ring_full(c)) {
        // Ring full: drop oldest event
        c->tail++;
        c->dropped++;
    }

    c->head++;
}

// Helper: get the current time as input_event timestamps
static void get_timestamp(int64_t *tv_sec, int64_t *tv_usec) {
    uint64_t ticks = timer_ticks();
    uint64_t hz = 100;  // Typical HZ value; adjust if needed

    *tv_sec = rtc_boot_epoch() + (int64_t)(ticks / hz);
    *tv_usec = (int64_t)((ticks % hz) * (1000000 / hz));
}

// Push a group of events to every client of dev, then wake its readers.
// The wake happens after the lock is dropped, which is why the two loops
// are separate.
void input_post(struct input_dev *dev, const struct input_event *evs, int n) {
    if (!dev || n <= 0) { return; }

    int64_t tv_sec, tv_usec;
    get_timestamp(&tv_sec, &tv_usec);

    uint64_t flags = spin_lock_irqsave(&dev->lock);
    for (struct evdev_client *c = dev->clients; c; c = c->next) {
        for (int i = 0; i < n; i++) {
            struct input_event ev = evs[i];
            ev.tv_sec = tv_sec;
            ev.tv_usec = tv_usec;
            push_event(c, &ev);
        }
        c->readable = 1;
    }
    spin_unlock_irqrestore(&dev->lock, flags);

    for (struct evdev_client *c = dev->clients; c; c = c->next) {
        waitq_wake_all(&c->readers);
        poll_head_notify(&c->poll);
    }
}

// Process a key event: fan out to clients and optionally to TTY
void input_key_event(const struct key_event *e) {
    if (!e || e->keycode == 0) {
        return;  // Unmapped scancode, ignore
    }

    // VT hotkeys are consumed entirely, before anything else sees them:
    // no evdev event, no tty character. That is what Linux does, and it
    // is the only behaviour that makes sense -- Alt+F2 is a request to
    // the console layer, not a keystroke for whatever is reading.
    // Checked ahead of the grab, too: an evdev client holding a grab
    // must not be able to lock the user out of switching away from it.
    if (e->pressed && (e->mods & (MOD_LALT | MOD_RALT)) &&
        e->keycode >= KEY_F1 && e->keycode < KEY_F1 + VT_COUNT) {
        vt_switch(e->keycode - KEY_F1);
        return;
    }
    if (e->pressed && (e->mods & (MOD_LSHIFT | MOD_RSHIFT)) &&
        (e->keycode == KEY_PAGEUP || e->keycode == KEY_PAGEDOWN)) {
        int rows = 25;
        vt_active_geometry(0, &rows);
        int half = rows / 2;
        if (half < 1) { half = 1; }
        vt_scroll(e->keycode == KEY_PAGEUP ? -half : +half);
        return;
    }

    struct input_dev *dev = &input_kbd;

    // Snapshot the grab and update the device keystate under the lock.
    uint64_t flags = spin_lock_irqsave(&dev->lock);
    int was_grabbed = (dev->grab != NULL);
    uint32_t byte_idx = e->keycode / 8;
    uint32_t bit_idx = e->keycode % 8;
    if (byte_idx < (KEY_CNT / 8)) {
        if (e->pressed) { dev->keystate[byte_idx] |= (1u << bit_idx); }
        else            { dev->keystate[byte_idx] &= ~(1u << bit_idx); }
    }
    spin_unlock_irqrestore(&dev->lock, flags);

    // The three events one keystroke produces: the raw scancode, the
    // keycode, and the end-of-group marker.
    struct input_event evs[3];
    evs[0].type = EV_MSC; evs[0].code = MSC_SCAN;   evs[0].value = e->raw_scan;
    evs[1].type = EV_KEY; evs[1].code = e->keycode; evs[1].value = e->pressed ? 1 : 0;
    evs[2].type = EV_SYN; evs[2].code = SYN_REPORT; evs[2].value = 0;
    input_post(dev, evs, 3);

    // A grab means the keystroke belongs to the grabbing client alone,
    // so the tty must not also see it.
    if (!was_grabbed && e->ascii >= 0) {
        tty_input_char(tty_active(), (char)e->ascii);
    }
}

// Open a new evdev client
struct evdev_client *evdev_client_open(struct input_dev *dev) {
    if (!dev) { return NULL; }
    struct evdev_client *c = kmalloc(sizeof(struct evdev_client));
    if (!c) {
        return NULL;
    }

    memset_local(c, 0, sizeof(*c));
    c->dev = dev;
    c->refcount = 1;
    waitq_init(&c->readers);
    poll_head_init(&c->poll, "evdev-poll");

    uint64_t flags = spin_lock_irqsave(&dev->lock);
    c->next = dev->clients;
    dev->clients = c;
    spin_unlock_irqrestore(&dev->lock, flags);

    return c;
}

struct input_dev *evdev_client_dev(struct evdev_client *c) {
    return c ? c->dev : NULL;
}

// Close an evdev client
void evdev_client_close(struct evdev_client *c) {
    if (!c) {
        return;
    }

    struct input_dev *dev = c->dev;
    uint64_t flags = spin_lock_irqsave(&dev->lock);

    // If this client holds the grab, release it
    if (dev->grab == c) {
        dev->grab = NULL;
    }

    // Unlink from the clients list
    struct evdev_client **prev = &dev->clients;
    for (struct evdev_client *client = dev->clients; client; client = client->next) {
        if (client == c) {
            *prev = client->next;
            break;
        }
        prev = &client->next;
    }

    spin_unlock_irqrestore(&dev->lock, flags);

    // Free the client structure
    kfree(c);
}

// Read events from a client's ring buffer
int64_t evdev_client_read(struct evdev_client *c, void *buf, uint64_t len, int nonblock) {
    if (!c || !buf) {
        return -EINVAL;
    }

    // Must read whole events, not partial
    if (len < sizeof(struct input_event)) {
        return -EINVAL;
    }

    struct input_event *out = (struct input_event *)buf;
    uint32_t max_events = len / sizeof(struct input_event);
    struct input_dev *dev = c->dev;

    uint64_t flags = spin_lock_irqsave(&dev->lock);
    for (;;) {
        uint32_t events_copied = 0;
        while (events_copied < max_events && !ring_empty(c)) {
            uint32_t idx = c->tail & 0xFF;
            out[events_copied] = c->ring[idx];
            c->tail++;
            events_copied++;
        }
        if (events_copied > 0) {
            spin_unlock_irqrestore(&dev->lock, flags);
            return events_copied * (int64_t)sizeof(struct input_event);
        }
        if (nonblock) {
            spin_unlock_irqrestore(&dev->lock, flags);
            return -EAGAIN;
        }

        // This used to return -EAGAIN even here, with a comment blaming
        // lock ordering. That comment was right: LOCK_RANK_INPUT is a
        // deliberate LEAF, taken from the keyboard IRQ holding nothing,
        // and it ranks ABOVE LOCK_RANK_WAITQ -- so the usual
        // waitq_sleep(&q, &dev->lock) handoff is a rank inversion and
        // panics. poll and epoll covered for the gap, which is why
        // nothing had noticed, but a compositor reads the event fd
        // directly and would have spun at 100% CPU.
        //
        // So the lock is dropped BEFORE sleeping, and the lost-wakeup
        // window that opens is closed the way waitq_poll_wait closes
        // its own: c->readable is set by input_post under dev->lock and
        // cleared here under the same lock, and waitq_sleep_unless
        // re-checks it under the WAIT QUEUE's lock -- the same lock a
        // waker must take. A producer that delivers in the window
        // therefore either set the flag before we test it (we do not
        // sleep) or takes the queue lock after we are enqueued (its
        // wake finds us).
        c->readable = 0;
        spin_unlock_irqrestore(&dev->lock, flags);
        int rc = waitq_sleep_unless(&c->readers, 0, &c->readable);
        if (rc == -EINTR) { return -EINTR; }
        flags = spin_lock_irqsave(&dev->lock);
    }
}

// Poll for readiness
int evdev_client_poll(struct evdev_client *c) {
    if (!c) {
        return 0;
    }

    uint64_t flags = spin_lock_irqsave(&c->dev->lock);
    int result = 0;

    // Return POLLIN if the ring has events
    if (!ring_empty(c)) {
        result = 1;  // POLLIN = 0x001
    }

    spin_unlock_irqrestore(&c->dev->lock, flags);
    return result;
}

// Grab or release exclusive input
int evdev_client_grab(struct evdev_client *c, int on) {
    if (!c) {
        return -EINVAL;
    }

    struct input_dev *dev = c->dev;
    uint64_t flags = spin_lock_irqsave(&dev->lock);

    if (on) {
        // Try to acquire grab
        if (dev->grab != NULL && dev->grab != c) {
            spin_unlock_irqrestore(&dev->lock, flags);
            return -EBUSY;
        }
        dev->grab = c;
    } else {
        // Release grab
        if (dev->grab == c) {
            dev->grab = NULL;
        }
    }

    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

// Get the bitmap of keys currently pressed (for EVIOCGKEY).
//
// This reports the DEVICE's key state. It used to merge the per-client
// bitmaps of every open client, which answered a question nobody asked:
// EVIOCGKEY means "what are this device's keys doing right now", not
// "what has some reader seen".
void evdev_client_key_bitmap(struct evdev_client *c, uint8_t *out, uint64_t len) {
    if (!c || !out || len == 0) { return; }
    memset_local(out, 0, len);

    struct input_dev *dev = c->dev;
    uint64_t flags = spin_lock_irqsave(&dev->lock);
    for (uint64_t i = 0; i < len && i < (KEY_CNT / 8); i++) {
        out[i] = dev->keystate[i];
    }
    spin_unlock_irqrestore(&dev->lock, flags);
}

void evdev_client_state_bitmap(struct evdev_client *c, uint8_t *out, uint64_t len) {
    evdev_client_key_bitmap(c, out, len);
}

// Test hook: inject a key event.
//
// The injected stream carries its own modifier state. keyboard_decode's
// `mods` are built from SCANCODES and an injected key never goes near
// the 8042, so a test that wants Alt+F2 has to be able to press Alt
// first and have the F2 event see it. Tracked here, one static, in the
// same make/break shape the real decoder uses.
//
// This is also why `mods` must be assigned unconditionally below rather
// than left to whatever was on the stack: a stray MOD_LALT in an
// uninitialised field would make an injected KEY_F2 switch VTs.
static uint32_t inject_mods;

static uint32_t inject_mod_bit(uint16_t keycode) {
    switch (keycode) {
    case KEY_LSHIFT: return MOD_LSHIFT;
    case KEY_RSHIFT: return MOD_RSHIFT;
    case KEY_LCTRL:  return MOD_LCTRL;
    case KEY_RCTRL:  return MOD_RCTRL;
    case KEY_LALT:   return MOD_LALT;
    case KEY_RALT:   return MOD_RALT;
    default:         return 0;
    }
}

void input_inject_key(uint16_t keycode, int pressed) {
    struct key_event e;
    e.keycode = keycode;
    e.pressed = pressed ? 1 : 0;
    e.raw_scan = 0;  // No raw scancode for injected keys

    uint32_t bit = inject_mod_bit(keycode);
    if (bit) {
        if (pressed) { inject_mods |= bit; } else { inject_mods &= ~bit; }
    }
    e.mods = inject_mods;

    // Map keycode back to ASCII if possible (simplified)
    // For testing: just support a few common keys
    if (pressed) {
        switch (keycode) {
            case 30:  // KEY_A
                e.ascii = 'a';
                break;
            case 48:  // KEY_B
                e.ascii = 'b';
                break;
            case 46:  // KEY_C
                e.ascii = 'c';
                break;
            default:
                e.ascii = -1;
        }
    } else {
        e.ascii = -1;  // No character on release
    }

    input_key_event(&e);
}

// A blocking read with an empty ring must sleep until an event arrives,
// not spin and not return -EAGAIN.
//
// The READER is a spawned kernel thread, not the boot thread. That is
// not incidental: on a CPU still on its boot path current_thread() is
// NULL, and nothing may sleep from there -- waitq would enqueue a null
// thread. A spawned thread is both a valid context and the one that
// resembles how this call is really made.
static volatile int     blkread_state;      // 0 running, 1 pass, 2 fail
static volatile int64_t blkread_rc;
static struct evdev_client *blkread_client;

static void input_blocking_read_reader(void) {
    struct input_event ev[8];
    int64_t n = evdev_client_read(blkread_client, ev, sizeof(ev), 0);
    blkread_rc = n;
    if (n > 0 && ev[1].type == EV_KEY && ev[1].code == 30 && ev[1].value == 1) {
        blkread_state = 1;
    } else {
        blkread_state = 2;
    }
    for (;;) { __asm__ volatile("hlt"); }
}

// The driver runs on its OWN kernel thread, not on the boot path. The
// boot path cannot host this: the BSP never enters the scheduler while
// it is booting, so current_thread() is NULL there, nothing may sleep,
// no other thread can be scheduled, and with interrupts off
// timer_ticks() does not even advance. Spawning the driver defers the
// whole check to a live system, at the cost of the marker appearing
// later in the log than the other selftests.
static void input_blkread_driver(void) {
    struct evdev_client *c = evdev_client_open(&input_kbd);
    if (!c) {
        serial_write_string("[input] blocking-read selftest FAILED: open\n");
        goto park;
    }

    // Non-blocking on an empty ring is still -EAGAIN.
    struct input_event ev[8];
    if (evdev_client_read(c, ev, sizeof(ev), 1) != -EAGAIN) {
        serial_write_string("[input] blocking-read selftest FAILED: nonblock not EAGAIN\n");
        goto park;
    }

    // Grab for the duration: input_key_event suppresses the tty while a
    // grab is held, so the injected keystroke cannot land in whatever
    // console is underneath.
    evdev_client_grab(c, 1);

    blkread_state = 0;
    blkread_rc = 0;
    blkread_client = c;
    if (!thread_alloc_kernel(input_blocking_read_reader)) {
        serial_write_string("[input] blocking-read selftest SKIPPED: no thread\n");
        evdev_client_grab(c, 0);
        goto park;
    }

    // Let the reader reach the sleep, then deliver what it waits for.
    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 5 && blkread_state == 0) { __asm__ volatile("pause"); }
    input_inject_key(30, 1);           // KEY_A press

    // Bounded: a read that spun instead of sleeping would otherwise hang
    // the system, and a hang says much less than a failure.
    start = timer_ticks();
    while (blkread_state == 0 && timer_ticks() - start < 200) { __asm__ volatile("pause"); }

    if (blkread_state == 1) {
        serial_write_string("[input] blocking-read selftest passed\n");
    } else if (blkread_state == 0) {
        serial_write_string("[input] blocking-read selftest FAILED: reader never returned\n");
    } else {
        serial_write_string("[input] blocking-read selftest FAILED: rc=");
        serial_write_hex64((uint64_t)blkread_rc);
        serial_write_string("\n");
    }

    input_inject_key(30, 0);           // release, so the keystate is clean
    evdev_client_grab(c, 0);
    // The client is deliberately NOT closed: the reader thread is parked
    // in hlt and still holds the pointer.
park:
    for (;;) { __asm__ volatile("hlt"); }
}

void input_blocking_read_selftest(void) {
    if (!thread_alloc_kernel(input_blkread_driver)) {
        serial_write_string("[input] blocking-read selftest SKIPPED: no thread\n");
    }
}

// The two devices must not leak into each other. This is the property
// the device split exists for, and it is cheap to state: a keystroke
// must not appear on event1 and a mouse packet must not appear on
// event0.
void input_isolation_selftest(void) {
    struct evdev_client *k = evdev_client_open(&input_kbd);
    struct evdev_client *m = evdev_client_open(&input_mouse);
    struct input_event ev[16];
    const char *why = 0;

    if (!k || !m) { why = "open"; goto out; }

    // Grab the keyboard so the injected keystroke cannot reach the tty.
    evdev_client_grab(k, 1);
    input_inject_key(30, 1);                       // KEY_A press
    if (evdev_client_read(m, ev, sizeof(ev), 1) > 0) {
        why = "key reached the mouse"; goto out;
    }
    if (evdev_client_read(k, ev, sizeof(ev), 1) <= 0) {
        why = "key missed the keyboard"; goto out;
    }
    input_inject_key(30, 0);
    (void)evdev_client_read(k, ev, sizeof(ev), 1);

    struct mouse_packet p = { .dx = 3, .dy = -4, .dwheel = 0, .buttons = 0x01 };
    mouse_post_packet(&p);
    if (evdev_client_read(k, ev, sizeof(ev), 1) > 0) {
        why = "motion reached the keyboard"; goto out;
    }
    int64_t n = evdev_client_read(m, ev, sizeof(ev), 1);
    if (n != 4 * (int64_t)sizeof(struct input_event)) {
        why = "wrong mouse event count"; goto out;
    }
    if (ev[0].type != EV_REL || ev[0].code != REL_X    || ev[0].value != 3  ||
        ev[1].type != EV_REL || ev[1].code != REL_Y    || ev[1].value != -4 ||
        ev[2].type != EV_KEY || ev[2].code != BTN_LEFT || ev[2].value != 1  ||
        ev[3].type != EV_SYN || ev[3].code != SYN_REPORT) {
        why = "mouse event content"; goto out;
    }

    // A resting mouse says nothing at all: no axes moved, no buttons
    // changed, so there is no group to post -- not even a SYN.
    struct mouse_packet still = { .dx = 0, .dy = 0, .dwheel = 0, .buttons = 0x01 };
    mouse_post_packet(&still);
    if (evdev_client_read(m, ev, sizeof(ev), 1) > 0) {
        why = "resting mouse still reported"; goto out;
    }

out:
    if (k) { evdev_client_grab(k, 0); evdev_client_close(k); }
    if (m) { evdev_client_close(m); }
    if (why) {
        serial_write_string("[input] isolation selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
    } else {
        serial_write_string("[input] isolation selftest passed\n");
    }
}

// Selftest
void input_selftest(void) {
    struct evdev_client *c = evdev_client_open(&input_kbd);
    if (!c) {
        serial_write_string("[input] selftest FAILED: could not open client\n");
        return;
    }

    // Ungrabbed: an injected key reaches the client AND the tty
    tty_selftest_reset();
    input_inject_key(30, 1);   // KEY_A press
    input_inject_key(30, 0);   // KEY_A release

    struct input_event ev[8];
    int64_t n = evdev_client_read(c, ev, sizeof(ev), 1);
    // Expect: MSC_SCAN, KEY down, SYN, MSC_SCAN, KEY up, SYN = 6 events
    if (n != 6 * (int64_t)sizeof(struct input_event)) {
        serial_write_string("[input] selftest FAILED: ungrabbed event count\n");
        evdev_client_close(c);
        return;
    }
    if (ev[1].type != EV_KEY || ev[1].code != 30 || ev[1].value != 1) {
        serial_write_string("[input] selftest FAILED: event content\n");
        evdev_client_close(c);
        return;
    }
    if (!tty_selftest_saw('a')) {
        serial_write_string("[input] selftest FAILED: ungrabbed key missed the tty\n");
        evdev_client_close(c);
        return;
    }

    // Grabbed: the tty sees nothing
    if (evdev_client_grab(c, 1) != 0) {
        serial_write_string("[input] selftest FAILED: grab failed\n");
        evdev_client_close(c);
        return;
    }
    tty_selftest_reset();
    input_inject_key(48, 1);   // KEY_B press
    input_inject_key(48, 0);   // KEY_B release
    if (tty_selftest_saw('b')) {
        serial_write_string("[input] selftest FAILED: grabbed key leaked to the tty\n");
        evdev_client_close(c);
        return;
    }
    (void)evdev_client_read(c, ev, sizeof(ev), 1);

    // A second grab attempt fails
    struct evdev_client *c2 = evdev_client_open(&input_kbd);
    if (!c2) {
        serial_write_string("[input] selftest FAILED: could not open c2\n");
        evdev_client_close(c);
        return;
    }
    if (evdev_client_grab(c2, 1) != -EBUSY) {
        serial_write_string("[input] selftest FAILED: double grab not refused\n");
        evdev_client_close(c2);
        evdev_client_close(c);
        return;
    }

    // Release: the tty sees keys again
    evdev_client_grab(c, 0);
    tty_selftest_reset();
    input_inject_key(46, 1);   // KEY_C press
    input_inject_key(46, 0);   // KEY_C release
    if (!tty_selftest_saw('c')) {
        serial_write_string("[input] selftest FAILED: tty did not recover after ungrab\n");
        evdev_client_close(c2);
        evdev_client_close(c);
        return;
    }

    // Closing a grab holder releases the grab
    evdev_client_grab(c, 1);
    evdev_client_close(c);
    tty_selftest_reset();
    input_inject_key(30, 1);   // KEY_A press
    input_inject_key(30, 0);   // KEY_A release
    if (!tty_selftest_saw('a')) {
        serial_write_string("[input] selftest FAILED: close did not release the grab\n");
        evdev_client_close(c2);
        return;
    }

    evdev_client_close(c2);

    serial_write_string("[input] selftest passed\n");
}

struct poll_head *evdev_client_poll_head(struct evdev_client *c) {
    return c ? &c->poll : 0;
}
