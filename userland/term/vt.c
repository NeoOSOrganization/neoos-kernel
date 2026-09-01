// VT engine -- see vt.h. Task 1 scope: grid, C0 controls, scrollback
// ring, dirty tracking. The escape/CSI parser lands in later tasks;
// vt_feed currently routes ESC (0x1b) through a minimal state machine
// that is fleshed out then.

#include "vt.h"
#include <string.h>

static const struct vt_cell BLANK = { 0, 0, 0, VT_DEFFG | VT_DEFBG };

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static struct vt_cell *screen_row(struct vt *v, int r) {
    if (v->alt_active) return v->alt[r];
    return v->ring[(v->top + r) % VT_RING];
}

static void dirty_row(struct vt *v, int r) {
    if (r >= 0 && r < v->rows) v->dirty[r] = 1;
}

static void dirty_all(struct vt *v) {
    for (int r = 0; r < v->rows; r++) v->dirty[r] = 1;
}

static void row_fill(struct vt *v, struct vt_cell *row, struct vt_cell fill) {
    for (int c = 0; c < v->cols; c++) row[c] = fill;
}

// Snap the scrollback view back to the live screen (called on any
// output the child produced).
static void snap_bottom(struct vt *v) {
    if (v->view != 0) {
        v->view = 0;
        dirty_all(v);
    }
}

// Scroll rows [sr_top, sr_bot] up by one. When the region reaches the
// bottom of a non-alt screen, the row leaving the top is retained as
// history (the ring's top simply advances); otherwise the vacated row
// is blanked in place.
static void region_scroll_up(struct vt *v) {
    if (!v->alt_active && v->sr_top == 0 && v->sr_bot == v->rows - 1) {
        v->top = (v->top + 1) % VT_RING;
        if (v->history < VT_SCROLLBACK) v->history++;
        row_fill(v, screen_row(v, v->rows - 1), BLANK);
        dirty_all(v);
        return;
    }
    for (int r = v->sr_top; r < v->sr_bot; r++) {
        memcpy(screen_row(v, r), screen_row(v, r + 1),
               sizeof(struct vt_cell) * v->cols);
        dirty_row(v, r);
    }
    row_fill(v, screen_row(v, v->sr_bot), BLANK);
    dirty_row(v, v->sr_bot);
}

// Cursor down one line, scrolling the region at the bottom.
static void index_down(struct vt *v) {
    snap_bottom(v);
    if (v->cy < v->sr_bot) {
        v->cy++;
    } else if (v->cy == v->sr_bot) {
        region_scroll_up(v);
    } else if (v->cy < v->rows - 1) {
        v->cy++;
    }
}

static void put_char(struct vt *v, uint8_t ch) {
    snap_bottom(v);
    if (v->wrap_pending) {
        v->cx = 0;
        index_down(v);
        v->wrap_pending = 0;
    }
    struct vt_cell *row = screen_row(v, v->cy);
    struct vt_cell cell = v->pen;
    cell.ch = ch;
    row[v->cx] = cell;
    dirty_row(v, v->cy);
    if (v->cx + 1 >= v->cols) {
        v->cx = v->cols - 1;
        v->wrap_pending = 1;
    } else {
        v->cx++;
    }
}

void vt_init(struct vt *v, int cols, int rows) {
    memset(v, 0, sizeof(*v));
    v->cols = clampi(cols, 1, VT_MAX_COLS);
    v->rows = clampi(rows, 1, VT_MAX_ROWS);
    v->pen = BLANK;
    v->cursor_visible = 1;
    v->sr_top = 0;
    v->sr_bot = v->rows - 1;
    v->pstate = VT_GROUND;

    for (int i = 0; i < VT_RING; i++)
        for (int c = 0; c < VT_MAX_COLS; c++)
            v->ring[i][c] = BLANK;
    for (int r = 0; r < VT_MAX_ROWS; r++)
        for (int c = 0; c < VT_MAX_COLS; c++)
            v->alt[r][c] = BLANK;

    dirty_all(v);
}

// --- C0 -----------------------------------------------------------------

static void exec_c0(struct vt *v, uint8_t c) {
    switch (c) {
    case '\b':
        snap_bottom(v);
        if (v->cx > 0) v->cx--;
        v->wrap_pending = 0;
        break;
    case '\r':
        snap_bottom(v);
        v->cx = 0;
        v->wrap_pending = 0;
        break;
    case '\n': case '\v': case '\f':
        index_down(v);
        break;
    case '\t':
        snap_bottom(v);
        v->cx = clampi((v->cx & ~7) + 8, 0, v->cols - 1);
        break;
    case '\a':
        v->bell = 1;
        break;
    default:
        break;                            // other C0: ignored
    }
}

// --- feed -------------------------------------------------------------

// The escape / CSI / OSC machinery is added in Tasks 2-3. For Task 1 an
// ESC just resets to GROUND after swallowing its final byte, so stray
// sequences never corrupt the grid.
static void feed_byte(struct vt *v, uint8_t c) {
    switch (v->pstate) {
    case VT_GROUND:
        if (c == 0x1b) { v->pstate = VT_ESC; return; }
        if (c < 0x20 || c == 0x7f) { exec_c0(v, c); return; }
        put_char(v, c);
        return;
    case VT_ESC:
        v->pstate = VT_GROUND;            // Task 2 dispatches here
        return;
    default:
        v->pstate = VT_GROUND;
        return;
    }
}

void vt_feed(struct vt *v, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) feed_byte(v, p[i]);
}

// --- read-back -------------------------------------------------------

const struct vt_cell *vt_cell_at(const struct vt *v, int row, int col) {
    if (row < 0 || row >= v->rows || col < 0 || col >= v->cols) return NULL;
    if (v->alt_active) return &v->alt[row][col];
    int idx = (v->top + row - v->view) % VT_RING;
    if (idx < 0) idx += VT_RING;
    return &v->ring[idx][col];
}

void vt_cursor(const struct vt *v, int *x, int *y, int *visible) {
    if (x) *x = v->cx;
    if (y) *y = v->cy;
    if (visible) *visible = v->cursor_visible;
}

int vt_take_dirty(struct vt *v, struct vt_span *out, int max) {
    int n = 0;
    for (int r = 0; r < v->rows; r++) {
        if (!v->dirty[r]) continue;
        v->dirty[r] = 0;
        if (n < max) {
            out[n].row = r;
            out[n].col0 = 0;
            out[n].col1 = v->cols;
        }
        n++;
    }
    return n > max ? max : n;
}

void vt_scroll_view(struct vt *v, int delta_lines) {
    if (v->alt_active) return;
    int nv = clampi(v->view - delta_lines, 0, v->history);
    if (nv != v->view) {
        v->view = nv;
        dirty_all(v);
    }
}

int vt_view_offset(const struct vt *v) { return v->view; }

void vt_resize(struct vt *v, int cols, int rows) {
    v->cols = clampi(cols, 1, VT_MAX_COLS);
    v->rows = clampi(rows, 1, VT_MAX_ROWS);
    v->sr_top = 0;
    v->sr_bot = v->rows - 1;
    v->cx = clampi(v->cx, 0, v->cols - 1);
    v->cy = clampi(v->cy, 0, v->rows - 1);
    v->wrap_pending = 0;
    dirty_all(v);
}
