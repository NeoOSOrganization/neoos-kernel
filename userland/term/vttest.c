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

int main(void) {
    test_print_and_wrap();
    test_c0();
    test_scroll_and_history();
    test_dirty();
    if (failures == 0) {
        printf("[vttest] ALL PASSED\n");
        return 0;
    }
    printf("[vttest] %d FAILED\n", failures);
    return 1;
}
