#ifndef NEOOS_KEYMAP_US_H
#define NEOOS_KEYMAP_US_H

#include <stdint.h>

// Linux KEY_* constants for US keyboard
#define KEY_RESERVED          0
#define KEY_ESC              1
#define KEY_1                2
#define KEY_2                3
#define KEY_3                4
#define KEY_4                5
#define KEY_5                6
#define KEY_6                7
#define KEY_7                8
#define KEY_8                9
#define KEY_9               10
#define KEY_0               11
#define KEY_MINUS           12
#define KEY_EQUAL           13
#define KEY_BACKSPACE       14
#define KEY_TAB             15
#define KEY_Q               16
#define KEY_W               17
#define KEY_E               18
#define KEY_R               19
#define KEY_T               20
#define KEY_Y               21
#define KEY_U               22
#define KEY_I               23
#define KEY_O               24
#define KEY_P               25
#define KEY_LEFTBRACE       26
#define KEY_RIGHTBRACE      27
#define KEY_ENTER           28
#define KEY_LCTRL           29
#define KEY_A               30
#define KEY_S               31
#define KEY_D               32
#define KEY_F               33
#define KEY_G               34
#define KEY_H               35
#define KEY_J               36
#define KEY_K               37
#define KEY_L               38
#define KEY_SEMICOLON       39
#define KEY_APOSTROPHE      40
#define KEY_GRAVE           41
#define KEY_LSHIFT          42
#define KEY_BACKSLASH       43
#define KEY_Z               44
#define KEY_X               45
#define KEY_C               46
#define KEY_V               47
#define KEY_B               48
#define KEY_N               49
#define KEY_M               50
#define KEY_COMMA           51
#define KEY_DOT             52
#define KEY_SLASH           53
#define KEY_RSHIFT          54
#define KEY_KPASTERISK      55
#define KEY_LALT            56
#define KEY_SPACE           57
#define KEY_CAPSLOCK        58
#define KEY_F1              59
#define KEY_F2              60
#define KEY_F3              61
#define KEY_F4              62
#define KEY_F5              63
#define KEY_F6              64
#define KEY_F7              65
#define KEY_F8              66
#define KEY_F9              67
#define KEY_F10             68
#define KEY_NUMLOCK         69
#define KEY_SCROLLLOCK      70
#define KEY_KP7             71
#define KEY_KP8             72
#define KEY_KP9             73
#define KEY_KPMINUS         74
#define KEY_KP4             75
#define KEY_KP5             76
#define KEY_KP6             77
#define KEY_KPPLUS          78
#define KEY_KP1             79
#define KEY_KP2             80
#define KEY_KP3             81
#define KEY_KP0             82
#define KEY_KPDOT           83
#define KEY_F11            87
#define KEY_F12            88
#define KEY_KPENTER        96
#define KEY_RCTRL          97
#define KEY_KPSLASH        98
#define KEY_SYSRQ          99
#define KEY_RALT          100
#define KEY_HOME          102
#define KEY_UP            103
#define KEY_PAGEUP        104
#define KEY_LEFT          105
#define KEY_RIGHT         106
#define KEY_END           107
#define KEY_DOWN          108
#define KEY_PAGEDOWN      109
#define KEY_INSERT        110
#define KEY_DELETE        111

