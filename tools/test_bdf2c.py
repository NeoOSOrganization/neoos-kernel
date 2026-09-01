import io
import unittest

import bdf2c

# A 2-glyph fixture with FONTBOUNDINGBOX 12 24 0 -4, and each glyph's
# BBX spanning the full cell -- so BITMAP row 0 maps to cell row 0.
# 'A' (0x41): one set pixel at column 0, row 0.
# 'B' (0x42): one set pixel at column 11, row 23.
_ZERO23 = "\n".join(["0000"] * 23)
FIXTURE = f"""\
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
{_ZERO23}
ENDCHAR
STARTCHAR B
ENCODING 66
DWIDTH 12 0
BBX 12 24 0 -4
BITMAP
{_ZERO23}
0010
ENDCHAR
ENDFONT
"""


class PackTest(unittest.TestCase):
    def test_pixels(self):
        g = bdf2c.parse_bdf(io.StringIO(FIXTURE), cell_w=12, cell_h=24)
        self.assertTrue(bdf2c.pixel(g[0x41], 0, 0))
        self.assertFalse(bdf2c.pixel(g[0x41], 1, 0))
        self.assertFalse(bdf2c.pixel(g[0x41], 0, 1))
        self.assertTrue(bdf2c.pixel(g[0x42], 11, 23))
        self.assertFalse(bdf2c.pixel(g[0x42], 10, 23))
        self.assertEqual(bytes(g[0x43]), b"\x00" * 48)

    def test_row_layout(self):
        g = bdf2c.parse_bdf(io.StringIO(FIXTURE), cell_w=12, cell_h=24)
        self.assertEqual(bytes(g[0x41][0:2]), b"\x80\x00")
        self.assertEqual(bytes(g[0x41][2:48]), b"\x00" * 46)

    def test_deterministic_emit(self):
        g = bdf2c.parse_bdf(io.StringIO(FIXTURE), 12, 24)
        a, b = io.StringIO(), io.StringIO()
        bdf2c.emit_source(a, g, "term_glyphs")
        bdf2c.emit_source(b, g, "term_glyphs")
        self.assertEqual(a.getvalue(), b.getvalue())


if __name__ == "__main__":
    unittest.main()
