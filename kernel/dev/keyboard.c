#include "dev/keyboard.h"
#include "arch/io.h"
#include "dev/serial.h"
#include "dev/vga.h"
#include "dev/tty.h"

#define KEYBOARD_DATA_PORT 0x60

// Scancode Set 1, basic US layout, make codes only (no shift state,
// no extended 0xE0 prefix handling) — entries left at 0 are unmapped.
static const char scancode_to_ascii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'','`', 0,  '\\','z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0,  '*', 0,   ' ',
};

void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode & 0x80) {
        return; // key release, ignore
    }

    char c = scancode_to_ascii[scancode];
    if (c != 0) {
        // Handed to the TERMINAL, not echoed here. Echo is a property
        // of the line discipline (the ECHO bit in c_lflag), and a
        // driver that printed the character itself would echo even
        // with echo turned off -- which is what a password prompt
        // turns off.
        tty_input_char(c);
    }
}
