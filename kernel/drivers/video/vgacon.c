#include "drivers/video/vgacon.h"
#include "tty/con_driver.h"
#include "mm/paging.h"

static volatile unsigned short *VGA_BUFFER;
#define VGA_CELL_COUNT 2000
#define VGA_COLUMNS 80
#define VGA_ROWS    25

static int vga_cursor;

void vgacon_clear(void) {
    VGA_BUFFER = (volatile unsigned short *)phys_to_virt(0xb8000);
    for (int i = 0; i < VGA_CELL_COUNT; i++) {
        VGA_BUFFER[i] = (unsigned short)' ' | ((unsigned short)CON_GREY << 8);
    }
    vga_cursor = 0;
}

void vgacon_putc(char c, uint8_t fg) {
    if (!VGA_BUFFER) { VGA_BUFFER = (volatile unsigned short *)phys_to_virt(0xb8000); }
    if (c == '\n') {
        vga_cursor += VGA_COLUMNS - (vga_cursor % VGA_COLUMNS);
    } else if (c == '\r') {
        vga_cursor -= vga_cursor % VGA_COLUMNS;
    } else {
        VGA_BUFFER[vga_cursor] =
            (unsigned short)(unsigned char)c | ((unsigned short)(fg & 0x0f) << 8);
        vga_cursor++;
    }
    if (vga_cursor >= VGA_CELL_COUNT) {
        // scroll one line
        for (int i = 0; i < VGA_CELL_COUNT - VGA_COLUMNS; i++) {
            VGA_BUFFER[i] = VGA_BUFFER[i + VGA_COLUMNS];
        }
        for (int i = VGA_CELL_COUNT - VGA_COLUMNS; i < VGA_CELL_COUNT; i++) {
            VGA_BUFFER[i] = (unsigned short)' ' | ((unsigned short)CON_GREY << 8);
        }
        vga_cursor -= VGA_COLUMNS;
    }
}

// ---- con_driver --------------------------------------------------

static int  vgacon_probe(void) { return 1; }   // always available
static void vgacon_init(int *cols, int *rows) {
    *cols = VGA_COLUMNS;
    *rows = VGA_ROWS;
    vgacon_clear();
}

struct con_driver vgacon_drv = {
    .name = "vgacon", .priority = 10,
    .probe = vgacon_probe, .init = vgacon_init,
    .putc_attr = vgacon_putc, .clear = vgacon_clear,
};
