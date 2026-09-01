#ifndef NEOOS_CON_DRIVER_H
#define NEOOS_CON_DRIVER_H

#include <stdint.h>

// How the kernel paints text on the screen. Separate from fb_device
// because VGA text mode and a linear framebuffer are different hardware
// (character cells at 0xb8000 vs pixels). fbcon renders onto an
// fb_device; vgacon writes cells; dummycon discards. Boot probes in
// priority order.
//
// M1c-2 interface: streaming, 16-colour. The row/col-addressed
// putc(row,col,ch,attr) + repaint(vc) ops arrive in M1c-3 with the VT
// grid.

// 16-colour indices, VGA order. fbcon maps to RGB; vgacon uses the
// index directly as the attribute foreground nibble.
#define CON_BLACK    0
#define CON_BLUE     1
#define CON_GREEN    2
#define CON_CYAN     3
#define CON_RED      4
#define CON_MAGENTA  5
#define CON_BROWN    6
#define CON_GREY     7
#define CON_DGREY    8
#define CON_LBLUE    9
#define CON_LGREEN   10
#define CON_LCYAN    11
#define CON_LRED     12
#define CON_LMAGENTA 13
#define CON_YELLOW   14
#define CON_WHITE    15

struct con_driver {
    const char *name;
    int priority;
    int  (*probe)(void);
    void (*init)(int *cols, int *rows);       // set up, report geometry
    void (*putc_attr)(char c, uint8_t fg);    // streaming: fg = CON_* index

    // Grid-addressed ops (M1c-3, the VT layer). attr byte = fg | (bg<<4),
    // both CON_* 0..15. No cursor advance, no scroll -- vt.c addresses
    // cells directly.
    void (*putc_at)(int row, int col, char ch, uint8_t attr);
    void (*cursor)(int row, int col, int visible);

    void (*clear)(void);
};

void               con_driver_register(struct con_driver *d);
void               con_driver_register_builtin(void);   // fbcon, vgacon, dummycon
void               con_driver_select(void);   // probe + init the winner
struct con_driver *con_driver_active(void);
void               con_driver_geometry(int *cols, int *rows);   // text cells
void               con_driver_selftest(void); // "[con] ..."

// Free helpers on top of the active driver.
void con_putc(char c);                              // grey
void con_write(const char *s, uint64_t n);          // grey
#endif
