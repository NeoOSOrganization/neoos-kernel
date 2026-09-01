#ifndef NEOOS_TERM_VT_H
#define NEOOS_TERM_VT_H

// A self-contained xterm-ish terminal emulator: cell grid, escape/CSI
// parser, SGR attributes, scroll regions, alt screen, scrollback ring.
// PURE logic -- no framebuffer, no syscalls, no I/O. vt_feed() is a
// function of the bytes fed in; the renderer reads back through
// vt_cell_at() / vt_cursor() / vt_take_dirty().

#include <stdint.h>
#include <stddef.h>

#define VT_MAX_COLS   200
#define VT_MAX_ROWS   64
#define VT_SCROLLBACK 1000                    // history lines above the screen
#define VT_RING       (VT_SCROLLBACK + VT_MAX_ROWS)

// cell attribute bits
#define VT_BOLD      0x01
#define VT_DIM       0x02
#define VT_UNDERLINE 0x04
#define VT_REVERSE   0x08
#define VT_HIDDEN    0x10
#define VT_DEFFG     0x20                     // fg is the terminal default
#define VT_DEFBG     0x40                     // bg is the terminal default

struct vt_cell {
    uint8_t ch;                              // Latin-1; 0 == blank
    uint8_t fg;                              // 0..255 xterm index, ignored if VT_DEFFG
    uint8_t bg;                              // 0..255 xterm index, ignored if VT_DEFBG
    uint8_t attr;
};

struct vt_span { int row; int col0; int col1; };   // half-open [col0, col1)

// parser states
enum { VT_GROUND = 0, VT_ESC, VT_ESC_CHARSET, VT_CSI, VT_OSC };

#define VT_MAXPARAM 16

struct vt {
    int cols, rows;

    // Scrollback ring. Screen row r lives at ring[(top + r) % VT_RING].
    struct vt_cell ring[VT_RING][VT_MAX_COLS];
    int top;                                 // ring index of screen row 0
    int history;                             // history lines available (0..VT_SCROLLBACK)
    int view;                                // lines scrolled up into history (0 == bottom)

    // Alt screen (separate grid, no history).
    struct vt_cell alt[VT_MAX_ROWS][VT_MAX_COLS];
    int alt_active;

    // Cursor + pending-wrap latch.
    int cx, cy;
    int wrap_pending;
    int cursor_visible;

    // Current SGR, carried as a template cell (ch unused).
    struct vt_cell pen;

    // Saved cursor (DECSC / CSI s) and the alt-swap save.
    int   sc_cx, sc_cy; struct vt_cell sc_pen; int sc_valid;
    int   alt_cx, alt_cy; struct vt_cell alt_pen;

    // Scroll region (0-based, inclusive).
    int sr_top, sr_bot;

    // Parser.
    int pstate;
    int params[VT_MAXPARAM];
    int nparam;
    int priv;                                // CSI '?' seen
    int inter;                               // last intermediate byte (0 if none)

    int bell;                                // set by BEL; renderer clears

    uint8_t dirty[VT_MAX_ROWS];
};

void vt_init(struct vt *v, int cols, int rows);
void vt_feed(struct vt *v, const uint8_t *p, size_t n);
void vt_resize(struct vt *v, int cols, int rows);

const struct vt_cell *vt_cell_at(const struct vt *v, int row, int col);
void vt_cursor(const struct vt *v, int *x, int *y, int *visible);

int  vt_take_dirty(struct vt *v, struct vt_span *out, int max);

void vt_scroll_view(struct vt *v, int delta_lines);
int  vt_view_offset(const struct vt *v);

#endif
