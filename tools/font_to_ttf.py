#!/usr/bin/env python3
"""Convert the game's overlay font into an installable TrueType font.

The font ships as an A8 atlas plus a metrics table (src/robox_font_a8.h and
src/robox_font_metrics.h, extracted from the game's title.brfna by
tools/extract_brfna.py). That is fine for the renderer and useless everywhere
else, so this rebuilds it as a .ttf you can install and type with.

The face is geometric -- 3px strokes on a pixel grid -- so the outlines here
follow pixel boundaries exactly rather than being smoothed. Tracing a curve
through it would invent detail the original does not have and soften the
corners that give it its look. Straight edges are then merged, so a 20px-wide
glyph ends up with a few dozen points rather than hundreds.

    python tools/font_to_ttf.py [out.ttf]

NOTE: the source is retail game data. A font built from it is game data too --
fine to install for yourself, not something to redistribute.
"""
import re
import sys

import numpy as np
from PIL import Image
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

ATLAS_H  = "src/robox_font_a8.h"
METRIC_H = "src/robox_font_metrics.h"

# Alpha above this counts as ink. The face is antialiased with a single
# half-intensity edge pixel; 128 is the midpoint, which keeps the 3px strokes
# 3px instead of fattening every one of them by a pixel.
THRESHOLD = 128

# Trace at this multiple of the source resolution.
#
# Nearly every glyph is axis-aligned, and for those the threshold is lossless:
# upscaling a hard edge and cutting at 50% puts the boundary back exactly where
# it was. 'x' and 'k' are the exceptions -- the only true diagonals in the face,
# drawn with graduated alpha where the sub-pixel edge position lives ENTIRELY in
# that antialiasing. Thresholding at source resolution throws it away and leaves
# a coarse 1px staircase, which is what made those two look wrong.
#
# Upscaling bilinearly first recovers it: the ramp becomes a finer staircase,
# which SIMPLIFY_TOL below then straightens back into an actual diagonal.
SUPERSAMPLE = 4

# Douglas-Peucker tolerance, in source pixels. Small enough that a real corner
# is never rounded off, large enough to collapse a supersampled staircase into
# the straight line it is approximating.
SIMPLIFY_TOL = 0.34

UPEM = 2048


def load_atlas():
    src = open(ATLAS_H, encoding="utf-8", errors="replace").read()
    w = int(re.search(r"robox_font_w\s*=\s*(\d+)", src).group(1))
    h = int(re.search(r"robox_font_h\s*=\s*(\d+)", src).group(1))
    body = src[src.index("{") + 1: src.rindex("}")]
    data = np.fromiter((int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", body)),
                       dtype=np.uint8, count=w * h)
    return data.reshape(h, w)


def load_metrics():
    src = open(METRIC_H, encoding="utf-8", errors="replace").read()
    rows = re.findall(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),"
                      r"\s*(-?\d+),\s*(-?\d+)\s*\}", src)
    return [[int(v) for v in r] for r in rows]


def trace(mask):
    """Pixel-boundary contours of a binary mask, as lists of (x, y) in pixels.

    Every filled pixel contributes the edges of its own square that face an
    empty neighbour, directed so that ink stays on the left. Chaining those
    head to tail closes them into loops, and holes come out wound opposite to
    the outlines that contain them -- which is exactly what a non-zero fill
    needs, with no point-in-polygon test anywhere.
    """
    h, w = mask.shape
    edges = {}
    for y in range(h):
        for x in range(w):
            if not mask[y, x]:
                continue
            if y == 0     or not mask[y - 1, x]: edges[(x, y)]         = (x + 1, y)
            if x == w - 1 or not mask[y, x + 1]: edges[(x + 1, y)]     = (x + 1, y + 1)
            if y == h - 1 or not mask[y + 1, x]: edges[(x + 1, y + 1)] = (x, y + 1)
            if x == 0     or not mask[y, x - 1]: edges[(x, y + 1)]     = (x, y)

    contours = []
    while edges:
        start = next(iter(edges))
        pt = start
        loop = []
        while pt in edges:
            loop.append(pt)
            nxt = edges.pop(pt)
            pt = nxt
            if pt == start:
                break
        if len(loop) >= 4:
            contours.append(loop)
    return contours


def drop_collinear(points):
    """Merge runs of points on the same straight edge."""
    out = []
    n = len(points)
    for i in range(n):
        ax, ay = points[i - 1]
        bx, by = points[i]
        cx, cy = points[(i + 1) % n]
        # keep b only if the direction changes there
        if (bx - ax) * (cy - by) != (by - ay) * (cx - bx):
            out.append((bx, by))
    return out


