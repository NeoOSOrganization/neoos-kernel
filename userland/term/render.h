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
// scrollback view is at the bottom, REPAINTING the cell it last
// occupied.
//
// The repaint is not optional. The cursor is drawn over a cell rather
// than being part of the grid, so when it moves, the cell it left keeps
// the inverted block unless something else happens to redraw it. A
// newline moves the cursor to a cell nothing writes to, which left a
// white block at the end of every line the user pressed Enter on.
void render_cursor(const struct term_fb *fb, const struct vt *v);

// Paint a glyph / a string at a cell position with explicit colours,
// bypassing the VT grid. Used for the logo header, which is not
// terminal content: it must not scroll, must not enter the scrollback,
// and must survive the shell clearing the screen.
void render_glyph_at(const struct term_fb *fb, int row, int col,
                     unsigned char ch, uint32_t fg_rgb, uint32_t bg_rgb);
void render_text_at(const struct term_fb *fb, int row, int col,
                    const char *s, uint32_t fg_rgb, uint32_t bg_rgb);

#endif
