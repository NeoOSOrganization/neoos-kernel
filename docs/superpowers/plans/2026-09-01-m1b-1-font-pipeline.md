# M1b-1 — font pipeline — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** turn the Spleen 12×24 bitmap font into a checked-in C glyph
table that the M1b terminal renderer will index, plus a deterministic,
tested converter to regenerate it.

**Architecture:** `third_party/spleen/spleen-12x24.bdf` (vendored, BSD)
→ `tools/bdf2c.py` (Python stdlib only) → `userland/term/font_term.c` +
`font_term.h` (generated, **checked in**, like `kernel/dev/font8x16.c`).
A `make font-check` target regenerates to a temp file and diffs it
against the committed one; a small host C harness confirms the table
compiles, indexes, and matches a hand-verified glyph. No kernel code,
no ELF, no `make test` impact in this sub-milestone — the font is
proven for real in M1b-3 when TERM renders pixels.

**Spec:** `docs/superpowers/specs/2026-09-01-m1b-framebuffer-terminal-design.md` (§2, §9 row M1b-1)

## Global Constraints

- **Font:** Spleen 12×24, `github.com/fcambus/spleen`, **BSD-2-Clause**.
  NokiaPure (the brainstorm pick) is out — proprietary, not
  redistributable as a rasterized derivative. This decision is
  settled; do not revisit it.
- **Vendor the two files directly** (`spleen-12x24.bdf` + `LICENSE`)
  under `third_party/spleen/` — **not** a git submodule. A single
  stable font file does not warrant submodule machinery, and the spec
  says "only `spleen-12x24.bdf` is needed". Keep the upstream `LICENSE`
  verbatim beside it and a one-line `third_party/spleen/README.md`
  naming the upstream URL and commit/release the BDF came from.
- **`tools/bdf2c.py` uses only the Python standard library** — no
  `freetype`, no `fonttools`, no `pip install`. It must run under the
  system `python3`.
- **The generated `font_term.c` / `font_term.h` are committed.** A
  normal `make` never runs Python. `bdf2c.py` is deterministic:
  regenerating from the same BDF produces byte-identical output.
