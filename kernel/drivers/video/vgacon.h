#ifndef NEOOS_VGACON_H
#define NEOOS_VGACON_H

#include <stdint.h>

// 80x25 VGA text console at 0xb8000. The con_driver fallback when no
// framebuffer is present. `fg` is a CON_* index (used directly as the
// VGA attribute foreground nibble).
void vgacon_clear(void);
void vgacon_putc(char c, uint8_t fg);

#endif