// Set-1 scancode to Linux KEY_* value (128 entries, base set)
// 0 = unmapped
static const uint16_t scancode_keycode[128] = {
    0,            // 0x00 - unmapped
    KEY_ESC,      // 0x01
    KEY_1,        // 0x02
    KEY_2,        // 0x03
    KEY_3,        // 0x04
    KEY_4,        // 0x05
    KEY_5,        // 0x06
    KEY_6,        // 0x07
    KEY_7,        // 0x08
    KEY_8,        // 0x09
    KEY_9,        // 0x0A
    KEY_0,        // 0x0B
    KEY_MINUS,    // 0x0C
    KEY_EQUAL,    // 0x0D
    KEY_BACKSPACE,// 0x0E
    KEY_TAB,      // 0x0F
    KEY_Q,        // 0x10
    KEY_W,        // 0x11
    KEY_E,        // 0x12
    KEY_R,        // 0x13
    KEY_T,        // 0x14
    KEY_Y,        // 0x15
    KEY_U,        // 0x16
    KEY_I,        // 0x17
    KEY_O,        // 0x18
    KEY_P,        // 0x19
    KEY_LEFTBRACE,// 0x1A
    KEY_RIGHTBRACE,// 0x1B
    KEY_ENTER,    // 0x1C
    KEY_LCTRL,    // 0x1D
    KEY_A,        // 0x1E
    KEY_S,        // 0x1F
    KEY_D,        // 0x20
    KEY_F,        // 0x21
    KEY_G,        // 0x22
    KEY_H,        // 0x23
    KEY_J,        // 0x24
    KEY_K,        // 0x25
    KEY_L,        // 0x26
    KEY_SEMICOLON,// 0x27
    KEY_APOSTROPHE,// 0x28
    KEY_GRAVE,    // 0x29
    KEY_LSHIFT,   // 0x2A
    KEY_BACKSLASH,// 0x2B
    KEY_Z,        // 0x2C
    KEY_X,        // 0x2D
    KEY_C,        // 0x2E
    KEY_V,        // 0x2F
    KEY_B,        // 0x30
    KEY_N,        // 0x31
    KEY_M,        // 0x32
    KEY_COMMA,    // 0x33
    KEY_DOT,      // 0x34
    KEY_SLASH,    // 0x35
    KEY_RSHIFT,   // 0x36
    KEY_KPASTERISK,// 0x37
    KEY_LALT,     // 0x38
    KEY_SPACE,    // 0x39
    KEY_CAPSLOCK, // 0x3A
    KEY_F1,       // 0x3B
    KEY_F2,       // 0x3C
    KEY_F3,       // 0x3D
    KEY_F4,       // 0x3E
    KEY_F5,       // 0x3F
    KEY_F6,       // 0x40
    KEY_F7,       // 0x41
    KEY_F8,       // 0x42
    KEY_F9,       // 0x43
    KEY_F10,      // 0x44
    KEY_NUMLOCK,  // 0x45
    KEY_SCROLLLOCK,// 0x46
    KEY_KP7,      // 0x47
    KEY_KP8,      // 0x48
    KEY_KP9,      // 0x49
    KEY_KPMINUS,  // 0x4A
    KEY_KP4,      // 0x4B
    KEY_KP5,      // 0x4C
    KEY_KP6,      // 0x4D
    KEY_KPPLUS,   // 0x4E
    KEY_KP1,      // 0x4F
    KEY_KP2,      // 0x50
    KEY_KP3,      // 0x51
    KEY_KP0,      // 0x52
    KEY_KPDOT,    // 0x53
    0,            // 0x54
    0,            // 0x55
    0,            // 0x56
    KEY_F11,      // 0x57
    KEY_F12,      // 0x58
    0,            // 0x59 - 0x7F unmapped
};

