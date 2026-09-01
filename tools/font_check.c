/* Host-side sanity check for the generated glyph table. Compiled with
   the system cc, NOT the cross toolchain -- this is host tooling, not
   kernel code, so the "no host tests" rule (CLAUDE.md) does not apply:
   the bare-metal proof is M1b-3's pixel render test. */
#include <stdio.h>

#include "../userland/term/font_term.h"
#include "../userland/term/font_term.c"

static int px(int c, int x, int y) {
    return (term_glyphs[c][y * 2 + (x >> 3)] >> (7 - (x & 7))) & 1;
}

int main(void) {
    /* 1. space is blank */
    for (int y = 0; y < TERM_GLYPH_H; y++) {
        for (int x = 0; x < TERM_GLYPH_W; x++) {
            if (px(' ', x, y)) {
                printf("[font_check] FAIL: space not blank\n");
                return 1;
            }
        }
    }

    /* 2. 'A' and 'M' have ink; a control char (0x01) does not */
    int ink_a = 0, ink_m = 0, ink_ctl = 0;
    for (int y = 0; y < TERM_GLYPH_H; y++) {
        for (int x = 0; x < TERM_GLYPH_W; x++) {
            ink_a   |= px('A', x, y);
            ink_m   |= px('M', x, y);
            ink_ctl |= px(0x01, x, y);
        }
    }
    if (!ink_a || !ink_m) {
        printf("[font_check] FAIL: A/M empty\n");
        return 1;
    }
    if (ink_ctl) {
        printf("[font_check] FAIL: 0x01 should be blank\n");
        return 1;
    }

    /* 3. render "Ag" so a human can eyeball the committed table */
    const char *s = "Ag";
    for (int y = 0; y < TERM_GLYPH_H; y++) {
        for (const char *p = s; *p; p++) {
            for (int x = 0; x < TERM_GLYPH_W; x++) {
                putchar(px((unsigned char)*p, x, y) ? '#' : '.');
            }
            putchar(' ');
        }
        putchar('\n');
    }
    printf("[font_check] ALL PASSED\n");
    return 0;
}
