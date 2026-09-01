#include "drivers/video/vga.h"
#include "mm/paging.h"

static volatile unsigned short *VGA_BUFFER;
static const unsigned short VGA_COLOR_WHITE_ON_BLACK = 0x0f;
#define VGA_CELL_COUNT 2000
#define VGA_COLUMNS 80

static int vga_cursor = 0;

void vga_clear(void) {
    VGA_BUFFER = (volatile unsigned short *)phys_to_virt(0xb8000);
    for (int i = 0; i < VGA_CELL_COUNT; i++) {
        VGA_BUFFER[i] = (unsigned short)(' ') | (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
    }
    vga_cursor = 0;
}

void vga_print_string(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        vga_putc(str[i]);
    }
}

void vga_putc(char c) {
    if (c == '\n') {
        vga_cursor += VGA_COLUMNS - (vga_cursor % VGA_COLUMNS);
    } else {
        VGA_BUFFER[vga_cursor] = (unsigned short)((unsigned char)c) |
                                 (unsigned short)(VGA_COLOR_WHITE_ON_BLACK << 8);
        vga_cursor++;
    }
    if (vga_cursor >= VGA_CELL_COUNT) {
        vga_cursor = 0;
    }
}
