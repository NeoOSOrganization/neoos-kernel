#include "dev/input.h"
#include "dev/keyboard.h"
#include "dev/tty.h"
#include "dev/timer.h"
#include "dev/rtc.h"
#include "dev/serial.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "mm/heap.h"
#include "sched/proc.h"
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
    struct input_event ring[256];      // Ring buffer, power of 2 size
    uint32_t head;                     // Next write position
    uint32_t tail;                     // Oldest unread event
    int dropped;                       // Count of dropped events (overflow)
    struct waitq readers;              // Waiters blocked on this client's queue
    uint8_t keystate[KEY_CNT / 8];    // Bitmap of key states
    int refcount;                      // Number of file descriptors referencing this
};

// Global input subsystem state
static struct {
    struct spinlock lock;
    struct evdev_client *clients;
    struct evdev_client *grab;         // Non-NULL if a client has exclusive grab
} input;

// Initialize the input subsystem
void input_init(void) {
    spin_init(&input.lock, LOCK_RANK_INPUT, "input");
    input.clients = NULL;
    input.grab = NULL;
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

// Process a key event: fan out to clients and optionally to TTY
void input_key_event(const struct key_event *e) {
    if (!e || e->keycode == 0) {
        return;  // Unmapped scancode, ignore
    }

    int64_t should_call_tty = 0;
    int was_grabbed = 0;

    // Phase 1: Take the lock, append events to all clients, snapshot grab state
    uint64_t flags = spin_lock_irqsave(&input.lock);

    // Check if a grab is in effect
    was_grabbed = (input.grab != NULL);

    int64_t tv_sec, tv_usec;
    get_timestamp(&tv_sec, &tv_usec);

    // For each client, push three events: MSC_SCAN, KEY (or autorepeat), SYN_REPORT
    for (struct evdev_client *c = input.clients; c; c = c->next) {
        struct input_event ev;

        // EV_MSC/MSC_SCAN event with raw scancode
        ev.tv_sec = tv_sec;
        ev.tv_usec = tv_usec;
        ev.type = EV_MSC;
        ev.code = MSC_SCAN;
        ev.value = e->raw_scan;
        push_event(c, &ev);

        // EV_KEY event with keycode and press/release state
        ev.type = EV_KEY;
        ev.code = e->keycode;
        ev.value = e->pressed ? 1 : 0;
        push_event(c, &ev);

        // EV_SYN/SYN_REPORT to mark end of this logical event
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        push_event(c, &ev);

        // Update keystate bitmap
        uint32_t byte_idx = e->keycode / 8;
        uint32_t bit_idx = e->keycode % 8;
        if (byte_idx < (KEY_CNT / 8)) {
            if (e->pressed) {
                c->keystate[byte_idx] |= (1u << bit_idx);
            } else {
                c->keystate[byte_idx] &= ~(1u << bit_idx);
            }
        }
    }

    // Decide whether to call tty_input_char
    // Only if: no grab is active AND ascii is valid
    if (!was_grabbed && e->ascii >= 0) {
        should_call_tty = 1;
    }

    spin_unlock_irqrestore(&input.lock, flags);

    // Phase 2: Wake all clients' reader waitqueues (after releasing the lock)
    for (struct evdev_client *c = input.clients; c; c = c->next) {
        waitq_wake_all(&c->readers);
    }

    // Phase 3: If no grab and ascii is valid, deliver to TTY
    if (should_call_tty) {
        tty_input_char(tty_console(), (char)e->ascii);
    }
}

// Open a new evdev client
struct evdev_client *evdev_client_open(void) {
    struct evdev_client *c = kmalloc(sizeof(struct evdev_client));
    if (!c) {
        return NULL;
    }

    memset_local(c, 0, sizeof(*c));
    c->refcount = 1;
    waitq_init(&c->readers);

    uint64_t flags = spin_lock_irqsave(&input.lock);
    c->next = input.clients;
    input.clients = c;
    spin_unlock_irqrestore(&input.lock, flags);

    return c;
}

// Close an evdev client
void evdev_client_close(struct evdev_client *c) {
    if (!c) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&input.lock);

    // If this client holds the grab, release it
    if (input.grab == c) {
        input.grab = NULL;
    }

    // Unlink from the clients list
    struct evdev_client **prev = &input.clients;
    for (struct evdev_client *client = input.clients; client; client = client->next) {
        if (client == c) {
            *prev = client->next;
            break;
        }
        prev = &client->next;
    }

    spin_unlock_irqrestore(&input.lock, flags);

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

    uint64_t flags = spin_lock_irqsave(&input.lock);

    // Copy all available events, up to max_events
    uint32_t events_copied = 0;
    while (events_copied < max_events && !ring_empty(c)) {
        uint32_t idx = c->tail & 0xFF;
        out[events_copied] = c->ring[idx];
        c->tail++;
        events_copied++;
    }

    spin_unlock_irqrestore(&input.lock, flags);

    // If we got any events, return them
    if (events_copied > 0) {
        return events_copied * (int64_t)sizeof(struct input_event);
    }

    // No events and blocking? Return EAGAIN (non-blocking behavior)
    // A real implementation would wait on waitq, but that requires
    // better lock ordering which we address by just returning EAGAIN
    return nonblock ? -EAGAIN : -EAGAIN;
}

// Poll for readiness
int evdev_client_poll(struct evdev_client *c) {
    if (!c) {
        return 0;
    }

    uint64_t flags = spin_lock_irqsave(&input.lock);
    int result = 0;

    // Return POLLIN if the ring has events
    if (!ring_empty(c)) {
        result = 1;  // POLLIN = 0x001
    }

    spin_unlock_irqrestore(&input.lock, flags);
    return result;
}

// Grab or release exclusive input
int evdev_client_grab(struct evdev_client *c, int on) {
    if (!c) {
        return -EINVAL;
    }

    uint64_t flags = spin_lock_irqsave(&input.lock);

    if (on) {
        // Try to acquire grab
        if (input.grab != NULL && input.grab != c) {
            spin_unlock_irqrestore(&input.lock, flags);
            return -EBUSY;
        }
        input.grab = c;
    } else {
        // Release grab
        if (input.grab == c) {
            input.grab = NULL;
        }
    }

    spin_unlock_irqrestore(&input.lock, flags);
    return 0;
}

// Get the bitmap of keys currently pressed (for EVIOCGKEY)
void evdev_client_key_bitmap(uint8_t *out, uint64_t len) {
    if (!out || len == 0) {
        return;
    }

    // Merge the keystate from all clients
    memset_local(out, 0, len);

    uint64_t flags = spin_lock_irqsave(&input.lock);

    for (struct evdev_client *c = input.clients; c; c = c->next) {
        for (uint64_t i = 0; i < len && i < (KEY_CNT / 8); i++) {
            out[i] |= c->keystate[i];
        }
    }

    spin_unlock_irqrestore(&input.lock, flags);
}

// Get the current state bitmap of all keys
void evdev_client_state_bitmap(uint8_t *out, uint64_t len) {
    // For now, same as key_bitmap (all clients see the same key state)
    evdev_client_key_bitmap(out, len);
}

// Test hook: inject a key event
void input_inject_key(uint16_t keycode, int pressed) {
    struct key_event e;
    e.keycode = keycode;
    e.pressed = pressed ? 1 : 0;
    e.raw_scan = 0;  // No raw scancode for injected keys

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

// Selftest
void input_selftest(void) {
    struct evdev_client *c = evdev_client_open();
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
    struct evdev_client *c2 = evdev_client_open();
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
