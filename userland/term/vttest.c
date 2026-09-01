// Headless checks for the VT engine. Prints "[vttest] ALL PASSED" on
// success; one "[vttest] FAILED: ..." line per failed check.

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "vt.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("[vttest] FAILED: %s\n", msg); failures++; } } while (0)

static struct vt V;

static char cell_ch(int r, int c) {
    const struct vt_cell *x = vt_cell_at(&V, r, c);
    return x ? (char)x->ch : '?';
}
static void feed(const char *s) {
    vt_feed(&V, (const uint8_t *)s, strlen(s));
}

static void test_print_and_wrap(void) {
    vt_init(&V, 10, 4);
    feed("hi");
    CHECK(cell_ch(0, 0) == 'h' && cell_ch(0, 1) == 'i', "basic print");
    int x, y, vis;
    vt_cursor(&V, &x, &y, &vis);
    CHECK(x == 2 && y == 0, "cursor after print");
    feed("23456789");                    // cols 2..9 filled, wrap pending
    CHECK(cell_ch(0, 9) == '9', "last col");
    feed("X");                            // wraps to row 1 col 0
    CHECK(cell_ch(1, 0) == 'X', "autowrap to next row");
}

static void test_c0(void) {
    vt_init(&V, 10, 4);
    feed("abc\r");
    int x, y, v;
    vt_cursor(&V, &x, &y, &v);
    CHECK(x == 0 && y == 0, "CR");
    feed("Z");
    CHECK(cell_ch(0, 0) == 'Z', "overwrite after CR");
    feed("\n");
    vt_cursor(&V, &x, &y, &v);
    CHECK(y == 1 && x == 1, "LF row advance, col kept");
    feed("\bq");
    CHECK(cell_ch(1, 0) == 'q', "BS then print");
    feed("\t");
    vt_cursor(&V, &x, &y, &v);
    CHECK(x == 8, "tab stop");
}

static void test_scroll_and_history(void) {
    vt_init(&V, 4, 2);
    feed("a\r\nb\r\nc");                  // 'a' -> history; screen shows b / c
    CHECK(cell_ch(0, 0) == 'b' && cell_ch(1, 0) == 'c', "screen after scroll");
    CHECK(vt_view_offset(&V) == 0, "view at bottom");
    vt_scroll_view(&V, -1);
    CHECK(vt_view_offset(&V) == 1, "scrolled one line into history");
    CHECK(cell_ch(0, 0) == 'a', "history line visible");
    feed("d");
    CHECK(vt_view_offset(&V) == 0, "snap back on output");
}

static void test_dirty(void) {
    vt_init(&V, 8, 4);
    struct vt_span sp[8];
    vt_take_dirty(&V, sp, 8);             // clear init's dirty
    feed("x");
    int n = vt_take_dirty(&V, sp, 8);
    CHECK(n == 1 && sp[0].row == 0, "one dirty row after a print");
    n = vt_take_dirty(&V, sp, 8);
    CHECK(n == 0, "dirty set cleared");
}

static uint8_t attr_at(int r, int c) {
    const struct vt_cell *x = vt_cell_at(&V, r, c);
    return x ? x->attr : 0;
}
static uint8_t fg_at(int r, int c) {
    const struct vt_cell *x = vt_cell_at(&V, r, c);
    return x ? x->fg : 0;
}

static void test_csi_cursor(void) {
    vt_init(&V, 20, 10);
    feed("\x1b[5;3H""Z");                 // CUP (5,3) 1-based -> (4,2)
    CHECK(cell_ch(4, 2) == 'Z', "CUP places");
    feed("\x1b[2A");
    int x, y, v;
    vt_cursor(&V, &x, &y, &v);
    CHECK(y == 2 && x == 3, "CUU");
    feed("\x1b[100C");
    vt_cursor(&V, &x, &y, &v);
    CHECK(x == 19, "CUF clamps");
    feed("\x1b[H");
    vt_cursor(&V, &x, &y, &v);
    CHECK(x == 0 && y == 0, "CUP home");
}

static void test_csi_erase(void) {
    vt_init(&V, 6, 3);
    feed("abcdef\r\nghijkl\r\nmno");
    feed("\x1b[2J");
    CHECK(cell_ch(0, 0) == 0 && cell_ch(1, 3) == 0, "ED 2 clears");
    feed("\x1b[HAB\x1b[K");
    CHECK(cell_ch(0, 0) == 'A' && cell_ch(0, 2) == 0, "EL 0 from cursor");
}

static void test_sgr(void) {
    vt_init(&V, 10, 2);
    feed("\x1b[1;4;31m""R");
    CHECK((attr_at(0, 0) & (VT_BOLD | VT_UNDERLINE)) == (VT_BOLD | VT_UNDERLINE),
          "SGR bold+ul");
    CHECK(!(attr_at(0, 0) & VT_DEFFG) && fg_at(0, 0) == 1, "SGR fg red");
    feed("\x1b[0m""x");
    CHECK(attr_at(0, 1) == (VT_DEFFG | VT_DEFBG), "SGR reset");
    feed("\x1b[38;5;200m""y");
    CHECK(!(attr_at(0, 2) & VT_DEFFG) && fg_at(0, 2) == 200, "SGR 256 fg");
}

static void test_scroll_region(void) {
    vt_init(&V, 4, 4);
    feed("\x1b[2;3r");                    // DECSTBM rows 2..3
    feed("\x1b[2;1HA\r\nB");
    feed("\r\nC");                        // LF at region bottom scrolls region only
    CHECK(cell_ch(1, 0) == 'B' && cell_ch(2, 0) == 'C', "region scrolled");
    CHECK(vt_view_offset(&V) == 0 && vt_cell_at(&V, 0, 0)->ch == 0,
          "row 0 untouched by region scroll");
}

static void test_decsc(void) {
    vt_init(&V, 10, 4);
    feed("\x1b[3;4H\x1b" "7");
    feed("\x1b[1;1Hxxxxx");
    feed("\x1b" "8""Q");
    CHECK(cell_ch(2, 3) == 'Q', "DECRC");
}

int main(void) {
    test_print_and_wrap();
    test_c0();
    test_scroll_and_history();
    test_dirty();
    test_csi_cursor();
    test_csi_erase();
    test_sgr();
    test_scroll_region();
    test_decsc();
    if (failures == 0) {
        printf("[vttest] ALL PASSED\n");
        return 0;
    }
    printf("[vttest] %d FAILED\n", failures);
    return 1;
}
