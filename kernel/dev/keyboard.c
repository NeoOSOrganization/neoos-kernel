#include "dev/keyboard.h"
#include "dev/keymap_us.h"
#include "arch/io.h"
#include "dev/serial.h"
#include "dev/vga.h"
#include "dev/tty.h"

#define KEYBOARD_DATA_PORT 0x60

// Stateful keyboard decoder
static int e0_pending = 0;
static uint32_t mods = 0;

// Temporary stub: routes to tty_input_char for backward compatibility.
// Task 10 will replace this with the real input core fan-out.
static void input_key_event(const struct key_event *e) {
    if (e->ascii >= 0) {
        tty_input_char((char)e->ascii);
    }
}

// Check if a keycode is a modifier key
static int is_modifier(uint16_t keycode) {
    return keycode == KEY_LSHIFT || keycode == KEY_RSHIFT ||
           keycode == KEY_LCTRL  || keycode == KEY_RCTRL  ||
           keycode == KEY_LALT   || keycode == KEY_RALT   ||
           keycode == KEY_CAPSLOCK || keycode == KEY_NUMLOCK;
}

// Check if a character is in the @-_ range or a letter
static int is_ctrl_char(char c) {
    return (c >= '@' && c <= '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int keyboard_decode(uint8_t byte, struct key_event *out) {
    // Handle 0xE0 prefix: next scancode is extended
    if (byte == 0xE0) {
        e0_pending = 1;
        return 0;  // Not complete yet
    }

    // Decode the scancode
    int pressed = !(byte & 0x80);  // 0x80 bit = break code
    uint8_t scancode = byte & 0x7F;
    uint16_t keycode;

    if (e0_pending) {
        keycode = scancode_e0_keycode[scancode];
        e0_pending = 0;
    } else {
        keycode = scancode_keycode[scancode];
    }

    out->keycode = keycode;
    out->pressed = pressed ? 1 : 0;
    out->raw_scan = scancode;

    // Handle modifier keys
    if (is_modifier(keycode)) {
        if (pressed) {
            // Key pressed - set the modifier bit
            if (keycode == KEY_LSHIFT) mods |= MOD_LSHIFT;
            else if (keycode == KEY_RSHIFT) mods |= MOD_RSHIFT;
            else if (keycode == KEY_LCTRL) mods |= MOD_LCTRL;
            else if (keycode == KEY_RCTRL) mods |= MOD_RCTRL;
            else if (keycode == KEY_LALT) mods |= MOD_LALT;
            else if (keycode == KEY_RALT) mods |= MOD_RALT;
            else if (keycode == KEY_CAPSLOCK) mods ^= MOD_CAPS;  // Toggle
            else if (keycode == KEY_NUMLOCK) mods ^= MOD_NUM;    // Toggle
        } else {
            // Key released - clear the modifier bit
            if (keycode == KEY_LSHIFT) mods &= ~MOD_LSHIFT;
            else if (keycode == KEY_RSHIFT) mods &= ~MOD_RSHIFT;
            else if (keycode == KEY_LCTRL) mods &= ~MOD_LCTRL;
            else if (keycode == KEY_RCTRL) mods &= ~MOD_RCTRL;
            else if (keycode == KEY_LALT) mods &= ~MOD_LALT;
            else if (keycode == KEY_RALT) mods &= ~MOD_RALT;
            // Note: CAPS and NUM are toggled, not released
        }
        out->ascii = -1;
    } else if (pressed && keycode > 0 && keycode < 112) {
        // Character key pressed - map to ASCII
        char base_char = keychar[keycode];
        if (base_char == -1) {
            out->ascii = -1;
        } else {
            // Check if this is a letter (to apply caps lock)
            int is_letter = (base_char >= 'a' && base_char <= 'z') ||
                           (base_char >= 'A' && base_char <= 'Z');

            int shift_active = (mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0;
            int caps_active = (mods & MOD_CAPS) != 0;

            if (is_letter) {
                // For letters: use shift XOR caps
                int use_shifted = shift_active ^ caps_active;
                out->ascii = use_shifted ? keychar_shift[keycode] : keychar[keycode];
            } else {
                // For non-letters: just use shift
                out->ascii = shift_active ? keychar_shift[keycode] : keychar[keycode];
            }

            // Apply Ctrl if pressed
            if ((mods & (MOD_LCTRL | MOD_RCTRL)) && out->ascii >= 0 && is_ctrl_char(out->ascii)) {
                out->ascii = out->ascii & 0x1F;
            }
        }
    } else {
        // Key released or unmapped
        out->ascii = -1;
    }

    return 1;  // Event is complete
}

void keyboard_decode_selftest(void) {
    struct key_event e;
    int rc;

    // Reset state for test
    e0_pending = 0;
    mods = 0;

    // plain 'a' make = 0x1E
    rc = keyboard_decode(0x1E, &e);
    if (!rc || e.keycode != KEY_A || !e.pressed || e.ascii != 'a') {
        serial_write_string("[keyboard] decode selftest FAILED: a make\n");
        return;
    }

    // 'a' break = 0x9E
    rc = keyboard_decode(0x9E, &e);
    if (!rc || e.keycode != KEY_A || e.pressed || e.ascii != -1) {
        serial_write_string("[keyboard] decode selftest FAILED: a break\n");
        return;
    }

    // left shift down (0x2A), then 'a' -> 'A'
    keyboard_decode(0x2A, &e);
    rc = keyboard_decode(0x1E, &e);
    if (!rc || e.ascii != 'A') {
        serial_write_string("[keyboard] decode selftest FAILED: shift+a\n");
        return;
    }
    keyboard_decode(0xAA, &e);   // shift up

    // extended: right arrow = 0xE0 0x4D
    rc = keyboard_decode(0xE0, &e);
    if (rc) {
        serial_write_string("[keyboard] decode selftest FAILED: E0 should not complete\n");
        return;
    }
    rc = keyboard_decode(0x4D, &e);
    if (!rc || e.keycode != KEY_RIGHT || e.ascii != -1) {
        serial_write_string("[keyboard] decode selftest FAILED: right arrow\n");
        return;
    }

    // ctrl+c = control char 3
    keyboard_decode(0x1D, &e);   // ctrl down
    rc = keyboard_decode(0x2E, &e);
    if (!rc || e.ascii != 3) {
        serial_write_string("[keyboard] decode selftest FAILED: ctrl+c\n");
        return;
    }
    keyboard_decode(0x9D, &e);   // ctrl up

    serial_write_string("[keyboard] decode selftest passed\n");
}

void keyboard_handler(void) {
    uint8_t sc = inb(KEYBOARD_DATA_PORT);
    struct key_event e;
    if (keyboard_decode(sc, &e) && e.keycode != 0) {
        input_key_event(&e);
    }
}
