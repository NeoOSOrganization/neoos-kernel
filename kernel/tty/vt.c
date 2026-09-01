#include "tty/vt.h"
#include "tty/kvt.h"
#include "tty/tty.h"
#include "tty/con_driver.h"
#include "drivers/char/serial.h"
#include "sync/waitq.h"
#include "sync/lock.h"
#include "errno.h"

struct vt_console {
    struct tty tty;
    struct kvt scr;
    int kd_mode;                       // KD_TEXT or KD_GRAPHICS
    struct waitq wait_active;          // VT_WAITACTIVE sleepers
    struct vc_cell shown[VC_MAX_ROWS][VC_MAX_COLS];   // last painted (diff)
    int shown_valid;
};

static struct vt_console vts[VT_COUNT];
static int vt_active;
static int g_cols, g_rows;

// pack a cell to the con_driver attr byte (fg | bg<<4), honouring reverse
static uint8_t pack(const struct vc_cell *c) {
    uint8_t fg = c->fg & 15, bg = c->bg & 15;
    if (c->attr & VC_REVERSE) { uint8_t t = fg; fg = bg; bg = t; }
    return (uint8_t)(fg | (bg << 4));
}

static void render_diff(struct vt_console *vc) {
    struct con_driver *cd = con_driver_active();
    if (!cd || !cd->putc_at) { return; }
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            const struct vc_cell *cell = kvt_cell(&vc->scr, r, c);
            struct vc_cell *prev = &vc->shown[r][c];
            if (!vc->shown_valid ||
                cell->ch != prev->ch || cell->fg != prev->fg ||
                cell->bg != prev->bg || cell->attr != prev->attr) {
                cd->putc_at(r, c, (char)cell->ch, pack(cell));
                *prev = *cell;
            }
        }
    }
    vc->shown_valid = 1;
    int x, y, vis;
    kvt_cursor(&vc->scr, &x, &y, &vis);
    if (cd->cursor) { cd->cursor(y, x, vis); }
}

static void render_full(struct vt_console *vc) {
    vc->shown_valid = 0;
    render_diff(vc);
}

// --- tty backend: cooked output feeds the parser ---------------------

static void vt_backend_output(struct tty *t, const char *s, uint32_t n) {
    struct vt_console *vc = t->backend_priv;
    kvt_feed(&vc->scr, s, n);
    if (vc == &vts[vt_active]) {
        serial_write_raw_n(s, n);              // the visible VT mirrors to serial
        if (vc->kd_mode == KD_TEXT) { render_diff(vc); }
    }
}
static const struct tty_backend vt_backend = { vt_backend_output };

// --- public ------------------------------------------------------

void vt_init(void) {
    con_driver_geometry(&g_cols, &g_rows);
    if (g_cols <= 0) { g_cols = 80; }
    if (g_rows <= 0) { g_rows = 25; }
    if (g_cols > VC_MAX_COLS) { g_cols = VC_MAX_COLS; }
    if (g_rows > VC_MAX_ROWS) { g_rows = VC_MAX_ROWS; }

    for (int i = 0; i < VT_COUNT; i++) {
        kvt_init(&vts[i].scr, g_cols, g_rows);
        vts[i].kd_mode = KD_TEXT;
        vts[i].shown_valid = 0;
        waitq_init(&vts[i].wait_active);
        tty_obj_init(&vts[i].tty, &vt_backend, &vts[i]);
        vts[i].tty.win.ws_col = (uint16_t)g_cols;
        vts[i].tty.win.ws_row = (uint16_t)g_rows;
    }
    vt_active = 0;
    serial_write_string("[vt] virtual terminals ready\n");
}

struct tty *vt_active_tty(void)   { return &vts[vt_active].tty; }
int  vt_active_index(void)        { return vt_active; }
void vt_active_geometry(int *cols, int *rows) {
    if (cols) { *cols = g_cols; }
    if (rows) { *rows = g_rows; }
}

struct tty *vt_tty(int vt_index) {
    if (vt_index <= 0) { return &vts[vt_active].tty; }
    if (vt_index > VT_COUNT) { return 0; }
    return &vts[vt_index - 1].tty;
}

void vt_switch(int n) {
    if (n < 0 || n >= VT_COUNT || n == vt_active) { return; }
    vt_active = n;
    struct con_driver *cd = con_driver_active();
    if (cd && cd->clear) { cd->clear(); }
    if (vts[n].kd_mode == KD_TEXT) { render_full(&vts[n]); }
    waitq_wake_all(&vts[n].wait_active);
}

void vt_scroll(int delta_lines) {
    kvt_scroll_view(&vts[vt_active].scr, delta_lines);
    render_full(&vts[vt_active]);
}

void vt_write_active(const char *s, unsigned n) {
    tty_obj_write(&vts[vt_active].tty, s, n);
}

void vt_panic_reset(void) {
    vt_active = 0;
    vts[0].kd_mode = KD_TEXT;
    struct con_driver *cd = con_driver_active();
    if (cd && cd->clear) { cd->clear(); }
    render_full(&vts[0]);
}

int64_t vt_ioctl(int vt_index, uint64_t request, void *arg) {
    int idx = (vt_index <= 0) ? vt_active : (vt_index - 1);
    if (idx < 0 || idx >= VT_COUNT) { return -EINVAL; }
    long a = (long)(intptr_t)arg;

    switch (request) {
    case VT_ACTIVATE:
        if (a < 1 || a > VT_COUNT) { return -EINVAL; }
        vt_switch((int)a - 1);
        return 0;
    case VT_WAITACTIVE:
        if (a < 1 || a > VT_COUNT) { return -EINVAL; }
        while (vt_active != (int)a - 1) {
            waitq_sleep(&vts[a - 1].wait_active, 0);
        }
        return 0;
    case VT_GETSTATE: {
        if (!arg) { return -EFAULT; }
        struct vt_stat *st = arg;
        st->v_active = (uint16_t)(vt_active + 1);
        st->v_signal = 0;
        st->v_state  = (uint16_t)(((1u << VT_COUNT) - 1) << 1);   // VTs 1..N allocated
        return 0;
    }
    case VT_OPENQRY:
        if (!arg) { return -EFAULT; }
        *(int *)arg = vt_active + 1;    // no free-VT pool; the active one
        return 0;
    case VT_GETMODE:
    case VT_SETMODE:
    case VT_RELDISP:
        return 0;                       // accepted, inert until M1c-4
    case KDSETMODE:
        vts[idx].kd_mode = (a == KD_GRAPHICS) ? KD_GRAPHICS : KD_TEXT;
        if (idx == vt_active && vts[idx].kd_mode == KD_TEXT) { render_full(&vts[idx]); }
        return 0;
    case KDGETMODE:
        if (!arg) { return -EFAULT; }
        *(int *)arg = vts[idx].kd_mode;
        return 0;
    default:
        return -ENOTTY;
    }
}

void vt_selftest(void) {
    int fail = 0;

    // a write to a background VT updates its grid, paints nothing
    vt_switch(0);
    vts[2].shown_valid = 0;
    tty_obj_write(&vts[2].tty, "probe\n", 6);
    if (kvt_cell(&vts[2].scr, 0, 0)->ch != 'p') { fail = 1; }
    if (vts[2].shown_valid) { fail = 1; }   // never rendered

    vt_switch(2);
    if (vt_active != 2 || !vts[2].shown_valid) { fail = 1; }
    vt_switch(0);
    vt_panic_reset();
    if (vt_active != 0) { fail = 1; }

    serial_write_string(fail ? "[vt] selftest FAILED\n" : "[vt] selftest passed\n");
}
