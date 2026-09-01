#include "tty/kvt.h"
#include "tty/con_driver.h"      // CON_* colour indices
#include "drivers/char/serial.h"

static const struct vc_cell BLANK = { 0, CON_GREY, CON_BLACK, 0 };

static void zero_bytes(void *p, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) { ((uint8_t *)p)[i] = 0; }
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static struct vc_cell *row_ptr(struct kvt *v, int r) {
    return v->ring[(v->top + r) % VC_RING];
}

static void row_fill(struct kvt *v, struct vc_cell *row, struct vc_cell fill) {
    for (int c = 0; c < v->cols; c++) { row[c] = fill; }
}

static void snap_bottom(struct kvt *v) { v->view = 0; }

// scroll the whole screen up one line, retaining the top line as history
static void scroll_up(struct kvt *v) {
    v->top = (v->top + 1) % VC_RING;
    if (v->history < VC_SCROLLBACK) { v->history++; }
    row_fill(v, row_ptr(v, v->rows - 1), BLANK);
}

static void index_down(struct kvt *v) {
    snap_bottom(v);
    if (v->cy < v->rows - 1) { v->cy++; }
    else { scroll_up(v); }
}

static void put_char(struct kvt *v, uint8_t ch) {
    snap_bottom(v);
    if (v->wrap_pending) { v->cx = 0; index_down(v); v->wrap_pending = 0; }
    struct vc_cell cell = v->pen;
    cell.ch = ch;
    row_ptr(v, v->cy)[v->cx] = cell;
    if (v->cx + 1 >= v->cols) { v->cx = v->cols - 1; v->wrap_pending = 1; }
    else { v->cx++; }
}

void kvt_init(struct kvt *v, int cols, int rows) {
    zero_bytes(v, sizeof(*v));
    v->cols = clampi(cols, 1, VC_MAX_COLS);
    v->rows = clampi(rows, 1, VC_MAX_ROWS);
    v->pen = BLANK;
    v->cursor_visible = 1;
    v->pstate = KVT_GROUND;
    for (int i = 0; i < VC_RING; i++) {
        for (int c = 0; c < VC_MAX_COLS; c++) { v->ring[i][c] = BLANK; }
    }
}

// --- erase ---------------------------------------------------------

static void erase_cells(struct kvt *v, int row, int c0, int c1) {
    struct vc_cell *r = row_ptr(v, row);
    for (int c = c0; c < c1 && c < v->cols; c++) { r[c] = BLANK; }
}

static void erase_display(struct kvt *v, int mode) {
    if (mode == 0) {
        erase_cells(v, v->cy, v->cx, v->cols);
        for (int r = v->cy + 1; r < v->rows; r++) { erase_cells(v, r, 0, v->cols); }
    } else if (mode == 1) {
        for (int r = 0; r < v->cy; r++) { erase_cells(v, r, 0, v->cols); }
        erase_cells(v, v->cy, 0, v->cx + 1);
    } else {
        for (int r = 0; r < v->rows; r++) { erase_cells(v, r, 0, v->cols); }
    }
}

static void erase_line(struct kvt *v, int mode) {
    if (mode == 0)      { erase_cells(v, v->cy, v->cx, v->cols); }
    else if (mode == 1) { erase_cells(v, v->cy, 0, v->cx + 1); }
    else                { erase_cells(v, v->cy, 0, v->cols); }
}

// --- SGR ---------------------------------------------------------

// ANSI colour order (0=blk 1=red 2=grn 3=yel 4=blu 5=mag 6=cyn 7=wht)
// -> VGA / CON_* order (swap the red and blue bits).
static uint8_t ansi_to_con(int a) {
    return (uint8_t)(((a & 4) >> 2) | (a & 2) | ((a & 1) << 2));
}

static void apply_sgr(struct kvt *v) {
    int n = v->nparam ? v->nparam : 1;
    for (int i = 0; i < n; i++) {
        int p = v->params[i];
        if (p == 0)        { v->pen = BLANK; }
        else if (p == 1)   { v->pen.attr |= VC_BOLD; if (v->pen.fg < 8) { v->pen.fg += 8; } }
        else if (p == 7)   { v->pen.attr |= VC_REVERSE; }
        else if (p == 22)  { v->pen.attr &= ~VC_BOLD; if (v->pen.fg >= 8) { v->pen.fg -= 8; } }
        else if (p == 27)  { v->pen.attr &= ~VC_REVERSE; }
        else if (p >= 30 && p <= 37)  { v->pen.fg = (uint8_t)(ansi_to_con(p - 30) + ((v->pen.attr & VC_BOLD) ? 8 : 0)); }
        else if (p == 39)  { v->pen.fg = CON_GREY; }
        else if (p >= 40 && p <= 47)  { v->pen.bg = ansi_to_con(p - 40); }
        else if (p == 49)  { v->pen.bg = CON_BLACK; }
        else if (p >= 90 && p <= 97)  { v->pen.fg = (uint8_t)(ansi_to_con(p - 90) + 8); }
        else if (p >= 100 && p <= 107) { v->pen.bg = (uint8_t)(ansi_to_con(p - 100) + 8); }
    }
}

// --- CSI dispatch ------------------------------------------------

static int P(struct kvt *v, int i, int def) {
    return (i < v->nparam && v->params[i] != 0) ? v->params[i] : def;
}

