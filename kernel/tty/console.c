#include "tty/console.h"
#include "tty/con_driver.h"

static int fb_owned;

void console_set_fb_owned(int on) { fb_owned = on ? 1 : 0; }
int  console_fb_owned(void)       { return fb_owned; }

void console_putc(char c) {
    if (fb_owned || !con_driver_active()) { return; }
    con_driver_active()->putc_attr(c, CON_GREY);
}

void console_write(const char *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { console_putc(s[i]); }
}

void console_clear(void) {
    if (fb_owned || !con_driver_active()) { return; }
    con_driver_active()->clear();
}
