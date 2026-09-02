#ifndef NEOOS_LOGO_H
#define NEOOS_LOGO_H

// The boot logo, shared by the KERNEL banner
// (kernel/tty/banner.c) and the USERLAND terminal (userland/term), which
// repaints it as a static header so the logo survives the terminal
// taking over the framebuffer.
//
// It lives in shared/ -- on both include paths and nothing else's --
// because the alternative is two copies that drift. Deliberately has no
// dependencies at all: no headers, no types beyond char, nothing either
// tree has to provide. That is what makes it safe for a freestanding
// kernel and a userland program to include the same file.
//
// `neoos_logo_art` is the glyph rows; `neoos_logo_col` is the per-cell
// colour, same shape: 'P' = purple (the @ accents), anything else = red
// (the rest of the wing/body strokes). Keep the two arrays the same
// length row for row.
// artist : Lorrie , https://www.asciiart.eu/art/4b04b264e7cfb9ca

static const char *const neoos_logo_art[] = {
    " / `._      .       .      _.' \\",
    "'.@ = `.     \\     /     .' = @.'",
    " \\ @`.@ `.    \\   /    .' @.'@ /",
    "  \\;`@`.@ `.   \\ /   .' @.'@`;/",
    "   \\`.@ `.@ `'.(\").'` @.' @.'/",
    "    \\ '=._`. @ :=: @ .'_.=' /",
    "     \\ @  '.'..'='..'.'  @ /",
    "      \\_@_.==.: = :.==._@_/",
    "      /  @ @_.: = :._@ @  \\",
    "     /@ _.-'  : = :  '-._ @\\",
    "    /`'@ @ .-': = :'-.@ @`'`\\",
    "    \\.@_.=` .-: = :-. `=._@./",
    "      \\._.-'   '.'   '-._./  lc",
    0,
};

static const char *const neoos_logo_col[] = {
    " R RRR      R       R      RRR R",
    "RRP R RR     R     R     RR R PRR",
    " R PRRP RR    R   R    RR PRRP R",
    "  RRRPRRP RR   R R   RR PRRPRRR",
    "   RRRP RRP RRRRRRRRR PRR PRRR",
    "    R RRRRRR P RRR P RRRRRR R",
    "     R P  RRRRRRRRRRRRR  P R",
    "      RRPRRRRRR R RRRRRRPRR",
    "      R  P PRRR R RRRP P  R",
    "     RP RRRR  R R R  RRRR PR",
    "    RRRP P RRRR R RRRRP PRRRR",
    "    RRPRRRR RRR R RRR RRRRPRR",
    "      RRRRRR   RRR   RRRRRR  RR",
    0,
};

#endif