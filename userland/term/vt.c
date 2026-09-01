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

static void region_scroll_down(struct vt *v) {
    snap_bottom(v);
    for (int r = v->sr_bot; r > v->sr_top; r--) {
        memcpy(screen_row(v, r), screen_row(v, r - 1),
               sizeof(struct vt_cell) * v->cols);
        dirty_row(v, r);
    }
    row_fill(v, screen_row(v, v->sr_top), BLANK);
    dirty_row(v, v->sr_top);
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

static void index_up(struct vt *v) {
    snap_bottom(v);
    if (v->cy > v->sr_top) {
        v->cy--;
    } else if (v->cy == v->sr_top) {
        region_scroll_down(v);
    } else if (v->cy > 0) {
        v->cy--;
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

static void enter_alt(struct vt *v) {
    if (v->alt_active) return;
    v->alt_cx = v->cx;
    v->alt_cy = v->cy;
    v->alt_pen = v->pen;
    for (int r = 0; r < VT_MAX_ROWS; r++)
        for (int c = 0; c < VT_MAX_COLS; c++)
            v->alt[r][c] = BLANK;
    v->alt_active = 1;
    v->view = 0;
    v->cx = v->cy = 0;
    v->wrap_pending = 0;
    dirty_all(v);
}

static void leave_alt(struct vt *v) {
    if (!v->alt_active) return;
    v->alt_active = 0;
    v->cx = clampi(v->alt_cx, 0, v->cols - 1);
    v->cy = clampi(v->alt_cy, 0, v->rows - 1);
    v->pen = v->alt_pen;
    v->wrap_pending = 0;
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

// --- erase / line ops -----------------------------------------------

static void erase_cells(struct vt *v, int row, int c0, int c1) {
    struct vt_cell *r = screen_row(v, row);
    for (int c = c0; c < c1 && c < v->cols; c++) r[c] = BLANK;
    dirty_row(v, row);
}

static void erase_display(struct vt *v, int mode) {
    if (mode == 0) {                          // cursor -> end of screen
        erase_cells(v, v->cy, v->cx, v->cols);
        for (int r = v->cy + 1; r < v->rows; r++) erase_cells(v, r, 0, v->cols);
    } else if (mode == 1) {                   // start of screen -> cursor
        for (int r = 0; r < v->cy; r++) erase_cells(v, r, 0, v->cols);
        erase_cells(v, v->cy, 0, v->cx + 1);
    } else {                                  // 2 / 3: whole screen
        for (int r = 0; r < v->rows; r++) erase_cells(v, r, 0, v->cols);
    }
}

static void erase_line(struct vt *v, int mode) {
    if (mode == 0)      erase_cells(v, v->cy, v->cx, v->cols);
    else if (mode == 1) erase_cells(v, v->cy, 0, v->cx + 1);
    else                erase_cells(v, v->cy, 0, v->cols);
}

// Insert/delete `n` blank lines at the cursor row, confined to the
// scroll region (only when the cursor is inside it).
static void insert_lines(struct vt *v, int n) {
    if (v->cy < v->sr_top || v->cy > v->sr_bot) return;
    if (n > v->sr_bot - v->cy + 1) n = v->sr_bot - v->cy + 1;
    for (int r = v->sr_bot; r >= v->cy + n; r--) {
        memcpy(screen_row(v, r), screen_row(v, r - n),
               sizeof(struct vt_cell) * v->cols);
        dirty_row(v, r);
    }
    for (int r = v->cy; r < v->cy + n; r++) {
        row_fill(v, screen_row(v, r), BLANK);
        dirty_row(v, r);
    }
}

static void delete_lines(struct vt *v, int n) {
    if (v->cy < v->sr_top || v->cy > v->sr_bot) return;
    if (n > v->sr_bot - v->cy + 1) n = v->sr_bot - v->cy + 1;
    for (int r = v->cy; r + n <= v->sr_bot; r++) {
        memcpy(screen_row(v, r), screen_row(v, r + n),
               sizeof(struct vt_cell) * v->cols);
        dirty_row(v, r);
    }
    for (int r = v->sr_bot - n + 1; r <= v->sr_bot; r++) {
        row_fill(v, screen_row(v, r), BLANK);
        dirty_row(v, r);
    }
}

// --- SGR -----------------------------------------------------------

static uint8_t rgb_to_256(int r, int g, int b) {
    if (r == g && g == b) {                   // grayscale ramp 232..255
        if (r < 8)  return 16;
        if (r > 238) return 231;
        return (uint8_t)(232 + (r - 8) / 10);
    }
    int r6 = (r * 5 + 127) / 255;
    int g6 = (g * 5 + 127) / 255;
    int b6 = (b * 5 + 127) / 255;
    return (uint8_t)(16 + 36 * r6 + 6 * g6 + b6);
}

static void apply_sgr(struct vt *v) {
    int n = v->nparam;
    for (int i = 0; i < n; i++) {
        int p = v->params[i];
        if (p == 0) {
            v->pen = BLANK;                    // full reset
        } else if (p == 1) v->pen.attr |= VT_BOLD;
        else if (p == 2)  v->pen.attr |= VT_DIM;
        else if (p == 4)  v->pen.attr |= VT_UNDERLINE;
        else if (p == 7)  v->pen.attr |= VT_REVERSE;
        else if (p == 8)  v->pen.attr |= VT_HIDDEN;
        else if (p == 22) v->pen.attr &= ~(VT_BOLD | VT_DIM);
        else if (p == 24) v->pen.attr &= ~VT_UNDERLINE;
        else if (p == 27) v->pen.attr &= ~VT_REVERSE;
        else if (p == 28) v->pen.attr &= ~VT_HIDDEN;
        else if (p >= 30 && p <= 37) { v->pen.fg = (uint8_t)(p - 30); v->pen.attr &= ~VT_DEFFG; }
        else if (p == 39) v->pen.attr |= VT_DEFFG;
        else if (p >= 40 && p <= 47) { v->pen.bg = (uint8_t)(p - 40); v->pen.attr &= ~VT_DEFBG; }
        else if (p == 49) v->pen.attr |= VT_DEFBG;
        else if (p >= 90 && p <= 97) { v->pen.fg = (uint8_t)(p - 90 + 8); v->pen.attr &= ~VT_DEFFG; }
        else if (p >= 100 && p <= 107) { v->pen.bg = (uint8_t)(p - 100 + 8); v->pen.attr &= ~VT_DEFBG; }
        else if (p == 38 || p == 48) {
            int deffg = (p == 38);
            if (i + 1 < n && v->params[i + 1] == 5 && i + 2 < n) {
                uint8_t idx = (uint8_t)v->params[i + 2];
                if (deffg) { v->pen.fg = idx; v->pen.attr &= ~VT_DEFFG; }
                else       { v->pen.bg = idx; v->pen.attr &= ~VT_DEFBG; }
                i += 2;
            } else if (i + 1 < n && v->params[i + 1] == 2 && i + 4 < n) {
                uint8_t idx = rgb_to_256(v->params[i + 2], v->params[i + 3], v->params[i + 4]);
                if (deffg) { v->pen.fg = idx; v->pen.attr &= ~VT_DEFFG; }
                else       { v->pen.bg = idx; v->pen.attr &= ~VT_DEFBG; }
                i += 4;
            }
        }
        // unknown SGR codes: ignored
    }
}

// --- CSI / ESC dispatch -------------------------------------------

static int clamp_row(struct vt *v, int r) { return clampi(r, 0, v->rows - 1); }
static int clamp_col(struct vt *v, int c) { return clampi(c, 0, v->cols - 1); }

// param i with default def when absent or zero
static int P(struct vt *v, int i, int def) {
    if (i < v->nparam && v->params[i] != 0) return v->params[i];
    return def;
}

static void dispatch_csi(struct vt *v, uint8_t final) {
    v->wrap_pending = 0;

    if (v->priv) {
        // private modes: DECTCEM, alt screen, and a set of accept-noops.
        int set = (final == 'h');
        for (int i = 0; i < v->nparam; i++) {
            switch (v->params[i]) {
            case 25:  v->cursor_visible = set; break;
            case 1049: case 1047: case 47:
                if (set) enter_alt(v); else leave_alt(v);
                break;
            default:  /* 2004, 1000, 1002, 1006, 1015, 7, ... : ignore */ break;
            }
        }
        return;
    }

    switch (final) {
    case 'A': v->cy = clamp_row(v, v->cy - P(v, 0, 1)); break;
    case 'B': v->cy = clamp_row(v, v->cy + P(v, 0, 1)); break;
    case 'C': v->cx = clamp_col(v, v->cx + P(v, 0, 1)); break;
    case 'D': v->cx = clamp_col(v, v->cx - P(v, 0, 1)); break;
    case 'E': v->cx = 0; v->cy = clamp_row(v, v->cy + P(v, 0, 1)); break;
    case 'F': v->cx = 0; v->cy = clamp_row(v, v->cy - P(v, 0, 1)); break;
    case 'G': case '`': v->cx = clamp_col(v, P(v, 0, 1) - 1); break;
    case 'd': v->cy = clamp_row(v, P(v, 0, 1) - 1); break;
    case 'H': case 'f':
        v->cy = clamp_row(v, P(v, 0, 1) - 1);
        v->cx = clamp_col(v, P(v, 1, 1) - 1);
        break;
    case 'J': erase_display(v, P(v, 0, 0)); break;
    case 'K': erase_line(v, P(v, 0, 0)); break;
    case 'L': insert_lines(v, P(v, 0, 1)); break;
    case 'M': delete_lines(v, P(v, 0, 1)); break;
    case 'S': for (int i = P(v, 0, 1); i > 0; i--) region_scroll_up(v);   break;
    case 'T': for (int i = P(v, 0, 1); i > 0; i--) region_scroll_down(v); break;
    case 'r': {
        int t = P(v, 0, 1) - 1;
        int b = (v->nparam >= 2 && v->params[1] != 0) ? v->params[1] - 1 : v->rows - 1;
        if (t < 0) t = 0;
        if (b > v->rows - 1) b = v->rows - 1;
        if (t < b) { v->sr_top = t; v->sr_bot = b; v->cx = 0; v->cy = t; }
        break;
    }
    case 's': v->sc_cx = v->cx; v->sc_cy = v->cy; v->sc_pen = v->pen; v->sc_valid = 1; break;
    case 'u':
        if (v->sc_valid) { v->cx = clamp_col(v, v->sc_cx); v->cy = clamp_row(v, v->sc_cy); v->pen = v->sc_pen; }
        break;
    case 'm': apply_sgr(v); break;
    case 'n': /* DSR: no reply channel inside vt */ break;
    default:  break;
    }
}

static void dispatch_esc(struct vt *v, uint8_t c) {
    switch (c) {
    case '7': v->sc_cx = v->cx; v->sc_cy = v->cy; v->sc_pen = v->pen; v->sc_valid = 1; break;
    case '8':
        if (v->sc_valid) { v->cx = clamp_col(v, v->sc_cx); v->cy = clamp_row(v, v->sc_cy); v->pen = v->sc_pen; }
        break;
    case 'c': vt_init(v, v->cols, v->rows); break; // RIS full reset
    case 'M': index_up(v); break;                 // reverse index
    case 'D': index_down(v); break;
    case 'E': v->cx = 0; index_down(v); break;
    default:  break;
    }
}

// --- feed -------------------------------------------------------------

static void csi_reset(struct vt *v) {
    v->nparam = 1;
    for (int i = 0; i < VT_MAXPARAM; i++) v->params[i] = 0;
    v->priv = 0;
    v->inter = 0;
}

static void feed_byte(struct vt *v, uint8_t c) {
    switch (v->pstate) {
    case VT_GROUND:
        if (c == 0x1b) { v->pstate = VT_ESC; return; }
        if (c < 0x20 || c == 0x7f) { exec_c0(v, c); return; }
        put_char(v, c);
        return;

    case VT_ESC:
        if (c == '[') { csi_reset(v); v->pstate = VT_CSI; return; }
        if (c == ']') { v->pstate = VT_OSC; return; }
        if (c == '(' || c == ')' || c == '*' || c == '+') { v->pstate = VT_ESC_CHARSET; return; }
        dispatch_esc(v, c);
        v->pstate = VT_GROUND;
        return;

    case VT_ESC_CHARSET:
        v->pstate = VT_GROUND;                    // consume the designator, ignore
        return;

    case VT_OSC:
        // Consume the payload until BEL or ST (ESC \). Discarded --
        // vt has no title/clipboard surface.
        if (c == 0x07) { v->pstate = VT_GROUND; return; }
        if (c == 0x1b) { v->pstate = VT_OSC_ESC; return; }
        return;

    case VT_OSC_ESC:
        // saw ESC inside OSC: '\' ends the string (ST); anything else
        // aborts the OSC and is reprocessed as a fresh ESC sequence.
        if (c == '\\') { v->pstate = VT_GROUND; return; }
        v->pstate = VT_ESC;
        feed_byte(v, c);
        return;

    case VT_CSI:
        if (c >= '0' && c <= '9') {
            v->params[v->nparam - 1] = v->params[v->nparam - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (v->nparam < VT_MAXPARAM) v->nparam++;
            return;
        }
        if (c == '?') { v->priv = 1; return; }
        if (c >= 0x20 && c <= 0x2f) { v->inter = c; return; }
        if (c < 0x20) { exec_c0(v, c); return; } // C0 mid-sequence: execute, stay
        if (c >= 0x40 && c <= 0x7e) {
            dispatch_csi(v, c);
            v->pstate = VT_GROUND;
            return;
        }
        v->pstate = VT_GROUND;
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
    int oc = v->cols, orr = v->rows;
    int nc = clampi(cols, 1, VT_MAX_COLS);
    int nr = clampi(rows, 1, VT_MAX_ROWS);

    // Blank the cells newly exposed by a grow, in both grids, so a
    // wider/taller screen does not surface stale ring or alt content.
    if (nc > oc || nr > orr) {
        for (int r = 0; r < nr; r++) {
            struct vt_cell *sr = v->ring[(v->top + r) % VT_RING];
            struct vt_cell *ar = v->alt[r];
            int c0 = (r < orr) ? oc : 0;      // fully new rows blank from col 0
            for (int c = c0; c < nc; c++) { sr[c] = BLANK; ar[c] = BLANK; }
        }
    }

    v->cols = nc;
    v->rows = nr;
    v->sr_top = 0;
    v->sr_bot = v->rows - 1;
    v->cx = clampi(v->cx, 0, v->cols - 1);
    v->cy = clampi(v->cy, 0, v->rows - 1);
    v->wrap_pending = 0;
    if (v->view > v->history) v->view = v->history;
    dirty_all(v);
}
