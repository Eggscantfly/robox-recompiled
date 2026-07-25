#!/usr/bin/env python3
"""Extract a Nintendo BRFNA (Revolution font archive) into C headers.

Ported from the Switch-Toolbox C# reference, which is the only authoritative
description of this format I could find:
    File_Format_Library/FileFormats/Font/BXFNT/{BXFNT,FINF,TGLP,CWDH,GLGR}.cs
    Switch_Toolbox_Library/Compression/Huffman_WII.cs
    Switch_Toolbox_Library/Texture Decoding/Gamecube/Decode_Gamecube.cs

Two things here are easy to get wrong and produce output that looks like a
rendering bug rather than a decode bug:

  * The TGLP sheet is Huffman-compressed (Nintendo type 0x2n) even though
    nothing in the TGLP header says so. The giveaway is that TGLP.SheetSize
    (131072 for a 512x512 I4 sheet) is far larger than the bytes actually
    present in the section. The compressed blob starts after a u32 length.

  * The Huffman bitstream is read as 32-bit words, and for this file those
    words are LITTLE-endian even though every other field in the file is
    big-endian. Decoding big-endian yields 131072 plausible-looking bytes that
    render as pure noise, so "it produced the right number of bytes" proves
    nothing -- look at the image.

Usage:
    python tools/extract_brfna.py Assets/fonts/title.brfna src/robox_font
"""

import os
import struct
import sys
import zlib

sys.setrecursionlimit(20000)


# --- container ------------------------------------------------------------

def sections(buf):
    """Yield (magic, offset, size) for each top-level section."""
    magic, _bom, _ver, size, hdr, count = struct.unpack('>4sHHIHH', buf[:16])
    if magic not in (b'RFNA', b'RFNT'):
        raise SystemExit('not a BRFNA/BRFNT file: %r' % magic)
    if size != len(buf):
        print('  warning: header size %d != file size %d' % (size, len(buf)))
    off = hdr
    for _ in range(count):
        m, s = struct.unpack('>4sI', buf[off:off + 8])
        yield m, off, s
        off += s


def read_finf(buf, off):
    """FINF gives the absolute offsets of the other sections (minus 8)."""
    p = off + 8
    _type, _linefeed, _alter = buf[p], buf[p + 1], struct.unpack('>H', buf[p + 2:p + 4])[0]
    p += 4
    _dl, _dg, _dc, _enc = buf[p], buf[p + 1], buf[p + 2], buf[p + 3]
    p += 4
    tglp, cwdh, cmap = struct.unpack('>III', buf[p:p + 12])
    p += 12
    height, width, ascent = buf[p], buf[p + 1], buf[p + 2]
    return dict(tglp=tglp - 8, cwdh=cwdh - 8, cmap=cmap - 8,
                height=height, width=width, ascent=ascent)


def read_tglp(buf, off):
    """RFNA variant: an extra byte sits before the 1-byte format field."""
    p = off + 8
    cell_w, cell_h, baseline, max_w = buf[p:p + 4]
    p += 4
    sheet_size, = struct.unpack('>I', buf[p:p + 4]); p += 4
    sheet_count, = struct.unpack('>H', buf[p:p + 2]); p += 2
    _unk, fmt = buf[p], buf[p + 1]; p += 2
    cols, rows, sw, sh = struct.unpack('>HHHH', buf[p:p + 8]); p += 8
    return dict(cell_w=cell_w, cell_h=cell_h, baseline=baseline, max_w=max_w,
                sheet_size=sheet_size, sheet_count=sheet_count, fmt=fmt,
                cols=cols, rows=rows, sheet_w=sw, sheet_h=sh, data=p)


def read_cwdh(buf, off):
    """{left, glyph_width, char_width} per glyph, over an index range."""
    p = off + 8
    start, end = struct.unpack('>HH', buf[p:p + 4]); p += 4
    _next, = struct.unpack('>I', buf[p:p + 4]); p += 4
    out = {}
    for i in range(start, end + 1):
        left = struct.unpack('>b', buf[p:p + 1])[0]     # signed
        out[i] = (left, buf[p + 1], buf[p + 2])
        p += 3
    return out


