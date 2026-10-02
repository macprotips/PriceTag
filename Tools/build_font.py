#!/usr/bin/env python3
"""Builds "Mint Display", PriceTag's own heavy display font for prices.

Every glyph is drawn as a centerline skeleton, stroked heavy, merged, clipped
to flat terminals and then corner-rounded, so the whole set shares one weight
and one softness. Only the characters a price needs are included:

    0-9  $  .  ,  -  +  space

Usage:
    pip install fonttools skia-pathops
    python3 Tools/build_font.py

Writes Font/MintDisplay.otf (installable copy) and
Sources/PriceTagKit/MintDisplayFontData.swift (the same font embedded in the
app, so it never depends on a resource bundle).
"""

import math
import os
import sys

import pathops
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen

UPM = 1000
CAP = 740           # height of digits
W = 166             # main stroke weight
H = W / 2           # half weight: centerlines sit this far inside the box
TOP = CAP - H       # centerline top
BOT = H             # centerline bottom
MID = CAP / 2
SIDE = 34           # side bearing on each side of a glyph
ROUND = 16          # corner rounding radius
KAPPA = 0.5523
XS = 1.14          # horizontal stretch applied to skeletons before stroking

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "Font", "MintDisplay.otf")
SWIFT_OUT = os.path.join(ROOT, "Sources", "PriceTagKit", "MintDisplayFontData.swift")


# ---------------------------------------------------------------- geometry

class Sketch:
    """A centerline built from lines, cubics and elliptical arcs."""

    def __init__(self):
        self.path = pathops.Path()
        self.cur = None

    def M(self, x, y):
        self.path.moveTo(x, y)
        self.cur = (x, y)
        return self

    def L(self, x, y):
        self.path.lineTo(x, y)
        self.cur = (x, y)
        return self

    def C(self, x1, y1, x2, y2, x, y):
        self.path.cubicTo(x1, y1, x2, y2, x, y)
        self.cur = (x, y)
        return self

    def arc(self, cx, cy, rx, ry, a0, a1):
        """Elliptical arc from angle a0 to a1 (degrees, y-up, CCW positive).
        Moves to the start point if no point is current yet."""
        p0 = (cx + rx * math.cos(math.radians(a0)), cy + ry * math.sin(math.radians(a0)))
        if self.cur is None:
            self.M(*p0)
        elif abs(self.cur[0] - p0[0]) > 0.5 or abs(self.cur[1] - p0[1]) > 0.5:
            self.L(*p0)
        steps = max(1, math.ceil(abs(a1 - a0) / 90))
        da = (a1 - a0) / steps
        for i in range(steps):
            t0 = math.radians(a0 + da * i)
            t1 = math.radians(a0 + da * (i + 1))
            k = 4 / 3 * math.tan((t1 - t0) / 4)
            x0, y0 = math.cos(t0), math.sin(t0)
            x3, y3 = math.cos(t1), math.sin(t1)
            self.C(cx + rx * (x0 - k * y0), cy + ry * (y0 + k * x0),
                   cx + rx * (x3 + k * y3), cy + ry * (y3 - k * x3),
                   cx + rx * x3, cy + ry * y3)
        return self

    def squircle(self, cx, cy, rx, ry, k=0.66):
        """Closed super-ellipse-ish loop (fuller than an ellipse)."""
        self.M(cx + rx, cy)
        self.C(cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry)
        self.C(cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy)
        self.C(cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry)
        self.C(cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy)
        self.path.close()
        self.cur = None
        return self

    def close(self):
        self.path.close()
        self.cur = None
        return self


def stroked(sketch, width=W, cap=pathops.LineCap.ROUND_CAP,
            join=pathops.LineJoin.ROUND_JOIN, xs=XS):
    p = pathops.Path()
    p.addPath(sketch.path)
    p = p.transform(xs, 0, 0, 1, 0, 0)
    p.stroke(width, cap, join, 4)
    p.convertConicsToQuads()
    return simplify(p)


def simplify(p):
    out = pathops.Path()
    out.addPath(p)
    out.simplify(fix_winding=True)
    if out.clockwise:
        out.reverse()   # keep one orientation so boolean ops never subtract
    return out


def union(*paths):
    acc = pathops.Path()
    for p in paths:
        acc = pathops.op(acc, simplify(p), pathops.PathOp.UNION, fix_winding=True)
    return simplify(acc)


def rect(x0, y0, x1, y1):
    p = pathops.Path()
    p.moveTo(x0, y0)
    p.lineTo(x1, y0)
    p.lineTo(x1, y1)
    p.lineTo(x0, y1)
    p.close()
    return p


def clip(p, y0=0, y1=CAP):
    return pathops.op(p, rect(-2000, y0, 4000, y1), pathops.PathOp.INTERSECTION,
                      fix_winding=True)


