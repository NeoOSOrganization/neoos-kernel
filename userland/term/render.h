#ifndef NEOOS_TERM_RENDER_H
#define NEOOS_TERM_RENDER_H

#include <stdint.h>
#include "vt.h"

// The framebuffer the terminal paints into. `pix` is the mapped base;
// `ro/go/bo` are the channel shift positions (from FBIOGET_VSCREENINFO).
struct term_fb {
    volatile uint8_t *pix;
    int pitch;                  // bytes per scanline
    int w, h;                   // pixels
    int ro, go, bo;             // red / green / blue shift
};

// Fill the whole framebuffer with one colour (0x00RRGGBB).
void render_clear(const struct term_fb *fb, uint32_t rgb);

// Blit cells [col0, col1) of screen row `row` from the VT grid.
void render_span(const struct term_fb *fb, const struct vt *v,
                 int row, int col0, int col1);

// Draw the cursor (inverse block) at its cell, if visible and the
// scrollback view is at the bottom.
void render_cursor(const struct term_fb *fb, const struct vt *v);

#endif
