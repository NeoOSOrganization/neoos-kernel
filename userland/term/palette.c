#include "palette.h"

uint32_t vt_palette[256];

// The 16 base colours (standard xterm/VGA-ish values).
static const uint32_t base16[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaa5500,
    0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
    0x555555, 0xff5555, 0x55ff55, 0xffff55,
    0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

void palette_init(void) {
    for (int i = 0; i < 16; i++) {
        vt_palette[i] = base16[i];
    }
    // 6x6x6 colour cube: level 0 -> 0, levels 1..5 -> 95,135,175,215,255.
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int rv = r ? 55 + r * 40 : 0;
                int gv = g ? 55 + g * 40 : 0;
                int bv = b ? 55 + b * 40 : 0;
                vt_palette[16 + 36 * r + 6 * g + b] =
                    ((uint32_t)rv << 16) | ((uint32_t)gv << 8) | (uint32_t)bv;
            }
        }
    }
    // grayscale ramp 232..255: 8, 18, ..., 238.
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        vt_palette[232 + i] = ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
    }
}