- **Glyph table shape** (final, used by M1b-3's renderer):
  ```c
  // font_term.h
  #define TERM_GLYPH_W 12
  #define TERM_GLYPH_H 24
  extern const unsigned char term_glyphs[256][TERM_GLYPH_H * 2];
  // pixel (x,y) of glyph c is set iff:
  //   term_glyphs[c][y*2 + (x >> 3)] & (0x80 >> (x & 7))
  // byte 0 of a row = columns 0..7 (MSB = column 0),
  // byte 1 = columns 8..11 in its top 4 bits, low 4 bits always 0.
  ```
  Same MSB-first convention as `kernel/dev/font8x16.c`.
- **Codepoint range:** 0x00–0xFF (Latin-1). Codepoints Spleen 12×24
  does not define get an all-zero (blank) glyph; 0x00 is blank too.
  Record in `bdf2c.py`'s output header how many glyphs were found vs.
  blank-filled.
- **Work on `main`**, one commit per task, trailer:
  `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01CNR4gEkyMq6qhFWfxt3KXE`.

## File Structure

| File | Responsibility |
|---|---|
| `third_party/spleen/spleen-12x24.bdf` | vendored font source (BSD) |
| `third_party/spleen/LICENSE` | upstream licence, verbatim |
| `third_party/spleen/README.md` | upstream URL + pinned version, one paragraph |
| `tools/bdf2c.py` | BDF → packed C table, stdlib only, deterministic |
| `tools/test_bdf2c.py` | `unittest` for the packer, with an inline BDF fixture |
| `userland/term/font_term.h` | table declaration + `TERM_GLYPH_*` + the pixel macro |
| `userland/term/font_term.c` | `const unsigned char term_glyphs[256][48]` (generated) |
| `tools/font_check.c` | host C harness: includes `font_term.c`, asserts one hand-verified glyph, renders "Ag" as ASCII art |
| `Makefile` | `font-regen` (regenerate + overwrite) and `font-check` (regenerate to temp, diff, run the C harness) phony targets |

---

## Task 1: `bdf2c.py` + the packer test + vendored font + generated table

**Files:**
- Create: `tools/bdf2c.py`, `tools/test_bdf2c.py`
- Create: `third_party/spleen/spleen-12x24.bdf`, `third_party/spleen/LICENSE`, `third_party/spleen/README.md`
- Create: `userland/term/font_term.h`, `userland/term/font_term.c` (generated)
- Modify: `Makefile` (add `font-regen`, `font-check`; `.PHONY`)

**Interfaces:**
- Produces:
  ```
  python3 tools/bdf2c.py <bdf-path> --array term_glyphs \
      --header userland/term/font_term.h --source userland/term/font_term.c
  # exit 0, writes both files; deterministic
  ```
  ```c
  /* font_term.h */
  #define TERM_GLYPH_W 12
  #define TERM_GLYPH_H 24
  extern const unsigned char term_glyphs[256][TERM_GLYPH_H * 2];
  ```

- [ ] **Step 1: failing test — `tools/test_bdf2c.py`**

```python
import io, unittest
import bdf2c   # tools/ on sys.path via the test runner (see Step 2)

# A 2-glyph fixture: FONTBOUNDINGBOX 12 24 0 -4.
# 'A' (0x41): a single set pixel at column 0, row 0 (top-left of the
# glyph's bounding box, which sits at the top of the cell).
# 'B' (0x42): a single set pixel at column 11, row 23 (bottom-right).
FIXTURE = """\
STARTFONT 2.1
FONTBOUNDINGBOX 12 24 0 -4
STARTPROPERTIES 1
FONT_ASCENT 20
ENDPROPERTIES
CHARS 2
STARTCHAR A
ENCODING 65
DWIDTH 12 0
BBX 12 24 0 -4
BITMAP
8000
{zero_rows_23}
ENDCHAR
STARTCHAR B
ENCODING 66
DWIDTH 12 0
BBX 12 24 0 -4
BITMAP
{zero_rows_23}
0010
ENDCHAR
ENDFONT
""".format(zero_rows_23="\n".join(["0000"] * 23))

class PackTest(unittest.TestCase):
    def test_pixels(self):
        glyphs = bdf2c.parse_bdf(io.StringIO(FIXTURE), cell_w=12, cell_h=24)
        # 'A': pixel (0,0) set, nothing else
        self.assertTrue(bdf2c.pixel(glyphs[0x41], 0, 0))
        self.assertFalse(bdf2c.pixel(glyphs[0x41], 1, 0))
        self.assertFalse(bdf2c.pixel(glyphs[0x41], 0, 1))
        # 'B': pixel (11,23) set
        self.assertTrue(bdf2c.pixel(glyphs[0x42], 11, 23))
        self.assertFalse(bdf2c.pixel(glyphs[0x42], 10, 23))
        # undefined codepoint -> all zero
        self.assertEqual(bytes(glyphs[0x43]), b"\x00" * 48)

    def test_row_layout(self):
        glyphs = bdf2c.parse_bdf(io.StringIO(FIXTURE), cell_w=12, cell_h=24)
        # row 0 of 'A' = 0x80,0x00 ; every other row 0x00,0x00
        self.assertEqual(bytes(glyphs[0x41][0:2]), b"\x80\x00")
        self.assertEqual(bytes(glyphs[0x41][2:48]), b"\x00" * 46)

    def test_deterministic_emit(self):
        a = io.StringIO(); b = io.StringIO()
        g = bdf2c.parse_bdf(io.StringIO(FIXTURE), 12, 24)
        bdf2c.emit_source(a, g, "term_glyphs"); bdf2c.emit_source(b, g, "term_glyphs")
        self.assertEqual(a.getvalue(), b.getvalue())

if __name__ == "__main__":
    unittest.main()
```

Note: the fixture's `BBX ... 0 -4` with `FONTBOUNDINGBOX ... 0 -4`
means the glyph box spans the full cell; the packer maps BBX
row 0 → cell row 0. Real Spleen uses whatever offsets the file
carries — the packer must compute the cell-row for each BBX row from
`FONTBOUNDINGBOX` height/descent and per-glyph `BBX` offsets, not
assume alignment.

- [ ] **Step 2: run — expect FAIL**

```
cd tools && python3 -m unittest test_bdf2c -v
```
Expected: `ModuleNotFoundError: No module named 'bdf2c'` (or, once the
file exists but is empty, `AttributeError`).

- [ ] **Step 3: implement `tools/bdf2c.py`**

Structure (stdlib only — `sys`, `argparse`, `io`):

```python
#!/usr/bin/env python3
"""BDF bitmap font -> packed C glyph table. Standard library only.

Layout of the emitted array `NAME[256][cell_h*2]`: row y of glyph c is
bytes [y*2 : y*2+2], MSB-first, byte 0 = columns 0..7, byte 1 =
columns 8..(cell_w-1) in its high bits. Matches kernel/dev/font8x16.c.
"""
import argparse, io, sys

def parse_bdf(f, cell_w, cell_h):
    """Return list of 256 bytearray(cell_h*2). Undefined codepoints
    are left all-zero. Honours FONTBOUNDINGBOX and per-glyph BBX
    offsets to place each glyph's bitmap within the cell."""
    glyphs = [bytearray(cell_h * 2) for _ in range(256)]
    fbb = None            # (w, h, xoff, yoff) from FONTBOUNDINGBOX
    enc = bbx = None
    rows = []
    in_bitmap = False
    for line in f:
        parts = line.split()
        if not parts:
            continue
        kw = parts[0]
        if kw == "FONTBOUNDINGBOX":
            fbb = tuple(int(x) for x in parts[1:5])
        elif kw == "STARTCHAR":
            enc = bbx = None; rows = []
        elif kw == "ENCODING":
            enc = int(parts[1])
        elif kw == "BBX":
            bbx = tuple(int(x) for x in parts[1:5])   # w h xoff yoff
        elif kw == "BITMAP":
            in_bitmap = True; rows = []
        elif kw == "ENDCHAR":
            in_bitmap = False
            if 0 <= (enc if enc is not None else -1) < 256 and bbx:
                _place(glyphs[enc], rows, bbx, fbb, cell_w, cell_h)
        elif in_bitmap:
            rows.append(kw)
    return glyphs

def _place(dst, hexrows, bbx, fbb, cell_w, cell_h):
    bw, bh, bxo, byo = bbx
    # BDF y grows upward; the glyph's top is at cell row:
    #   top = (fbb_h + fbb_yoff) - (bh + byo)      [pixels from cell top]
    # with fbb providing the cell's own baseline placement.
    fbh, fbyo = fbb[1], fbb[3]
    top = (fbh + fbyo) - (bh + byo)
    left = bxo - fbb[2]
    hexchars = (bw + 7) // 8 * 2
    for i, hr in enumerate(hexrows):
        y = top + i
        if not (0 <= y < cell_h):
            continue
        val = int(hr, 16)
        # hr is left-aligned in bw bits, stored in hexchars*4 bits
        bits = hexchars * 4
        for x in range(bw):
            if val & (1 << (bits - 1 - x)):
                cx = left + x
                if 0 <= cx < cell_w:
                    dst[y * 2 + (cx >> 3)] |= 0x80 >> (cx & 7)

def pixel(glyph, x, y):
    return bool(glyph[y * 2 + (x >> 3)] & (0x80 >> (x & 7)))

def emit_header(f, array, cell_w, cell_h, guard="NEOOS_FONT_TERM_H"):
    f.write(f"""#ifndef {guard}
#define {guard}
/* GENERATED by tools/bdf2c.py from third_party/spleen/spleen-12x24.bdf
   -- do not edit. Regenerate with `make font-regen`. */
#define TERM_GLYPH_W {cell_w}
#define TERM_GLYPH_H {cell_h}
extern const unsigned char {array}[256][TERM_GLYPH_H * 2];
#endif
""")

def emit_source(f, glyphs, array, found=None):
    f.write("/* GENERATED by tools/bdf2c.py -- do not edit. */\n")
    if found is not None:
        f.write(f"/* {found} glyphs from BDF, {256 - found} blank-filled. */\n")
    f.write(f'#include "font_term.h"\n\n')
    f.write(f"const unsigned char {array}[256][TERM_GLYPH_H * 2] = {{\n")
    for c, g in enumerate(glyphs):
        body = ",".join(f"0x{b:02x}" for b in g)
        f.write(f"    {{ {body} }}, /* 0x{c:02x} */\n")
    f.write("};\n")

def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("bdf")
    ap.add_argument("--array", default="term_glyphs")
    ap.add_argument("--header", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--cell", default="12x24")
    a = ap.parse_args(argv)
    cw, ch = (int(x) for x in a.cell.split("x"))
    with open(a.bdf, encoding="latin-1") as f:
        glyphs = parse_bdf(f, cw, ch)
    found = sum(1 for g in glyphs if any(g))
    with open(a.header, "w") as f:
        emit_header(f, a.array, cw, ch)
    with open(a.source, "w") as f:
        emit_source(f, glyphs, a.array, found)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
```

The `_place` baseline arithmetic is the one subtle part — verify it
against the fixture in Step 1 and against two real Spleen glyphs by
hand in Step 6.

- [ ] **Step 4: run the test — expect PASS**

```
cd tools && python3 -m unittest test_bdf2c -v
```
All three tests green. If `test_row_layout` fails, the `_place` offset
math is wrong — fix it here, not by adjusting the test.

- [ ] **Step 5: vendor the Spleen files**

Download `spleen-12x24.bdf` and `LICENSE` from a pinned release of
`github.com/fcambus/spleen` (latest stable tag). Put them at
`third_party/spleen/`. Write `third_party/spleen/README.md`:

```markdown
# Spleen

`spleen-12x24.bdf` and `LICENSE` are vendored verbatim from
https://github.com/fcambus/spleen (release <TAG>, commit <SHA>).

Spleen is BSD-2-Clause. Only the 12×24 face is used, by
`tools/bdf2c.py`, to generate `userland/term/font_term.c`. The `.bdf`
is a build input; the generated table is what ships.
```

- [ ] **Step 6: generate the table, hand-verify two glyphs**

```
python3 tools/bdf2c.py third_party/spleen/spleen-12x24.bdf \
    --array term_glyphs \
    --header userland/term/font_term.h \
    --source userland/term/font_term.c
```

Open `spleen-12x24.bdf`, find the `STARTCHAR` for `A` (ENCODING 65)
and for `M` (ENCODING 77). By hand, decode the first two non-blank
`BITMAP` rows of each into a set of `(x,y)` cell pixels. Confirm the
generated `term_glyphs[0x41]` / `term_glyphs[0x4d]` bytes set exactly
those bits (write the check as a throwaway `python3 -c` using
`bdf2c.pixel`). If they disagree, the `_place` math is still wrong —
fix and regenerate.

Also eyeball: `found` in the source header should be ~95+ (Spleen
12×24 covers ASCII and a good part of Latin-1); if it says e.g. 3,
the parser is dropping glyphs.

- [ ] **Step 7: Makefile targets**

```make
.PHONY: font-regen font-check

FONT_BDF := third_party/spleen/spleen-12x24.bdf
FONT_SRC := userland/term/font_term.c
FONT_HDR := userland/term/font_term.h

font-regen:
	python3 tools/bdf2c.py $(FONT_BDF) --array term_glyphs \
	    --header $(FONT_HDR) --source $(FONT_SRC)

font-check:
	@tmp=$$(mktemp -d); \
	python3 tools/bdf2c.py $(FONT_BDF) --array term_glyphs \
	    --header $$tmp/font_term.h --source $$tmp/font_term.c; \
	diff -u $(FONT_HDR) $$tmp/font_term.h && \
	diff -u $(FONT_SRC) $$tmp/font_term.c && \
	echo "font-check: generated table matches the committed one"; \
	rc=$$?; rm -rf $$tmp; exit $$rc
```
(Task 2 appends the C-harness step to `font-check`.)

- [ ] **Step 8: `make font-check` passes; commit**

```
make font-check
cd tools && python3 -m unittest test_bdf2c
git add tools/bdf2c.py tools/test_bdf2c.py third_party/spleen \
        userland/term/font_term.c userland/term/font_term.h Makefile
git commit -m "M1b-1: Spleen 12x24 -> font_term.c via tools/bdf2c.py"
```

---

## Task 2: host C harness — the table compiles, indexes, and matches

**Files:**
- Create: `tools/font_check.c`
- Modify: `Makefile` (`font-check` runs it after the diff)

**Interfaces:**
- Consumes: `userland/term/font_term.h`, `userland/term/font_term.c`,
  `bdf2c.pixel` semantics (the macro in the header).

- [ ] **Step 1: write `tools/font_check.c`**

```c
/* Host-side sanity check for the generated glyph table. Compiled with
   the system cc, NOT the cross toolchain -- this is host tooling, not
   kernel code, so the "no host tests" rule (CLAUDE.md) does not apply:
   the bare-metal proof is M1b-3's pixel render test. */
#include <stdio.h>
#include <string.h>
#include "../userland/term/font_term.h"
#include "../userland/term/font_term.c"

static int px(int c, int x, int y) {
    return (term_glyphs[c][y * 2 + (x >> 3)] >> (7 - (x & 7))) & 1;
}

int main(void) {
    /* 1. space is blank */
    for (int y = 0; y < TERM_GLYPH_H; y++)
        for (int x = 0; x < TERM_GLYPH_W; x++)
            if (px(' ', x, y)) { printf("[font_check] FAIL: space not blank\n"); return 1; }

    /* 2. 'A' and 'M' have ink; a control char (0x01) does not */
    int inkA = 0, inkM = 0, inkCtl = 0;
    for (int y = 0; y < TERM_GLYPH_H; y++)
        for (int x = 0; x < TERM_GLYPH_W; x++) {
            inkA   |= px('A', x, y);
            inkM   |= px('M', x, y);
            inkCtl |= px(0x01, x, y);
        }
    if (!inkA || !inkM) { printf("[font_check] FAIL: A/M empty\n"); return 1; }
    if (inkCtl)         { printf("[font_check] FAIL: 0x01 should be blank\n"); return 1; }

    /* 3. render "Ag" so a human can eyeball the committed table */
    const char *s = "Ag";
    for (int y = 0; y < TERM_GLYPH_H; y++) {
        for (const char *p = s; *p; p++) {
            for (int x = 0; x < TERM_GLYPH_W; x++)
                putchar(px((unsigned char)*p, x, y) ? '#' : '.');
            putchar(' ');
        }
        putchar('\n');
    }
    printf("[font_check] ALL PASSED\n");
    return 0;
}
```

- [ ] **Step 2: build + run it, eyeball "Ag"**

```
cc -std=c11 -Wall -Wextra -o /tmp/font_check tools/font_check.c && /tmp/font_check
```
Expected: an ASCII-art `A` and `g` that actually look like an A and a
g at 12×24, then `[font_check] ALL PASSED`. If the glyphs look
mirrored, rotated, or vertically offset, the `_place` math in
`bdf2c.py` is wrong — go back to Task 1 Step 3.

- [ ] **Step 3: wire into `make font-check`**

Append to the `font-check` recipe, after the two `diff` lines:

```make
	cc -std=c11 -Wall -Wextra -o $$tmp/font_check tools/font_check.c && $$tmp/font_check
```
(Adjust the `rc`/cleanup so a harness failure fails the target.)

- [ ] **Step 4: full check + commit**

```
make font-check          # diff + harness both pass
```
```
git add tools/font_check.c Makefile
git commit -m "M1b-1: host C harness for the glyph table (font-check)"
```

---

## Self-Review

**Spec coverage (§2, §9 M1b-1):**
- "Spleen 12×24 vendored + LICENSE" → Task 1 Step 5.
- "`tools/bdf2c.py` (stdlib only)" → Task 1 Step 3; enforced by the
  Global Constraints and the test importing nothing but `bdf2c`.
- "generated `userland/term/font_term.c/.h` checked in" → Task 1
  Step 6 + Step 8 commit.
- "Makefile regeneration rule" → Task 1 Step 7 (`font-regen`).
- Acceptance "`tools/bdf2c.py` reproduces the checked-in `font_term.c`
  byte-for-byte" → `font-check` diff, Task 1 Step 7–8.
- Acceptance "a tiny host harness renders 'Ag' … and it matches the
  BDF" → Task 2 (renders "Ag"; Task 1 Step 6 is the by-hand BDF match).

**Placeholder scan:** `<TAG>` / `<SHA>` in the README template are
explicit "fill from the pinned release" instructions, not hidden
TODOs. The `bdf2c.py` body is complete, not a sketch — `_place`'s
baseline arithmetic is written out and Step 6 says to verify it
against real glyphs. No "add error handling" hand-waves.

**Type consistency:** `term_glyphs[256][TERM_GLYPH_H * 2]`,
`TERM_GLYPH_W` = 12, `TERM_GLYPH_H` = 24, and the MSB-first
`(0x80 >> (x & 7))` pixel convention are identical in the Global
Constraints, `font_term.h` (Task 1), `test_bdf2c.py`'s `pixel`
expectations, and `font_check.c`'s `px`. The converter entry point
`python3 tools/bdf2c.py <bdf> --array --header --source` is spelled
the same in the interface block, Task 1 Step 6, and the Makefile.

**Scope:** no kernel code, no ELF, no `make test` / gauntlet changes —
correct for M1b-1. The renderer that consumes this table, and the
pixel-level proof, are M1b-3.
