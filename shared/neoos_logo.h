#ifndef NEOOS_LOGO_H
#define NEOOS_LOGO_H

// The butterfly-N boot logo, shared by the KERNEL banner
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
// colour, same shape: 'P' = purple (the N strokes, body and antennae),
// anything else = red (the wings). Keep the two arrays the same length
// row for row.

static const char *const neoos_logo_art[] = {
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
    0,
};

static const char *const neoos_logo_col[] = {
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
    0,
};

#endif
