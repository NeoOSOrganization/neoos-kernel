#include "tty/console.h"
#include "drivers/video/fb.h"
#include "drivers/video/fbcon.h"
#include "drivers/video/vga.h"

static int fb_owned;

void console_set_fb_owned(int on) { fb_owned = on ? 1 : 0; }
int  console_fb_owned(void)       { return fb_owned; }

void console_putc(char c) {
    if (fb_owned) { return; }               // userland owns the pixels
    if (fb.present) { fbcon_putc(c); }
    else            { vga_putc(c); }
}

void console_write(const char *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { console_putc(s[i]); }
}

void console_clear(void) {
    if (fb_owned) { return; }
    if (fb.present) { fbcon_clear(); }
    else            { vga_clear(); }
}
