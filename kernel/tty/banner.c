#include <stddef.h>
#include "tty/banner.h"
#include "tty/con_driver.h"
#include "version.h"
#include "arch/cpu.h"
#include "mm/pmm.h"
#include "smp/smp.h"
#include "drivers/char/serial.h"

// The butterfly-N. `art` is the glyph rows; `col` is the per-cell
// colour, same shape: 'P' = purple (the N strokes + body + antennae),
// anything else = red (the wings). Iterate the glyphs freely; keep the
// two arrays the same length row-for-row.
static const char *const art[] = {
    "  \\.                    ./  ",
    "   \\`.      .--.      .`/   ",
    " |  \\ `.   /    \\   .` /  | ",
    " |   \\  `-'  ()  '-`  /   | ",
    " |    \\      .--.     /    | ",
    " |  () \\    ( ## )   / ()  | ",
    " |      \\    '--'    /      |",
    " |   .-. \\    ||    / .-.   |",
    " |  ( ) ) \\   ||   / ( ( )  |",
    " |   `-'   \\  ||  /   `-'   |",
    " |  ()      \\ || /      ()  |",
    " |    .--.   \\||/   .--.    |",
    " |   ( () )   \\/   ( () )   |",
    "  \\.  '--'    /\\    '--'  ./ ",
    "   `/        /  \\        \\`  ",
    NULL,
};
static const char *const col[] = {
    "  PP                    PP  ",
    "   PPP      RRRR      PPP   ",
    " P  PP PP   RRRRRR   PP PP P ",
    " P   PP  PPP  RR  PPP  PP   P ",
    " P    PP      RRRR     PP    P ",
    " P  RR PP    RRRRRR   PP RR  P ",
    " P      PP    RRRR    PP      P",
    " P   RRR PP    PP    PP RRR   P",
    " P  RRRRRR PP   PP   PP RRRRR  P",
    " P   RRR   PP  PP  PP   RRR   P",
    " P  RR      PP PP PP      RR  P",
    " P    RRRR   PPPP   RRRR    P",
    " P   RRRRRR   PP   RRRRRR   P",
    "  PP  RRRR    PP    RRRR  PP ",
    "   PP        P  P        PP  ",
    NULL,
};

// --- tiny console-only formatter -------------------------------------

static void bputs(uint8_t fg, const char *s) {
    for (int i = 0; s[i]; i++) { con_driver_active()->putc_attr(s[i], fg); }
}

static void bputu(uint8_t fg, uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) { buf[i++] = '0'; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) { con_driver_active()->putc_attr(buf[--i], fg); }
}

void banner_show(void) {
    struct con_driver *cd = con_driver_active();
    if (!cd) { return; }
    cd->clear();

    // logo
    for (int r = 0; art[r]; r++) {
        const char *a = art[r], *c = col[r];
        int clen = 0;
        while (c && c[clen]) { clen++; }
        for (int i = 0; a[i]; i++) {
            uint8_t fg = (i < clen && c[i] == 'P') ? CON_LMAGENTA : CON_LRED;
            cd->putc_attr(a[i], fg);
        }
        cd->putc_attr('\n', CON_LRED);
    }

    // wordmark
    bputs(CON_LRED, "\n     ");
    con_driver_active()->putc_attr('N', CON_LMAGENTA);
    bputs(CON_LRED, "eoOS  ");
    bputs(CON_LRED, NEOOS_VERSION);
    bputs(CON_LRED, "  (");
    bputs(CON_LRED, NEOOS_GITREV);
    bputs(CON_LRED, ")\n\n");

    char brand[49];
    cpu_brand_string(brand);
    bputs(CON_LRED, "     ");
    bputs(CON_LRED, brand[0] ? brand : "unknown CPU");
    bputs(CON_LRED, "\n     ");

    bputu(CON_LRED, (uint64_t)smp_online_count());
    bputs(CON_LRED, " core");
    if (smp_online_count() != 1) { con_driver_active()->putc_attr('s', CON_LRED); }
    bputs(CON_LRED, "   -   ");

    uint64_t free_mib  = pmm_free_frame_count()  * 4096ull / (1ull << 20);
    uint64_t total_mib = pmm_total_frame_count() * 4096ull / (1ull << 20);
    bputu(CON_LRED, free_mib);
    bputs(CON_LRED, " / ");
    bputu(CON_LRED, total_mib);
    bputs(CON_LRED, " MiB free\n     ");

    char feats[192];
    cpu_feature_string(feats, sizeof feats);
    bputs(CON_LRED, feats);
    bputs(CON_LRED, "\n\n");

    // one serial line, a REQUIRED_MARKER, proving the CPUID/mem reads work
    serial_write_string("[banner] NeoOS " NEOOS_VERSION " (" NEOOS_GITREV ") | ");
    serial_write_string(brand[0] ? brand : "unknown CPU");
    serial_write_string(" | ");
    serial_write_hex64((uint64_t)smp_online_count());
    serial_write_string(" cores | ");
    serial_write_hex64(free_mib);
    serial_write_string("/");
    serial_write_hex64(total_mib);
    serial_write_string(" MiB | ");
    serial_write_string(feats);
    serial_write_string(" | shown\n");
}
