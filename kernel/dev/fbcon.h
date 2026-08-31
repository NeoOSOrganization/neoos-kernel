#ifndef NEOOS_FBCON_H
#define NEOOS_FBCON_H

#include <stdint.h>

// A dumb character grid over the linear framebuffer: one embedded 8x16
// font, line wrap, memmove scroll. No escape sequences, no scrollback,
// one fixed colour pair -- that is M1b's userspace terminal's job. This
// is what kernel selftest output and the panic path render through when
// fb.present; dev/console.c picks it over VGA text.

void fbcon_init(void);      // compute cols/rows from fb + font, clear
void fbcon_putc(char c);
void fbcon_write(const char *s, uint64_t n);
void fbcon_clear(void);
void fbcon_selftest(void);

#endif