def read_cmap(buf, off, out):
    """Codepoint -> glyph index. Three mapping methods; follows the chain."""
    while off:
        p = off + 8
        lo, hi, method = struct.unpack('>HHH', buf[p:p + 6]); p += 6
        p += 2                                            # padding
        nxt, = struct.unpack('>I', buf[p:p + 4]); p += 4
        if method == 0:                                   # direct
            first, = struct.unpack('>H', buf[p:p + 2])
            for c in range(lo, hi + 1):
                out[c] = first + (c - lo)
        elif method == 1:                                 # table
            for n, c in enumerate(range(lo, hi + 1)):
                idx, = struct.unpack('>H', buf[p + n * 2:p + n * 2 + 2])
                if idx != 0xFFFF:
                    out[c] = idx
        elif method == 2:                                 # scan
            count, = struct.unpack('>H', buf[p:p + 2]); p += 2
            for n in range(count):
                c, idx = struct.unpack('>HH', buf[p + n * 4:p + n * 4 + 4])
                if idx != 0xFFFF:
                    out[c] = idx
        else:
            raise SystemExit('unknown CMAP method %d' % method)
        off = nxt - 8 if nxt else 0
    return out


# --- Nintendo Huffman (Huffman_WII.cs / dsdecmp) --------------------------

def _tree(buf, pos, maxin):
    b = buf[pos]
    off, end0, end1 = b & 0x3F, b & 0x80, b & 0x40
    base = (pos - (pos & 1)) + off * 2 + 2
    node = [None, None]
    if base < maxin:
        node[0] = ('d', buf[base]) if end0 else _tree(buf, base, maxin)
    if base + 1 < maxin:
        node[1] = ('d', buf[base + 1]) if end1 else _tree(buf, base + 1, maxin)
    return node


def huffman(buf, want):
    if (buf[0] & 0xF0) != 0x20:
        raise SystemExit('not Huffman-compressed: tag %#x' % buf[0])
    data_size = buf[0] & 0x0F
    if data_size != 8:
        raise SystemExit('only 8-bit Huffman handled here, got %d' % data_size)
    tree_size = buf[4]
    maxin = 4 + (tree_size + 1) * 2
    root = _tree(buf, 5, maxin)

    out = bytearray()
    node, i = root, maxin
    # '<I': see the module docstring -- the words are little-endian here even
    # though the container is big-endian. Bits run MSB-first within each word.
    while len(out) < want and i + 4 <= len(buf):
        word, = struct.unpack('<I', buf[i:i + 4]); i += 4
        for bit in range(31, -1, -1):
            nxt = node[(word >> bit) & 1]
            if nxt is None:
                node = root
                continue
            if nxt[0] == 'd':
                out.append(nxt[1])
                node = root
                if len(out) >= want:
                    break
            else:
                node = nxt
    return bytes(out)


# --- I4 (Decode_Gamecube.DecodeI4) ----------------------------------------

def decode_i4(buf, w, h):
    """4bpp greyscale in 8x8 blocks; high nibble is the left pixel."""
    out = bytearray(w * h)
    i = 0
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for py in range(by, by + 8):
                for px in range(bx, bx + 8, 2):
                    b = buf[i]; i += 1
                    if px < w and py < h:
                        out[py * w + px] = ((b >> 4) & 0xF) * 17
                    if px + 1 < w and py < h:
                        out[py * w + px + 1] = (b & 0xF) * 17
    return out


def write_png(path, w, h, gray):
    rows = b''.join(b'\x00' + bytes(gray[y * w:(y + 1) * w]) for y in range(h))
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(rows, 9)))
        f.write(chunk(b'IEND', b''))


