#include "tty/console.h"
#include "tty/con_driver.h"
#include "tty/vt.h"

// Kernel screen output goes to the active virtual terminal (which
// renders through the con_driver). fb_owned is the M1b-3 escape hatch:
// while a userland terminal owns the pixels, the kernel does not paint
// (serial output is written by the callers themselves). M1c-4 replaces
// it with the per-VT KD_GRAPHICS mode.
static int fb_owned;

void console_set_fb_owned(int on) { fb_owned = on ? 1 : 0; }
int  console_fb_owned(void)       { return fb_owned; }

void console_putc(char c) {
    if (fb_owned) { return; }
    vt_write_active(&c, 1);
}

void console_write(const char *s, uint64_t n) {
    if (fb_owned) { return; }
    vt_write_active(s, (unsigned)n);
}

void console_clear(void) {
    if (fb_owned) { return; }
    struct con_driver *cd = con_driver_active();
    if (cd && cd->clear) { cd->clear(); }
}