static void dispatch_csi(struct kvt *v, char final) {
    v->wrap_pending = 0;
    switch (final) {
    case 'A': v->cy = clampi(v->cy - P(v, 0, 1), 0, v->rows - 1); break;
    case 'B': v->cy = clampi(v->cy + P(v, 0, 1), 0, v->rows - 1); break;
    case 'C': v->cx = clampi(v->cx + P(v, 0, 1), 0, v->cols - 1); break;
    case 'D': v->cx = clampi(v->cx - P(v, 0, 1), 0, v->cols - 1); break;
    case 'G': v->cx = clampi(P(v, 0, 1) - 1, 0, v->cols - 1); break;
    case 'H': case 'f':
        v->cy = clampi(P(v, 0, 1) - 1, 0, v->rows - 1);
        v->cx = clampi(P(v, 1, 1) - 1, 0, v->cols - 1);
        break;
    case 'J': erase_display(v, P(v, 0, 0)); break;
    case 'K': erase_line(v, P(v, 0, 0)); break;
    case 'm': apply_sgr(v); break;
    case 'h': case 'l':
        if (v->priv) {
            for (int i = 0; i < v->nparam; i++) {
                if (v->params[i] == 25) { v->cursor_visible = (final == 'h'); }
            }
        }
        break;
    default: break;
    }
}

// --- feed -------------------------------------------------------

static void csi_reset(struct kvt *v) {
    v->nparam = 0;
    for (int i = 0; i < 8; i++) { v->params[i] = 0; }
    v->priv = 0;
}

static void feed_byte(struct kvt *v, uint8_t c) {
    switch (v->pstate) {
    case KVT_GROUND:
        if (c == 0x1b) { v->pstate = KVT_ESC; return; }
        if (c == '\n' || c == '\v' || c == '\f') { index_down(v); return; }
        if (c == '\r') { snap_bottom(v); v->cx = 0; v->wrap_pending = 0; return; }
        if (c == '\b') { snap_bottom(v); if (v->cx) { v->cx--; } v->wrap_pending = 0; return; }
        if (c == '\t') { snap_bottom(v); v->cx = clampi((v->cx & ~7) + 8, 0, v->cols - 1); return; }
        if (c == '\a') { v->bell = 1; return; }
        if (c < 0x20 || c == 0x7f) { return; }
        put_char(v, c);
        return;

    case KVT_ESC:
        if (c == '[') { csi_reset(v); v->pstate = KVT_CSI; return; }
        if (c == 'c') { int cc = v->cols, rr = v->rows; kvt_init(v, cc, rr); v->pstate = KVT_GROUND; return; }
        if (c == '(' || c == ')' || c == '*' || c == '+') { v->pstate = KVT_ESC_SKIP; return; }
        v->pstate = KVT_GROUND;
        return;

    case KVT_ESC_SKIP:
        v->pstate = KVT_GROUND;
        return;

    case KVT_CSI:
        if (c >= '0' && c <= '9') {
            if (v->nparam == 0) { v->nparam = 1; }
            v->params[v->nparam - 1] = v->params[v->nparam - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') { if (v->nparam < 8) { v->nparam++; } return; }
        if (c == '?') { v->priv = 1; return; }
        if (c >= 0x40 && c <= 0x7e) { dispatch_csi(v, (char)c); v->pstate = KVT_GROUND; return; }
        if (c < 0x20) { feed_byte(v, c); return; }   // execute C0, stay in CSI
        v->pstate = KVT_GROUND;
        return;
    }
}

void kvt_feed(struct kvt *v, const char *p, unsigned n) {
    for (unsigned i = 0; i < n; i++) { feed_byte(v, (uint8_t)p[i]); }
}

// --- read-back ------------------------------------------------

const struct vc_cell *kvt_cell(const struct kvt *v, int row, int col) {
    if (row < 0 || row >= v->rows || col < 0 || col >= v->cols) { return &BLANK; }
    int idx = (v->top + row - v->view) % VC_RING;
    if (idx < 0) { idx += VC_RING; }
    return &v->ring[idx][col];
}

void kvt_cursor(const struct kvt *v, int *x, int *y, int *visible) {
    if (x) { *x = v->cx; }
    if (y) { *y = v->cy; }
    if (visible) { *visible = v->cursor_visible && v->view == 0; }
}

void kvt_scroll_view(struct kvt *v, int delta_lines) {
    v->view = clampi(v->view - delta_lines, 0, v->history);
}

// --- selftest ------------------------------------------------

void kvt_selftest(void) {
    static struct kvt v;
    int fail = 0;

    #define CK(cond, tag) do { if (!(cond)) { fail = 1; \
        serial_write_string("[kvt] check failed: " tag "\n"); } } while (0)

    kvt_init(&v, 10, 4);
    kvt_feed(&v, "hi", 2);
    CK(kvt_cell(&v, 0, 0)->ch == 'h', "print");
    kvt_feed(&v, "\r\nX", 3);
    CK(kvt_cell(&v, 1, 0)->ch == 'X', "crlf");
    kvt_feed(&v, "\x1b[1;1HZ", 7);
    CK(kvt_cell(&v, 0, 0)->ch == 'Z', "cup");
    kvt_feed(&v, "\x1b[31mR", 6);
    CK(kvt_cell(&v, 0, 1)->fg == CON_RED, "sgr-fg");
    kvt_feed(&v, "\x1b[2J", 4);
    CK(kvt_cell(&v, 0, 0)->ch == 0, "ed2");

    kvt_init(&v, 4, 2);
    kvt_feed(&v, "a\r\nb\r\nc", 7);
    CK(kvt_cell(&v, 0, 0)->ch == 'b' && kvt_cell(&v, 1, 0)->ch == 'c', "scroll");
    kvt_scroll_view(&v, -1);
    CK(kvt_cell(&v, 0, 0)->ch == 'a', "history");
    #undef CK

    serial_write_string(fail ? "[kvt] selftest FAILED\n" : "[kvt] selftest passed\n");
}