# --- main -----------------------------------------------------------------

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'Assets/fonts/title.brfna'
    prefix = sys.argv[2] if len(sys.argv) > 2 else 'src/robox_font'
    buf = open(src, 'rb').read()

    secs = {m: (o, s) for m, o, s in sections(buf)}
    finf = read_finf(buf, secs[b'FINF'][0])
    tglp = read_tglp(buf, finf['tglp'])
    cwdh = read_cwdh(buf, finf['cwdh'])
    cmap = read_cmap(buf, finf['cmap'], {})

    print('%s: cell %dx%d  grid %dx%d  sheet %dx%d  fmt %d  baseline %d' % (
        os.path.basename(src), tglp['cell_w'], tglp['cell_h'],
        tglp['cols'], tglp['rows'], tglp['sheet_w'], tglp['sheet_h'],
        tglp['fmt'], tglp['baseline']))

    if tglp['fmt'] != 0:
        raise SystemExit('only I4 sheets handled; this one is format %d' % tglp['fmt'])

    # Indirection: the TGLP header ends with an ABSOLUTE file offset (the gap
    # between is zero padding to a 32-byte boundary). At that offset sits a u32
    # byte count, and the compressed stream follows it.
    p = tglp['data']
    blob_at, = struct.unpack('>I', buf[p:p + 4])
    blob_len, = struct.unpack('>I', buf[blob_at:blob_at + 4])
    comp = buf[blob_at + 4:blob_at + 4 + blob_len]
    sheet = huffman(comp, tglp['sheet_size'])
    if len(sheet) != tglp['sheet_size']:
        raise SystemExit('decompressed %d, expected %d' % (len(sheet), tglp['sheet_size']))

    w, h = tglp['sheet_w'], tglp['sheet_h']
    gray = decode_i4(sheet, w, h)

    # Cell grid. The header's cellWidth/cellHeight are the GLYPH BOX; the cells
    # are laid out with one pixel of padding before each, so the pitch is
    # cell+1 and the first cell starts at (1,1). Derived empirically by finding
    # the offsets at which every cell seam carries exactly zero ink -- assuming
    # a tight cell*N grid puts every glyph a row and a column out of place, and
    # the result still looks like a font, just an unreadable one.
    pitch_x, pitch_y = tglp['cell_w'] + 1, tglp['cell_h'] + 1
    org_x = org_y = 1

    # Metrics for printable ASCII only; the menu never draws anything else.
    metrics = [(0, 0, 0, 0, 0, 0)] * 256
    covered = []
    for code in range(32, 127):
        # The sheet holds two styles of every letter: a clean one and a
        # "boxed" one drawn knocked out of a filled rectangle. The font maps
        # UPPERCASE ASCII to the clean glyphs and lowercase to the boxed ones
        # -- the game writes its menus in caps and gets the lowercase-looking
        # clean forms. Nothing here wants the boxed style, so point a-z at the
        # A-Z glyphs and let callers write whichever case reads better.
        lookup = code - 32 if ord('a') <= code <= ord('z') else code
        idx = cmap.get(lookup)
        if idx is None:
            continue
        col, row = idx % tglp['cols'], idx // tglp['cols']
        if row >= tglp['rows']:
            continue
        _left, gw, adv = cwdh.get(idx, (0, tglp['cell_w'], tglp['cell_w']))
        # The quad is the whole cell so nothing is ever clipped; `advance` is
        # what the cursor moves by. They are different numbers, which is why
        # the renderer needed a separate advance field -- width + 1 would space
        # every glyph as if it were the widest in the font.
        metrics[code] = (org_x + col * pitch_x, org_y + row * pitch_y,
                         tglp['cell_w'], tglp['cell_h'], 0, max(adv, 1))
        if gw > 0:
            covered.append(chr(code))

    base = os.path.basename(prefix)
    with open(prefix + '_metrics.h', 'w') as f:
        f.write('/* Generated by tools/extract_brfna.py from %s -- do not edit. */\n'
                % os.path.basename(src))
        f.write('struct { int x, y, width, height, yOffset, advance; }'
                ' %s_metrics[256] = {\n' % base)
        for m in metrics:
            f.write('    { %d, %d, %d, %d, %d, %d },\n' % m)
        f.write('};\n')

    with open(prefix + '_a8.h', 'w') as f:
        f.write('/* Generated by tools/extract_brfna.py from %s -- do not edit.\n'
                ' * Single channel coverage; the text shader samples .r. */\n'
                % os.path.basename(src))
        f.write('const unsigned int %s_w = %d;\n' % (base, w))
        f.write('const unsigned int %s_h = %d;\n' % (base, h))
        f.write('const unsigned char %s_a8[] = {\n' % base)
        for i in range(0, len(gray), 32):
            f.write(','.join(str(v) for v in gray[i:i + 32]) + ',\n')
        f.write('};\n')

    print('  glyphs: %d printable ASCII' % len(covered))
    print('  lowercase: %s' % ('yes' if all(c in covered for c in 'abcdefghijklmnopqrstuvwxyz') else 'NO'))
    print('  wrote %s_metrics.h and %s_a8.h' % (prefix, prefix))
    return gray, w, h, metrics


if __name__ == '__main__':
    main()