def _dp(points, tol):
    """Douglas-Peucker on an open run of points."""
    if len(points) < 3:
        return points
    ax, ay = points[0]
    bx, by = points[-1]
    dx, dy = bx - ax, by - ay
    span = (dx * dx + dy * dy) ** 0.5

    worst, at = -1.0, 0
    for i in range(1, len(points) - 1):
        px, py = points[i]
        if span == 0:
            d = ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
        else:
            d = abs(dy * px - dx * py + bx * ay - by * ax) / span
        if d > worst:
            worst, at = d, i

    if worst <= tol:
        return [points[0], points[-1]]
    return _dp(points[:at + 1], tol)[:-1] + _dp(points[at:], tol)


def simplify(points, tol):
    """Douglas-Peucker around a closed loop.

    Split at the two most distant points first so the loop is reduced as two
    open runs. Running it on a closed ring from an arbitrary start would let
    the seam wander, which on a glyph shows up as one corner quietly losing its
    square edge.
    """
    n = len(points)
    if n < 4:
        return points
    ax, ay = points[0]
    far = max(range(n), key=lambda i: (points[i][0] - ax) ** 2 + (points[i][1] - ay) ** 2)
    a = _dp(points[:far + 1], tol)
    b = _dp(points[far:] + [points[0]], tol)
    return (a[:-1] + b[:-1]) or points


def build(out_path):
    atlas = load_atlas()
    metrics = load_metrics()

    live = [(cp, m) for cp, m in enumerate(metrics) if m[5] > 0 and cp < 128]
    if not live:
        sys.exit("no glyphs with a positive advance -- wrong metrics file?")

    cell_h = max(m[3] for _, m in live)
    cell_w = max(m[2] for _, m in live)

    # Baseline: the row most glyphs' ink rests on. Taking the mode of each
    # glyph's lowest inked row finds it without hardcoding, and descenders
    # (which are a minority) do not drag it down.
    bottoms = []
    for cp, (x, y, gw, gh, yo, adv) in live:
        cell = atlas[y:y + cell_h, x:x + cell_w] > THRESHOLD
        rows = np.nonzero(cell.any(axis=1))[0]
        if rows.size:
            bottoms.append(int(rows[-1]) + 1)
    baseline = int(np.bincount(bottoms).argmax())

    scale = UPEM / cell_h

    def px(v):
        return int(round(v * scale))

    glyph_order = [".notdef"]
    glyphs, widths, cmap = {}, {}, {}

    pen = TTGlyphPen(None)
    glyphs[".notdef"] = pen.glyph()
    widths[".notdef"] = px(cell_h // 2)

    for cp, (x, y, gw, gh, yo, adv) in live:
        name = f"uni{cp:04X}"
        glyph_order.append(name)
        cmap[cp] = name
        widths[name] = px(adv)

        alpha = atlas[y:y + cell_h, x:x + cell_w]
        big = np.asarray(Image.fromarray(alpha).resize(
            (cell_w * SUPERSAMPLE, cell_h * SUPERSAMPLE), Image.BILINEAR))
        cell = big > THRESHOLD

        pen = TTGlyphPen(None)
        for contour in trace(cell):
            pts = simplify(drop_collinear(contour), SIMPLIFY_TOL * SUPERSAMPLE)
            if len(pts) < 3:
                continue
            # image space is y-down from the cell top; font space is y-up from
            # the baseline. Coordinates are in supersampled pixels here.
            fp = [(px(cx / SUPERSAMPLE), px(baseline - cy / SUPERSAMPLE))
                  for cx, cy in pts]
            pen.moveTo(fp[0])
            for p in fp[1:]:
                pen.lineTo(p)
            pen.closePath()
        glyphs[name] = pen.glyph()

    ascent  = px(baseline)
    descent = px(baseline - cell_h)      # negative

    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(glyph_order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({g: (widths[g], 0) for g in glyph_order})
    fb.setupHorizontalHeader(ascent=ascent, descent=descent)
    fb.setupNameTable({
        "familyName":   "Robox",
        "styleName":    "Regular",
        "psName":       "Robox-Regular",
        "fullName":     "Robox Regular",
        "version":      "Version 1.000",
        "uniqueFontIdentifier": "Robox-Regular;1.000",
        "copyright":    "Traced from the Robox (Dreambox Games, 2010) title "
                        "font. Game data -- not for redistribution.",
    })
    fb.setupOS2(sTypoAscender=ascent, sTypoDescender=descent,
                usWinAscent=ascent, usWinDescent=-descent)
    fb.setupPost()
    fb.save(out_path)

    print(f"{out_path}")
    print(f"  glyphs   : {len(live)} (U+{min(c for c,_ in live):04X}"
          f"..U+{max(c for c,_ in live):04X})")
    print(f"  cell     : {cell_w}x{cell_h} px, baseline at row {baseline}")
    print(f"  upem     : {UPEM}  (1 px = {scale:.1f} units)")
    print(f"  ascent   : {ascent}   descent: {descent}")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "Robox-Regular.ttf")