// Set-1 scancode to Linux KEY_* value (128 entries, after 0xE0 prefix)
// These are extended keys (arrows, keypad enter, right ctrl/alt, etc.)
static const uint16_t scancode_e0_keycode[128] = {
    0,            // 0x00
    0,            // 0x01
    0,            // 0x02
    0,            // 0x03
    0,            // 0x04
    0,            // 0x05
    0,            // 0x06
    0,            // 0x07
    0,            // 0x08
    0,            // 0x09
    0,            // 0x0A
    0,            // 0x0B
    0,            // 0x0C
    0,            // 0x0D
    0,            // 0x0E
    0,            // 0x0F
    0,            // 0x10
    0,            // 0x11
    0,            // 0x12
    0,            // 0x13
    0,            // 0x14
    0,            // 0x15
    0,            // 0x16
    0,            // 0x17
    0,            // 0x18
    0,            // 0x19
    0,            // 0x1A
    0,            // 0x1B
    KEY_KPENTER,  // 0x1C - keypad enter
    KEY_RCTRL,    // 0x1D - right ctrl
    0,            // 0x1E
    0,            // 0x1F
    0,            // 0x20
    0,            // 0x21
    0,            // 0x22
    0,            // 0x23
    0,            // 0x24
    0,            // 0x25
    0,            // 0x26
    0,            // 0x27
    0,            // 0x28
    0,            // 0x29
    0,            // 0x2A
    0,            // 0x2B
    0,            // 0x2C
    0,            // 0x2D
    0,            // 0x2E
    0,            // 0x2F
    0,            // 0x30
    0,            // 0x31
    0,            // 0x32
    0,            // 0x33
    0,            // 0x34
    KEY_KPSLASH,  // 0x35 - keypad /
    0,            // 0x36
    KEY_SYSRQ,    // 0x37 - print screen
    KEY_RALT,     // 0x38 - right alt
    0,            // 0x39
    0,            // 0x3A
    0,            // 0x3B
    0,            // 0x3C
    0,            // 0x3D
    0,            // 0x3E
    0,            // 0x3F
    0,            // 0x40
    0,            // 0x41
    0,            // 0x42
    0,            // 0x43
    0,            // 0x44
    0,            // 0x45
    0,            // 0x46
    KEY_HOME,     // 0x47
    KEY_UP,       // 0x48
    KEY_PAGEUP,   // 0x49
    0,            // 0x4A
    KEY_LEFT,     // 0x4B
    0,            // 0x4C
    KEY_RIGHT,    // 0x4D
    0,            // 0x4E
    KEY_END,      // 0x4F
    KEY_DOWN,     // 0x50
    KEY_PAGEDOWN, // 0x51
    KEY_INSERT,   // 0x52
    KEY_DELETE,   // 0x53
    0,            // 0x54
    0,            // 0x55
    0,            // 0x56
    0,            // 0x57
    0,            // 0x58
    0,            // 0x59
    0,            // 0x5A
    0,            // 0x5B
    0,            // 0x5C
    0,            // 0x5D
    0,            // 0x5E
    0,            // 0x5F
    0,            // 0x60
    0,            // 0x61
    0,            // 0x62
    0,            // 0x63
    0,            // 0x64
    0,            // 0x65
    0,            // 0x66
    0,            // 0x67
    0,            // 0x68
    0,            // 0x69
    0,            // 0x6A
    0,            // 0x6B
    0,            // 0x6C
    0,            // 0x6D
    0,            // 0x6E
    0,            // 0x6F
    0,            // 0x70
    0,            // 0x71
    0,            // 0x72
    0,            // 0x73
    0,            // 0x74
    0,            // 0x75
    0,            // 0x76
    0,            // 0x77
    0,            // 0x78
    0,            // 0x79
    0,            // 0x7A
    0,            // 0x7B
    0,            // 0x7C
    0,            // 0x7D
    0,            // 0x7E
    0,            // 0x7F
};

