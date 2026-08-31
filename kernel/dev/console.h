#ifndef NEOOS_CONSOLE_H
#define NEOOS_CONSOLE_H

#include <stdint.h>

// The kernel's screen output, one indirection above the hardware:
// framebuffer (fbcon) when GRUB gave us one, VGA text mode otherwise.
// Serial is a separate, always-on mirror -- callers that want both
// (tty output, panic) write serial themselves and then call these.

void console_putc(char c);
void console_write(const char *s, uint64_t n);
void console_clear(void);

#endif
