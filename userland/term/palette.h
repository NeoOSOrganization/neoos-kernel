#ifndef NEOOS_TERM_PALETTE_H
#define NEOOS_TERM_PALETTE_H

#include <stdint.h>

// The xterm 256-colour palette as 0x00RRGGBB. Filled by palette_init()
// (call once before rendering): 0..15 the ANSI + bright set, 16..231
// the 6x6x6 cube, 232..255 the grayscale ramp.
extern uint32_t vt_palette[256];
void palette_init(void);

#define VT_DEFAULT_FG 0x00c8c8c8u
#define VT_DEFAULT_BG 0x00000000u

#endif
