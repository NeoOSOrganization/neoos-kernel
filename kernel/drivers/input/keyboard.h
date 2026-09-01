#ifndef NEOOS_KEYBOARD_H
#define NEOOS_KEYBOARD_H

#include <stdint.h>

#define VECTOR_KEYBOARD 0x21

// Modifier key bits
#define MOD_LSHIFT  0x00000001
#define MOD_RSHIFT  0x00000002
#define MOD_LCTRL   0x00000004
#define MOD_RCTRL   0x00000008
#define MOD_LALT    0x00000010
#define MOD_RALT    0x00000020
#define MOD_CAPS    0x00000040
#define MOD_NUM     0x00000080

// Event returned by keyboard_decode()
struct key_event {
    uint16_t keycode;   // Linux KEY_* value, 0 if the scancode is unmapped
    uint8_t  pressed;   // 1 = make, 0 = break
    uint8_t  raw_scan;  // the Set-1 byte (low 7 bits), for EV_MSC/MSC_SCAN
    int      ascii;     // -1 if none; else the character under current modifiers
};

// Feed one byte from port 0x60; returns 1 and fills *out when a
// complete event is decoded, 0 while mid-sequence (0xE0 prefix).
int keyboard_decode(uint8_t byte, struct key_event *out);

void keyboard_handler(void);
void keyboard_decode_selftest(void);

#endif
