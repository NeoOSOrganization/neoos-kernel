#ifndef NEOOS_CONSOLE_H
#define NEOOS_CONSOLE_H

#include <stdint.h>

// The kernel's screen output, one indirection above the hardware:
// framebuffer (fbcon) when GRUB gave us one, VGA text mode otherwise.
// Serial is a separate, always-on mirror -- callers that want both
// (tty output, panic) write serial themselves and then call these.

void console_write(const char *s, uint64_t n);
// While set, a userland terminal owns the framebuffer: console_putc /
// console_clear stop touching pixels (serial output is written by the
// callers themselves and is unaffected). Set/cleared by NEOOS_TIOCSACTIVE
// on a pty master; forcibly cleared by the exception path.
void console_set_fb_owned(int on);
#endif
