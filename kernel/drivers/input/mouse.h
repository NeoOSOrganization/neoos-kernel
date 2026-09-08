#ifndef NEOOS_MOUSE_H
#define NEOOS_MOUSE_H

#include <stdint.h>

// IRQ12, the i8042 auxiliary (mouse) port.
#define VECTOR_MOUSE 0x2C

// One decoded PS/2 packet, already converted to evdev's sense of the
// axes: dy is positive DOWNWARD and dwheel is positive UPWARD, which is
// the opposite of what the wire carries for both. See mouse_decode.
struct mouse_packet {
    int16_t dx, dy, dwheel;
    uint8_t buttons;        // bit 0 left, bit 1 right, bit 2 middle
};

// Feed one byte from port 0x60. Returns 1 and fills *out when a packet
// completes, 0 while mid-packet or when the byte was dropped to resync.
int  mouse_decode(uint8_t byte, struct mouse_packet *out);

// 3 for a plain PS/2 mouse, 4 once the IntelliMouse knock has succeeded
// and the device reports a scroll wheel. Resets the packet position.
void mouse_set_packet_size(int n);

// Brings up the aux port and negotiates the wheel. Returns 1 on
// success, 0 if the controller never answered -- a machine with no aux
// port must not wedge the boot.
int  mouse_init(void);

// Fan a decoded packet out to /dev/input/event1. Exposed so a selftest
// can post one without any hardware.
void mouse_post_packet(const struct mouse_packet *p);

void mouse_handler(void);
void mouse_decode_selftest(void);

#endif
