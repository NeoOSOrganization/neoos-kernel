#ifndef NEOOS_KVT_H
#define NEOOS_KVT_H

#include <stdint.h>

// The in-kernel virtual-terminal screen: a cell grid, a scrollback
// ring, and a deliberately small escape parser. One per /dev/ttyN.
// PURE logic -- no rendering, no locks. The full xterm engine lives in
// userland (userland/term/vt.c); this handles just what kernel log
// output and a getty-style session need.

#define VC_MAX_COLS   200
#define VC_MAX_ROWS   64
#define VC_SCROLLBACK 256
#define VC_RING       (VC_SCROLLBACK + VC_MAX_ROWS)

#define VC_BOLD    0x01
#define VC_REVERSE 0x02

struct vc_cell {
    uint8_t ch;     // 0 == blank
    uint8_t fg;     // CON_* 0..15
    uint8_t bg;     // CON_* 0..15
    uint8_t attr;   // VC_*
};

enum { KVT_GROUND = 0, KVT_ESC, KVT_CSI, KVT_ESC_SKIP };

struct kvt {
    int cols, rows;

    struct vc_cell ring[VC_RING][VC_MAX_COLS];
    int top;            // ring index of screen row 0
    int history;        // history lines available (0..VC_SCROLLBACK)
    int view;           // lines scrolled up (0 == bottom)

    int cx, cy;
    int wrap_pending;
    int cursor_visible;

    struct vc_cell pen;

    int pstate;
    int params[8];
    int nparam;
    int priv;

    int bell;
};

void kvt_init(struct kvt *v, int cols, int rows);
void kvt_feed(struct kvt *v, const char *p, unsigned n);

const struct vc_cell *kvt_cell(const struct kvt *v, int row, int col);
void kvt_cursor(const struct kvt *v, int *x, int *y, int *visible);

void kvt_scroll_view(struct kvt *v, int delta_lines);
int  kvt_view(const struct kvt *v);

void kvt_selftest(void);   // "[kvt] ..."

#endif
