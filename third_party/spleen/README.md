# Spleen

`spleen-12x24.bdf` and `LICENSE` are vendored verbatim from
<https://github.com/fcambus/spleen> (Spleen 12x24 version 2.2.0,
commit `57f9219328c9f5873085320fe8bc8f7dd34b8791`).

Spleen is BSD-2-Clause (see `LICENSE`). Only the 12×24 face is used, by
`tools/bdf2c.py`, to generate `userland/term/font_term.c`. The `.bdf`
is a build input; the generated table is what ships in the terminal.
