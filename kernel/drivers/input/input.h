#ifndef NEOOS_INPUT_H
#define NEOOS_INPUT_H

#include <stdint.h>
#include "sync/waitq.h"
#include "drivers/input/keyboard.h"

// Linux x86-64 input_event layout, exactly 24 bytes
struct input_event {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
_Static_assert(sizeof(struct input_event) == 24, "input_event ABI");

// Event type constants (Linux values)
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_MSC   0x04

// EV_SYN codes
#define SYN_REPORT  0

// EV_MSC codes
#define MSC_SCAN    4

// KEY_CNT: size of the keystate bitmap (covers all Linux KEY_* constants we support)
#define KEY_CNT  256

// Forward declaration of opaque client struct
struct evdev_client;

// Initialize the input subsystem. Must be called before keyboard IRQ is unmasked.
void input_init(void);

// Called from the keyboard IRQ when a key event is decoded.
// Fans the event out to all open evdev clients and optionally to the TTY.
void input_key_event(const struct key_event *e);

// Client lifecycle functions (called by evdev file_ops)
struct evdev_client *evdev_client_open(void);
void  evdev_client_close(struct evdev_client *c);
int64_t evdev_client_read(struct evdev_client *c, void *buf, uint64_t len, int nonblock);
int   evdev_client_poll(struct evdev_client *c);          // returns POLLIN or 0
int   evdev_client_grab(struct evdev_client *c, int on);  // on: 1 = grab, 0 = release; returns 0 or -EBUSY
void  evdev_client_key_bitmap(uint8_t *out, uint64_t len);
void  evdev_client_state_bitmap(uint8_t *out, uint64_t len);

// Test hook: inject a key event as if from the keyboard (only in NEOOS_TEST_HOOKS builds)
void input_inject_key(uint16_t keycode, int pressed);

// Selftest: tests fan-out, grab semantics, client queue management
void input_selftest(void);

#endif
