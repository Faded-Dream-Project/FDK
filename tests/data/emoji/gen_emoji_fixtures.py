#!/usr/bin/env python3
"""gen_emoji_fixtures.py — the C-3 test fonts, built with fontTools (the
reference implementation) so the C parser's wire reading is pinned
against what the reference actually writes.

Three faces, each exercising one family:

  colr0.ttf   — COLRv0 + CPAL: two color glyphs, one multi-layer with
                palette alpha.
  colr1.ttf   — COLRv1: solid + linear/radial/sweep gradients,
                transforms (rotate-around-center, translate),
                PaintColrLayers stacking, PaintComposite (multiply),
                a PaintColrGlyph reuse, and a PaintVarSolid.
  cbdt.ttf    — CBDT/CBLC: a bitmap-only face (no glyf table at all —
                the Noto Color Emoji posture) with format-17 PNG
                strikes at two ppem sizes, index subtable formats 1
                and 4.

Written into tests/data/emoji/ and checked in; the generator rides
alongside (the gifgen pattern)."""
import os
import struct
import zlib

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.ttLib.tables import otTables as ot

OUT = os.path.dirname(os.path.abspath(__file__))
UPM = 1000


def base_font(glyphs, cmap_extra):
    glyph_order = list(glyphs.keys())
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(glyph_order)
    fb.setupCharacterMap({c: g for c, g in
                          [(0x45, "E"), (0x46, "F"), (0x47, "G"),
                           (0x48, "H"), (0x49, "I"), (0x4A, "J"),
                           (0x4B, "K"), (0x4C, "L"), (0x4D, "M"),
                           (0x20, "space")]
                          if g in glyph_order})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({name: (600, 0) for name in glyph_order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "FDK Test Emoji",
                       "styleName": "Regular",
                       "psName": "FDKTestEmoji"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200,
                usWinAscent=800, usWinDescent=200)
    return fb


def box_glyph(x0, y0, x1, y1):
    pen = TTGlyphPen(None)
    pen.moveTo((x0, y0))
    pen.lineTo((x1, y0))
    pen.lineTo((x1, y1))
    pen.lineTo((x0, y1))
    pen.closePath()
    return pen.glyph()


def box_layer(glyph_name, color_id):
    """A PaintGlyph wrapping a solid fill — the COLRv1 workhorse."""
    return (ot.PaintFormat.PaintGlyph,
            (ot.PaintFormat.PaintSolid, color_id, 1.0), glyph_name)


def build_colr0():
    glyphs = {
        ".notdef": box_glyph(0, 0, 100, 100),
        "space": box_glyph(0, 0, 0, 0),
        "E": box_glyph(0, 0, 500, 500),
        "E.l1": box_glyph(0, 0, 500, 500),
        "E.l2": box_glyph(0, 0, 500, 500),
        "E.l3": box_glyph(0, 0, 250, 500),
        "F": box_glyph(0, 0, 500, 500),
        "F.l1": box_glyph(0, 0, 500, 500),
    }
    fb = base_font(glyphs, None)
    fb.font["COLR"] = buildCOLR({
        "E": [("E.l1", 0), ("E.l2", 1), ("E.l3", 2)],
        "F": [("F.l1", 3)],
    }, version=0)
    fb.font["CPAL"] = buildCPAL([[(1.0, 0.0, 0.0, 1.0),   # 0: red
                                  (0.0, 1.0, 0.0, 0.5),   # 1: green, a=0.5
                                  (0.0, 0.0, 1.0, 1.0),   # 2: blue
                                  (1.0, 1.0, 0.0, 1.0)]])  # 3: yellow
    path = os.path.join(OUT, "colr0.ttf")
    fb.save(path)
    print("wrote", path)


def build_colr1():
    glyphs = {
        ".notdef": box_glyph(0, 0, 100, 100),
        "space": box_glyph(0, 0, 0, 0),
        "E": box_glyph(0, 0, 500, 500),
        "F": box_glyph(0, 0, 500, 500),
        "G": box_glyph(0, 0, 500, 500),
        "H": box_glyph(0, 0, 500, 500),
        "I": box_glyph(0, 0, 500, 250),  # a RECTANGLE: rotated 90
                                        # degrees it becomes the
                                        # vertical 250x500 box the
                                        # transform test pins
        "J": box_glyph(0, 0, 500, 500),
        "J.bd": box_glyph(0, 0, 500, 500),
        "J.src": box_glyph(0, 0, 500, 500),
        "K": box_glyph(0, 0, 500, 500),
        "L": box_glyph(0, 0, 500, 500),
        "L.l1": box_glyph(0, 0, 500, 500),
        "L.l2": box_glyph(100, 100, 400, 400),
        "L.l3": box_glyph(200, 200, 300, 300),
        "M": box_glyph(0, 0, 500, 500),
    }
    fb = base_font(glyphs, None)
    colr = {
        "E": box_layer("E", 0),
        "F": (ot.PaintFormat.PaintGlyph,
              (ot.PaintFormat.PaintLinearGradient,
               (0, [(0.0, 0, 1.0), (1.0, 2, 1.0)]),
               0, 0, 500, 0, 0, 500),
              "F"),
        "G": (ot.PaintFormat.PaintGlyph,
              (ot.PaintFormat.PaintRadialGradient,
               (0, [(0.0, 3, 1.0), (1.0, 4, 0.0)]),
               250, 250, 0, 250, 250, 250),
              "G"),
        "H": (ot.PaintFormat.PaintGlyph,
              (ot.PaintFormat.PaintSweepGradient,
               (0, [(0.0, 0, 1.0), (1.0, 2, 1.0)]),
               250, 250, 0.0, 180.0),
              "H"),
        "I": (ot.PaintFormat.PaintRotateAroundCenter,
              box_layer("I", 1), 90.0, 250, 250),
        "J": (ot.PaintFormat.PaintComposite,
              box_layer("J.src", 0),
              ot.CompositeMode.MULTIPLY,
              box_layer("J.bd", 5)),
        "K": (ot.PaintFormat.PaintTranslate,
              (ot.PaintFormat.PaintColrGlyph, "E"), 100, 0),
        "L": (ot.PaintFormat.PaintColrLayers,
              [box_layer("L.l1", 0),
               box_layer("L.l2", 1),
               box_layer("L.l3", 2)]),
        "M": (ot.PaintFormat.PaintGlyph,
              (ot.PaintFormat.PaintVarSolid, 3, 1.0, 0),
              "M"),
    }
    fb.font["COLR"] = buildCOLR(colr, version=1)
    fb.font["CPAL"] = buildCPAL([[
        (1.0, 0.0, 0.0, 1.0),   # 0: red
        (0.0, 1.0, 0.0, 1.0),   # 1: green
        (0.0, 0.0, 1.0, 1.0),   # 2: blue
        (1.0, 1.0, 0.0, 1.0),   # 3: yellow
        (1.0, 1.0, 0.0, 0.0),   # 4: yellow, transparent
        (0.0, 0.0, 1.0, 0.5),   # 5: blue, half alpha
    ]])
    path = os.path.join(OUT, "colr1.ttf")
    fb.save(path)
    print("wrote", path)


def png_bytes(w, h, color):
    r, g, b, a = color
    raw = b""
    for _ in range(h):
        raw += b"\x00" + bytes([r, g, b, a]) * w
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def build_cbdt():
    """A bitmap-only face: NO glyf/loca at all. Two strikes (32 and
    64 ppem); the 32-ppem strike uses index subtable format 1, the
    64-ppem strike format 4 (sparse). Image format 17 throughout."""
    glyph_order = [".notdef", "E", "F", "G"]
    glyph_ids = {name: i for i, name in enumerate(glyph_order)}

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(glyph_order)
    fb.setupCharacterMap({0x45: "E", 0x46: "F", 0x47: "G"})
    # Empty outlines first (fontBuilder's TTF path requires glyf) —
    # stripped after the save/reload below.
    fb.setupGlyf({name: box_glyph(0, 0, 0, 0) for name in glyph_order})
    fb.setupHorizontalMetrics({name: (600, 0) for name in glyph_order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "FDK CBDT Test",
                       "styleName": "Regular",
                       "psName": "FDKCBDTTest"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200,
                usWinAscent=800, usWinDescent=200)
    tmp = os.path.join(OUT, "_cbdt_tmp.ttf")
    fb.save(tmp)

    from fontTools.ttLib import TTFont, newTable
    f = TTFont(tmp)

    E_PNG = png_bytes(16, 16, (255, 0, 0, 255))
    F_PNG = png_bytes(16, 16, (0, 0, 255, 255))
    G_PNG = png_bytes(24, 24, (0, 200, 0, 128))
    E_BIG = png_bytes(32, 32, (200, 0, 0, 255))

    def fmt17(png, h, w):
        # SmallGlyphMetrics: height, width, bearingX, bearingY,
        # advance — all five bytes, or fontTools' reference reader
        # picks the data length up from the wrong offset.
        return (bytes([h, w, 0, h, w]) +
                struct.pack(">I", len(png)) + png)

    imgs32 = [fmt17(E_PNG, 16, 16), fmt17(F_PNG, 16, 16),
              fmt17(G_PNG, 24, 24)]
    offs32 = [0]
    for p in imgs32:
        offs32.append(offs32[-1] + len(p))

    cbd = b"\x00\x03\x00\x00"  # CBDT version 3.0
    strike1_img_start = 4       # imageDataOffset, relative to CBDT start
    cbd += b"".join(imgs32)
    strike2_img_start = len(cbd)
    img64 = fmt17(E_BIG, 32, 32)  # strike 2's image is ALSO a
                                   # format-17 record (imageFormat 17)
    cbd += img64

    sub0 = struct.pack(">HHL", 1, 17, strike1_img_start)
    for o in offs32:
        sub0 += struct.pack(">I", o)
    # Index subtable format 4: u32 numGlyphs, then numGlyphs+1
    # (u16 glyphId, u16 offset) entries — the last is the end-offset
    # sentinel (its glyphId is ignored). One trailing pad pair keeps
    # fontTools' reference reader (which over-reads by one pair)
    # in-bounds.
    sub1 = struct.pack(">HHL", 4, 17, strike2_img_start)
    sub1 += struct.pack(">I", 1)                       # numGlyphs
    sub1 += struct.pack(">HH", glyph_ids["E"], 0)      # E -> 0
    sub1 += struct.pack(">HH", 0, len(img64))          # sentinel
    sub1 += struct.pack(">HH", 0, len(img64))          # reader pad

    index_array0_off = 104
    index_array1_off = 112
    sub0_off = 120
    sub1_off = sub0_off + len(sub0)

    def size_table(index_array_off, n_subs, start_glyph, end_glyph, ppem):
        part1 = struct.pack(">IIII", index_array_off, 0, n_subs, 0)
        line = bytes([8, 254, 20, 1, 0, 0, 0, 0, 0, 0, 0, 0])
        part2 = struct.pack(">HHBBBB", start_glyph, end_glyph, ppem, ppem,
                            32, 1)
        return part1 + line + line + part2

    cblc = b"\x00\x03\x00\x00" + struct.pack(">I", 2)
    cblc += size_table(index_array0_off, 1, 1, 3, 32)
    cblc += size_table(index_array1_off, 1, 1, 1, 64)
    cblc += struct.pack(">HHL", 1, 3, sub0_off - index_array0_off)
    cblc += struct.pack(">HHL", 1, 1, sub1_off - index_array1_off)
    assert len(cblc) == sub0_off, (len(cblc), sub0_off)
    cblc += sub0
    cblc += sub1

    cblc_t = newTable("CBLC")
    cblc_t.decompile(cblc, f)
    f["CBLC"] = cblc_t  # CBDT's decompile reads the locator from the font
    cbdt_t = newTable("CBDT")
    cbdt_t.decompile(cbd, f)
    f["CBDT"] = cbdt_t
    del f["glyf"]
    del f["loca"]
    path = os.path.join(OUT, "cbdt.ttf")
    f.save(path)
    os.remove(tmp)
    print("wrote", path)


if __name__ == "__main__":
    build_colr0()
    build_colr1()
    build_cbdt()