def outline_band(p, width, join=pathops.LineJoin.ROUND_JOIN):
    band = pathops.Path()
    band.addPath(p)
    band.stroke(width, pathops.LineCap.ROUND_CAP, join, 4)
    band.convertConicsToQuads()
    return simplify(band)


def soften(p, r=ROUND):
    """Rounds convex corners (open) and concave corners (close) with radius r."""
    # opening: erode then dilate -> rounds outer corners
    eroded = pathops.op(p, outline_band(p, 2 * r), pathops.PathOp.DIFFERENCE, fix_winding=True)
    opened = union(eroded, outline_band(eroded, 2 * r))
    # closing: dilate then erode -> rounds inner corners
    dilated = union(opened, outline_band(opened, 2 * r * 0.6))
    closed = pathops.op(dilated, outline_band(dilated, 2 * r * 0.6),
                        pathops.PathOp.DIFFERENCE, fix_winding=True)
    return closed


def mirror(p, cx, cy):
    """Rotate a path 180 degrees around (cx, cy)."""
    out = pathops.Path()
    out.addPath(p)
    return simplify(out.transform(-1, 0, 0, -1, 2 * cx, 2 * cy))


# ---------------------------------------------------------------- glyphs

BUTT = pathops.LineCap.BUTT_CAP
MITER = pathops.LineJoin.MITER_JOIN


def g_zero():
    return stroked(Sketch().squircle(165, MID, 165, MID - H, k=0.70))


def g_one():
    s = Sketch().M(30, 548).L(150, TOP).L(150, -50)
    return clip(stroked(s, join=MITER, cap=BUTT))


def g_two():
    s = Sketch().arc(160, 472, 152, TOP - 472, 168, 12)
    s.C(305, 330, 190, 250, 22, BOT).L(330, BOT)
    body = stroked(s, join=MITER)
    foot = stroked(Sketch().M(22, BOT).L(340, BOT), cap=BUTT)
    return union(body, foot)


def g_three():
    top_ry = (TOP - 375) / 2
    top_cy = TOP - top_ry
    bot_ry = (375 - BOT) / 2
    bot_cy = BOT + bot_ry
    a = Sketch().arc(158, top_cy, 148, top_ry, 158, -90).L(105, 375)
    b = Sketch().M(98, 375).arc(165, bot_cy, 165, bot_ry, 90, -158)
    return union(stroked(a), stroked(b))


def g_four():
    s = Sketch().M(352, 205).L(4, 205).L(212, TOP + 30)
    stem = Sketch().M(262, -40).L(262, TOP + 120)
    return clip(union(stroked(s, join=MITER), stroked(stem, cap=BUTT)))


def g_five():
    s = Sketch().M(318, TOP).L(48, TOP).L(34, 370)
    s.C(80, 398, 130, 410, 178, 410)
    s.C(272, 410, 338, 338, 338, 248)
    s.arc(172, 248, 166, 248 - BOT, 0, -158)
    return stroked(s, join=MITER)


def g_six():
    bowl = Sketch().squircle(168, 238, 168, 238 - BOT, k=0.62)
    stem = Sketch().M(2, 238).C(2, 470, 110, TOP, 270, TOP)
    return union(stroked(bowl), stroked(stem))


def g_seven():
    s = Sketch().M(0, TOP).L(322, TOP).C(220, 480, 150, 260, 120, -60)
    return clip(stroked(s, join=MITER))


def g_eight():
    split = 395
    bot = Sketch().squircle(168, (split + BOT) / 2, 168, (split - BOT) / 2, k=0.62)
    top = Sketch().squircle(168, (TOP + split) / 2, 145, (TOP - split) / 2, k=0.62)
    return union(stroked(bot), stroked(top))


def g_nine():
    return mirror(g_six(), 168, MID)


def g_dollar():
    split = 368
    top_ry = (TOP - split) / 2 + 6
    bot_ry = (split - BOT) / 2 + 6
    s = Sketch().arc(160, split + top_ry, 148, top_ry, 18, 270)
    s.arc(160, split - bot_ry, 152, bot_ry, 90, -160)
    bar = Sketch().M(160, -95).L(160, CAP + 95)
    return clip(union(stroked(s), stroked(bar, width=W * 0.62, cap=BUTT)), -95, CAP + 95)


def g_period():
    r = W * 0.64
    return stroked(Sketch().squircle(r, r, r * 0.5, r * 0.5, k=0.56), width=r, xs=1)


def g_comma():
    r = W * 0.64
    dot = stroked(Sketch().squircle(r, r, r * 0.5, r * 0.5, k=0.56), width=r, xs=1)
    tail = Sketch().M(r + 38, r - 10).C(r + 38, -10, r, -90, r - 50, -150)
    return union(dot, stroked(tail, width=W * 0.7, xs=1))