// KEY_* value to ASCII character (unshifted)
// Extended to 112 entries to cover all used KEY_* constants
static const char keychar[112] = {
    -1,   // 0 - unmapped
    27,   // 1 - ESC
    '1',  // 2 - KEY_1
    '2',  // 3 - KEY_2
    '3',  // 4 - KEY_3
    '4',  // 5 - KEY_4
    '5',  // 6 - KEY_5
    '6',  // 7 - KEY_6
    '7',  // 8 - KEY_7
    '8',  // 9 - KEY_8
    '9',  // 10 - KEY_9
    '0',  // 11 - KEY_0
    '-',  // 12 - KEY_MINUS
    '=',  // 13 - KEY_EQUAL
    '\b', // 14 - KEY_BACKSPACE
    '\t', // 15 - KEY_TAB
    'q',  // 16 - KEY_Q
    'w',  // 17 - KEY_W
    'e',  // 18 - KEY_E
    'r',  // 19 - KEY_R
    't',  // 20 - KEY_T
    'y',  // 21 - KEY_Y
    'u',  // 22 - KEY_U
    'i',  // 23 - KEY_I
    'o',  // 24 - KEY_O
    'p',  // 25 - KEY_P
    '[',  // 26 - KEY_LEFTBRACE
    ']',  // 27 - KEY_RIGHTBRACE
    '\n', // 28 - KEY_ENTER
    -1,   // 29 - KEY_LCTRL (modifier)
    'a',  // 30 - KEY_A
    's',  // 31 - KEY_S
    'd',  // 32 - KEY_D
    'f',  // 33 - KEY_F
    'g',  // 34 - KEY_G
    'h',  // 35 - KEY_H
    'j',  // 36 - KEY_J
    'k',  // 37 - KEY_K
    'l',  // 38 - KEY_L
    ';',  // 39 - KEY_SEMICOLON
    '\'', // 40 - KEY_APOSTROPHE
    '`',  // 41 - KEY_GRAVE
    -1,   // 42 - KEY_LSHIFT (modifier)
    '\\', // 43 - KEY_BACKSLASH
    'z',  // 44 - KEY_Z
    'x',  // 45 - KEY_X
    'c',  // 46 - KEY_C
    'v',  // 47 - KEY_V
    'b',  // 48 - KEY_B
    'n',  // 49 - KEY_N
    'm',  // 50 - KEY_M
    ',',  // 51 - KEY_COMMA
    '.',  // 52 - KEY_DOT
    '/',  // 53 - KEY_SLASH
    -1,   // 54 - KEY_RSHIFT (modifier)
    '*',  // 55 - KEY_KPASTERISK
    -1,   // 56 - KEY_LALT (modifier)
    ' ',  // 57 - KEY_SPACE
    -1,   // 58 - KEY_CAPSLOCK (modifier)
    -1,   // 59 - KEY_F1
    -1,   // 60 - KEY_F2
    -1,   // 61 - KEY_F3
    -1,   // 62 - KEY_F4
    -1,   // 63 - KEY_F5
    -1,   // 64 - KEY_F6
    -1,   // 65 - KEY_F7
    -1,   // 66 - KEY_F8
    -1,   // 67 - KEY_F9
    -1,   // 68 - KEY_F10
    -1,   // 69 - KEY_NUMLOCK
    -1,   // 70 - KEY_SCROLLLOCK
    -1,   // 71 - KEY_KP7
    -1,   // 72 - KEY_KP8
    -1,   // 73 - KEY_KP9
    -1,   // 74 - KEY_KPMINUS
    -1,   // 75 - KEY_KP4
    -1,   // 76 - KEY_KP5
    -1,   // 77 - KEY_KP6
    -1,   // 78 - KEY_KPPLUS
    -1,   // 79 - KEY_KP1
    -1,   // 80 - KEY_KP2
    -1,   // 81 - KEY_KP3
    -1,   // 82 - KEY_KP0
    -1,   // 83 - KEY_KPDOT
    -1,   // 84-86
    -1,   // 87 - KEY_F11
    -1,   // 88 - KEY_F12
    -1,   // 89-95
    -1,   // 96 - KEY_KPENTER
    -1,   // 97 - KEY_RCTRL
    -1,   // 98 - KEY_KPSLASH
    -1,   // 99 - KEY_SYSRQ
    -1,   // 100 - KEY_RALT
    -1,   // 101
    -1,   // 102 - KEY_HOME
    -1,   // 103 - KEY_UP
    -1,   // 104 - KEY_PAGEUP
    -1,   // 105 - KEY_LEFT
    -1,   // 106 - KEY_RIGHT
    -1,   // 107 - KEY_END
    -1,   // 108 - KEY_DOWN
    -1,   // 109 - KEY_PAGEDOWN
    -1,   // 110 - KEY_INSERT
    -1,   // 111 - KEY_DELETE
};

