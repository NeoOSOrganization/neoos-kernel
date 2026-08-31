#include "dev/console.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "dev/vga.h"

void console_putc(char c) {
    if (fb.present) { fbcon_putc(c); }
    else            { vga_putc(c); }
}

void console_write(const char *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { console_putc(s[i]); }
}

void console_clear(void) {
    if (fb.present) { fbcon_clear(); }
    else            { vga_clear(); }
}
