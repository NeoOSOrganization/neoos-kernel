#include "render.h"
#include "palette.h"
#include "font_term.h"

static uint32_t pack(const struct term_fb *fb, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (r << fb->ro) | (g << fb->go) | (b << fb->bo);
}

static uint32_t brighten(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    r = r * 3 / 2; if (r > 255) r = 255;
    g = g * 3 / 2; if (g > 255) g = 255;
    b = b * 3 / 2; if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

void render_clear(const struct term_fb *fb, uint32_t rgb) {
    uint32_t v = pack(fb, rgb);
    for (int y = 0; y < fb->h; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb->pix + y * fb->pitch);
        for (int x = 0; x < fb->w; x++) row[x] = v;
    }
}

void render_span(const struct term_fb *fb, const struct vt *v,
                 int row, int col0, int col1) {
    int y0 = row * TERM_GLYPH_H;
    if (y0 + TERM_GLYPH_H > fb->h) return;

    for (int c = col0; c < col1; c++) {
        const struct vt_cell *cell = vt_cell_at(v, row, c);
        if (!cell) continue;

        uint32_t fg = (cell->attr & VT_DEFFG) ? VT_DEFAULT_FG : vt_palette[cell->fg];
        uint32_t bg = (cell->attr & VT_DEFBG) ? VT_DEFAULT_BG : vt_palette[cell->bg];
        if (cell->attr & VT_REVERSE) { uint32_t t = fg; fg = bg; bg = t; }
        if (cell->attr & VT_BOLD)    { fg = brighten(fg); }
        if (cell->attr & VT_DIM)     { fg = (fg >> 1) & 0x7f7f7f; }
        if (cell->attr & VT_HIDDEN)  { fg = bg; }

        uint32_t pfg = pack(fb, fg), pbg = pack(fb, bg);
        const uint8_t *g = term_glyphs[cell->ch];
        int x0 = c * TERM_GLYPH_W;
        if (x0 + TERM_GLYPH_W > fb->w) break;

        for (int gy = 0; gy < TERM_GLYPH_H; gy++) {
            volatile uint32_t *dst =
                (volatile uint32_t *)(fb->pix + (y0 + gy) * fb->pitch) + x0;
            int under = (cell->attr & VT_UNDERLINE) && gy == TERM_GLYPH_H - 2;
            for (int gx = 0; gx < TERM_GLYPH_W; gx++) {
                int ink = (g[gy * 2 + (gx >> 3)] >> (7 - (gx & 7))) & 1;
                dst[gx] = (ink || under) ? pfg : pbg;
            }
        }
    }
}

void render_cursor(const struct term_fb *fb, const struct vt *v) {
    int x, y, visible;
    vt_cursor(v, &x, &y, &visible);
    if (!visible || vt_view_offset(v) != 0) return;

    int x0 = x * TERM_GLYPH_W, y0 = y * TERM_GLYPH_H;
    if (x0 + TERM_GLYPH_W > fb->w || y0 + TERM_GLYPH_H > fb->h) return;

    const struct vt_cell *cell = vt_cell_at(v, y, x);
    uint32_t fg = (cell && !(cell->attr & VT_DEFFG)) ? vt_palette[cell->fg] : VT_DEFAULT_FG;
    uint32_t bg = (cell && !(cell->attr & VT_DEFBG)) ? vt_palette[cell->bg] : VT_DEFAULT_BG;
    const uint8_t *g = cell ? term_glyphs[cell->ch] : term_glyphs[0];
    uint32_t pfg = pack(fb, bg), pbg = pack(fb, fg);   // inverted

    for (int gy = 0; gy < TERM_GLYPH_H; gy++) {
        volatile uint32_t *dst =
            (volatile uint32_t *)(fb->pix + (y0 + gy) * fb->pitch) + x0;
        for (int gx = 0; gx < TERM_GLYPH_W; gx++) {
            int ink = (g[gy * 2 + (gx >> 3)] >> (7 - (gx & 7))) & 1;
            dst[gx] = ink ? pfg : pbg;
        }
    }
}

// Draws one glyph at a CELL position with explicit colours, bypassing
// the VT grid entirely. The logo header is not terminal content -- it
// must not scroll, must not be in the scrollback, and must survive the
// shell clearing the screen -- so it is painted straight into the
// framebuffer instead.
void render_glyph_at(const struct term_fb *fb, int row, int col,
                     unsigned char ch, uint32_t fg_rgb, uint32_t bg_rgb) {
    int y0 = row * TERM_GLYPH_H;
    int x0 = col * TERM_GLYPH_W;
    if (y0 + TERM_GLYPH_H > fb->h || x0 + TERM_GLYPH_W > fb->w) { return; }

    uint32_t pfg = pack(fb, fg_rgb), pbg = pack(fb, bg_rgb);
    const uint8_t *g = term_glyphs[ch];
    for (int gy = 0; gy < TERM_GLYPH_H; gy++) {
        volatile uint32_t *dst =
            (volatile uint32_t *)(fb->pix + (y0 + gy) * fb->pitch) + x0;
        for (int gx = 0; gx < TERM_GLYPH_W; gx++) {
            int ink = (g[gy * 2 + (gx >> 3)] >> (7 - (gx & 7))) & 1;
            dst[gx] = ink ? pfg : pbg;
        }
    }
}

void render_text_at(const struct term_fb *fb, int row, int col,
                    const char *s, uint32_t fg_rgb, uint32_t bg_rgb) {
    for (int i = 0; s[i]; i++) {
        render_glyph_at(fb, row, col + i, (unsigned char)s[i], fg_rgb, bg_rgb);
    }
}