// KEY_* value to ASCII character (shifted)
// Extended to 112 entries to cover all used KEY_* constants
static const char keychar_shift[112] = {
    -1,   // 0 - unmapped
    27,   // 1 - ESC
    '!',  // 2 - KEY_1
    '@',  // 3 - KEY_2
    '#',  // 4 - KEY_3
    '$',  // 5 - KEY_4
    '%',  // 6 - KEY_5
    '^',  // 7 - KEY_6
    '&',  // 8 - KEY_7
    '*',  // 9 - KEY_8
    '(',  // 10 - KEY_9
    ')',  // 11 - KEY_0
    '_',  // 12 - KEY_MINUS
    '+',  // 13 - KEY_EQUAL
    '\b', // 14 - KEY_BACKSPACE
    '\t', // 15 - KEY_TAB
    'Q',  // 16 - KEY_Q
    'W',  // 17 - KEY_W
    'E',  // 18 - KEY_E
    'R',  // 19 - KEY_R
    'T',  // 20 - KEY_T
    'Y',  // 21 - KEY_Y
    'U',  // 22 - KEY_U
    'I',  // 23 - KEY_I
    'O',  // 24 - KEY_O
    'P',  // 25 - KEY_P
    '{',  // 26 - KEY_LEFTBRACE
    '}',  // 27 - KEY_RIGHTBRACE
    '\n', // 28 - KEY_ENTER
    -1,   // 29 - KEY_LCTRL (modifier)
    'A',  // 30 - KEY_A
    'S',  // 31 - KEY_S
    'D',  // 32 - KEY_D
    'F',  // 33 - KEY_F
    'G',  // 34 - KEY_G
    'H',  // 35 - KEY_H
    'J',  // 36 - KEY_J
    'K',  // 37 - KEY_K
    'L',  // 38 - KEY_L
    ':',  // 39 - KEY_SEMICOLON
    '"',  // 40 - KEY_APOSTROPHE
    '~',  // 41 - KEY_GRAVE
    -1,   // 42 - KEY_LSHIFT (modifier)
    '|',  // 43 - KEY_BACKSLASH
    'Z',  // 44 - KEY_Z
    'X',  // 45 - KEY_X
    'C',  // 46 - KEY_C
    'V',  // 47 - KEY_V
    'B',  // 48 - KEY_B
    'N',  // 49 - KEY_N
    'M',  // 50 - KEY_M
    '<',  // 51 - KEY_COMMA
    '>',  // 52 - KEY_DOT
    '?',  // 53 - KEY_SLASH
    -1,   // 54 - KEY_RSHIFT (modifier)
    '*',  // 55 - KEY_KPASTERISK
    -1,   // 56 - KEY_LALT (modifier)
    ' ',  // 57 - KEY_SPACE
    -1,   // 58 - KEY_CAPSLOCK (modifier)
    -1,   // 59 - KEY_F1
    -1,   // 60 - KEY_F2
    -1,   // 61 - KEY_F3
    -1,   // 62 - KEY_F4
    -1,   // 63 - KEY_F5
    -1,   // 64 - KEY_F6
    -1,   // 65 - KEY_F7
    -1,   // 66 - KEY_F8
    -1,   // 67 - KEY_F9
    -1,   // 68 - KEY_F10
    -1,   // 69 - KEY_NUMLOCK
    -1,   // 70 - KEY_SCROLLLOCK
    -1,   // 71 - KEY_KP7
    -1,   // 72 - KEY_KP8
    -1,   // 73 - KEY_KP9
    -1,   // 74 - KEY_KPMINUS
    -1,   // 75 - KEY_KP4
    -1,   // 76 - KEY_KP5
    -1,   // 77 - KEY_KP6
    -1,   // 78 - KEY_KPPLUS
    -1,   // 79 - KEY_KP1
    -1,   // 80 - KEY_KP2
    -1,   // 81 - KEY_KP3
    -1,   // 82 - KEY_KP0
    -1,   // 83 - KEY_KPDOT
    -1,   // 84-86
    -1,   // 87 - KEY_F11
    -1,   // 88 - KEY_F12
    -1,   // 89-95
    -1,   // 96 - KEY_KPENTER
    -1,   // 97 - KEY_RCTRL
    -1,   // 98 - KEY_KPSLASH
    -1,   // 99 - KEY_SYSRQ
    -1,   // 100 - KEY_RALT
    -1,   // 101
    -1,   // 102 - KEY_HOME
    -1,   // 103 - KEY_UP
    -1,   // 104 - KEY_PAGEUP
    -1,   // 105 - KEY_LEFT
    -1,   // 106 - KEY_RIGHT
    -1,   // 107 - KEY_END
    -1,   // 108 - KEY_DOWN
    -1,   // 109 - KEY_PAGEDOWN
    -1,   // 110 - KEY_INSERT
    -1,   // 111 - KEY_DELETE
};

#endif
