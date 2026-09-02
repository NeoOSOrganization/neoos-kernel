#!/usr/bin/env python3
"""Render shared/neoos_logo.h into an ANSI-coloured text file.

    tools/logo2ansi.py shared/neoos_logo.h > nsh.logo

The art and its per-cell colour mask live in ONE place -- the shared
header the kernel banner also draws from -- so editing the logo updates
the boot banner and the shell's greeting together. Generating the text
file at build time is what keeps that true; a hand-written copy would
drift the first time the art changed.

'P' in the colour mask is bright magenta, anything else bright red,
matching kernel/tty/banner.c.
"""
import re
import sys

MAGENTA = "\033[95m"
RED     = "\033[91m"
RESET   = "\033[0m"


def rows(src, name):
    m = re.search(r"%s\[\] = \{(.*?)\};" % name, src, re.S)
    if not m:
        sys.exit("logo2ansi: %s not found" % name)
    out = []
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
        # Undo C escaping: the header is C source, so a backslash in the
        # art is written \\ and a quote \".
        out.append(lit.replace('\\\\', '\\').replace('\\"', '"'))
    return out


def main():
    src = open(sys.argv[1]).read()
    art = rows(src, "neoos_logo_art")
    col = rows(src, "neoos_logo_col")

    out = []
    for i, line in enumerate(art):
        mask = col[i] if i < len(col) else ""
        cur = None
        buf = []
        for j, ch in enumerate(line):
            want = MAGENTA if (j < len(mask) and mask[j] == 'P') else RED
            if want != cur:
                buf.append(want)
                cur = want
            buf.append(ch)
        buf.append(RESET)
        out.append("".join(buf))

    sys.stdout.write("\n".join(out))
    sys.stdout.write("\n" + MAGENTA + "  NeoOS" + RESET + "\n")


if __name__ == "__main__":
    main()