def g_minus():
    return stroked(Sketch().M(0, MID - 10).L(270, MID - 10), cap=BUTT, width=W * 0.9)


def g_plus():
    arm = 185
    h = stroked(Sketch().M(0, MID - 10).L(2 * arm, MID - 10), cap=BUTT, width=W * 0.9, xs=1)
    v = stroked(Sketch().M(arm, MID - 10 - arm).L(arm, MID - 10 + arm), cap=BUTT,
                width=W * 0.9, xs=1)
    return union(h, v)


GLYPHS = {
    "zero": ("0", g_zero), "one": ("1", g_one), "two": ("2", g_two),
    "three": ("3", g_three), "four": ("4", g_four), "five": ("5", g_five),
    "six": ("6", g_six), "seven": ("7", g_seven), "eight": ("8", g_eight),
    "nine": ("nine", g_nine), "dollar": ("$", g_dollar), "period": (".", g_period),
    "comma": (",", g_comma), "hyphen": ("-", g_minus), "plus": ("+", g_plus),
}
GLYPHS["nine"] = ("9", g_nine)

# Tighter spacing next to punctuation looks better at display sizes.
SIDE_OVERRIDE = {"period": 22, "comma": 22, "one": 26, "hyphen": 16, "plus": 20}
SPACE_WIDTH = 230


def build():
    order = [".notdef", "space"] + list(GLYPHS)
    cmap = {ord(" "): "space"}
    charstrings, metrics = {}, {}

    empty = T2CharStringPen(0, None)
    charstrings[".notdef"] = empty.getCharString()
    metrics[".notdef"] = (SPACE_WIDTH, 0)
    pen = T2CharStringPen(SPACE_WIDTH, None)
    charstrings["space"] = pen.getCharString()
    metrics["space"] = (SPACE_WIDTH, 0)

    for name, (char, fn) in GLYPHS.items():
        path = soften(simplify(fn()))
        xmin, _, xmax, _ = path.bounds
        side = SIDE_OVERRIDE.get(name, SIDE)
        dx = side - xmin
        path = simplify(path.transform(1, 0, 0, 1, dx, 0))
        adv = round(xmax - xmin + 2 * side)
        pen = T2CharStringPen(adv, None)
        path.draw(_RoundingPen(pen))
        charstrings[name] = pen.getCharString()
        metrics[name] = (adv, round(path.bounds[0]))
        cmap[ord(char)] = name

    fb = FontBuilder(UPM, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupCFF("MintDisplay-Heavy", {"FullName": "Mint Display Heavy"},
                charstrings, {})
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=CAP + 120, descent=-200)
    fb.setupNameTable({
        "familyName": "Mint Display",
        "styleName": "Heavy",
        "uniqueFontIdentifier": "PriceTag:MintDisplay-Heavy:1.0",
        "fullName": "Mint Display Heavy",
        "psName": "MintDisplay-Heavy",
        "version": "Version 1.000",
        "copyright": "Copyright (c) 2026 PriceTag",
        "designer": "PriceTag",
    })
    fb.setupOS2(sTypoAscender=CAP + 120, sTypoDescender=-200, sTypoLineGap=0,
                usWinAscent=CAP + 220, usWinDescent=250, sCapHeight=CAP,
                sxHeight=CAP, usWeightClass=900, achVendID="PTAG")
    fb.setupPost()
    fb.setupHead(unitsPerEm=UPM, fontRevision=1.0)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fb.save(OUT)
    print("wrote", os.path.normpath(OUT))
    write_swift(OUT)


def write_swift(otf_path):
    import base64
    import textwrap
    data = base64.b64encode(open(otf_path, "rb").read()).decode()
    lines = "\n".join("    " + l for l in textwrap.wrap(data, 100))
    with open(SWIFT_OUT, "w") as f:
        f.write("// Generated by Tools/build_font.py. Do not edit by hand.\n")
        f.write("// Mint Display Heavy, PriceTag's own price font, embedded as base64.\n\n")
        f.write("enum MintDisplayFontData {\n")
        f.write('    static let base64 = """\n')
        f.write(lines + "\n")
        f.write('    """\n}\n')
    print("wrote", os.path.normpath(SWIFT_OUT))


class _RoundingPen:
    """Rounds coordinates to integers while forwarding to another pen."""

    def __init__(self, pen):
        self.pen = pen

    def _r(self, pts):
        return [(round(x), round(y)) for x, y in pts]

    def moveTo(self, p):
        self.pen.moveTo(*self._r([p]))

    def lineTo(self, p):
        self.pen.lineTo(*self._r([p]))

    def curveTo(self, *pts):
        self.pen.curveTo(*self._r(pts))

    def qCurveTo(self, *pts):
        self.pen.qCurveTo(*self._r(pts))

    def closePath(self):
        self.pen.closePath()

    def endPath(self):
        self.pen.endPath()


if __name__ == "__main__":
    build()
