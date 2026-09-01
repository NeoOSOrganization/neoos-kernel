/* Host cc, not the cross toolchain. Proves render.c blits the right
   pixels; the on-hardware proof is M1b-3 Task 3's [term] render
   self-check. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../userland/term/vt.h"
#include "../userland/term/render.h"
#include "../userland/term/palette.h"

#include "../userland/term/vt.c"
#include "../userland/term/render.c"
#include "../userland/term/palette.c"
#include "../userland/term/font_term.c"

static uint8_t fbbuf[800 * 64 * 4];
static struct term_fb FB = { fbbuf, 800 * 4, 800, 48, 16, 8, 0 };  /* xRGB8888 */

static uint32_t px(int x, int y) {
    uint32_t v;
    memcpy(&v, (void *)(fbbuf + y * FB.pitch + x * 4), 4);
    return v & 0x00ffffff;
}

int main(void) {
    palette_init();

    struct vt v;
    vt_init(&v, 10, 2);
    vt_feed(&v, (const uint8_t *)"\x1b[31mA", 6);   /* red 'A' in cell (0,0) */

    struct vt_span sp[8];
    int n = vt_take_dirty(&v, sp, 8);
    render_clear(&FB, VT_DEFAULT_BG);
    for (int i = 0; i < n; i++)
        render_span(&FB, &v, sp[i].row, sp[i].col0, sp[i].col1);

    uint32_t red = vt_palette[1] & 0xffffff;
    uint32_t bg  = VT_DEFAULT_BG & 0xffffff;

    int red_seen = 0, stray = 0;
    for (int y = 0; y < TERM_GLYPH_H; y++)
        for (int x = 0; x < TERM_GLYPH_W; x++) {
            uint32_t p = px(x, y);
            if (p == red) red_seen = 1;
            else if (p != bg) stray = 1;
        }
    /* cell (0,1) is untouched -> all background */
    for (int y = 0; y < TERM_GLYPH_H; y++)
        for (int x = TERM_GLYPH_W; x < 2 * TERM_GLYPH_W; x++)
            if (px(x, y) != bg) stray = 1;

    if (!red_seen) { printf("[rendertest] FAILED: no red ink\n"); return 1; }
    if (stray)     { printf("[rendertest] FAILED: stray pixels\n"); return 1; }
    printf("[rendertest] ALL PASSED\n");
    return 0;
}
