#ifndef NEOOS_INPUT_H
#define NEOOS_INPUT_H

#include <stdint.h>
#include "sync/waitq.h"
#include "sync/lock.h"
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

// EV_REL and its codes (Linux values), for the mouse.
#define EV_REL   0x02
#define REL_X       0x00
#define REL_Y       0x01
#define REL_WHEEL   0x08

// Pointer buttons (Linux values).
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

// Forward declaration of opaque client struct
struct evdev_client;
struct poll_head;

// One input device: its open clients, its exclusive grab, and what it
// can report. The subsystem used to have exactly one of each of those
// as file-scope globals, which meant every event reached every client
// -- fine while the keyboard was the only device, impossible the moment
// a second one exists, because /dev/input/event0 would start emitting
// pointer events.
//
// keystate lives here rather than per-client because that is what it
// describes: EVIOCGKEY asks what the DEVICE's keys are doing, not what
// one reader has seen.
struct input_dev {
    struct spinlock lock;
    struct evdev_client *clients;
    struct evdev_client *grab;         // non-NULL if a client has exclusive grab
    const char *name;                  // EVIOCGNAME
    uint32_t ev_bits;                  // EVIOCGBIT(0): 1 << EV_*
    uint32_t rel_bits;                 // EVIOCGBIT(EV_REL): 1 << REL_*
    uint8_t  keystate[KEY_CNT / 8];    // EVIOCGKEY
};

extern struct input_dev input_kbd;
extern struct input_dev input_mouse;

// Set up a device before any client can open it.
void input_dev_init(struct input_dev *d, const char *name);

// Push n events to every client of dev as one group, then wake the
// readers and notify the pollers. The caller has already decided what
// the group is; this does not add a SYN_REPORT.
void input_post(struct input_dev *dev, const struct input_event *evs, int n);

// Initialize the input subsystem. Must be called before keyboard IRQ is unmasked.
void input_init(void);

// Called from the keyboard IRQ when a key event is decoded.
// Fans the event out to all open evdev clients and optionally to the TTY.
void input_key_event(const struct key_event *e);

// Client lifecycle functions (called by evdev file_ops)
struct evdev_client *evdev_client_open(struct input_dev *dev);
struct input_dev *evdev_client_dev(struct evdev_client *c);
void  evdev_client_close(struct evdev_client *c);
int64_t evdev_client_read(struct evdev_client *c, void *buf, uint64_t len, int nonblock);
int   evdev_client_poll(struct evdev_client *c);          // returns POLLIN or 0
// CS5.2: this client's poll head, so poll_core registers on the client
// that will actually receive the events rather than on the broadcast.
struct poll_head *evdev_client_poll_head(struct evdev_client *c);
int   evdev_client_grab(struct evdev_client *c, int on);  // on: 1 = grab, 0 = release; returns 0 or -EBUSY
void  evdev_client_key_bitmap(struct evdev_client *c, uint8_t *out, uint64_t len);
void  evdev_client_state_bitmap(struct evdev_client *c, uint8_t *out, uint64_t len);

// Test hook: inject a key event as if from the keyboard (only in NEOOS_TEST_HOOKS builds)
void input_inject_key(uint16_t keycode, int pressed);

// Selftest: tests fan-out, grab semantics, client queue management
void input_selftest(void);

// Needs a second CPU to deliver the event the sleeper waits for, so
// it runs after smp_start_aps() rather than with the others.
void input_blocking_read_selftest(void);

// Keyboard and mouse must not leak into each other. Runs after
// mouse_init, since it posts to the mouse device.
void input_isolation_selftest(void);

#endif
