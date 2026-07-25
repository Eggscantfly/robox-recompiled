#!/usr/bin/env python3
"""Bake the splash-screen images into a raw RGBA format the port can load.

The splash is the animation described by build/Assets/generate_mp4.py (and its
web twin index.html). Rather than ship an MP4 -- which would need an H.264
decoder on Windows and a second one for the browser -- the port replays the
animation natively. That needs only the still images, and it renders at the
window's real resolution instead of a fixed 1080p, so it stays sharp.

The engine has no PNG decoder (and adding one means dragging in zlib on two
platforms), so the images are pre-decoded here into a trivial container:

    magic  "RSPL"      4 bytes
    width  uint32 LE
    height uint32 LE
    pixels width*height*4 bytes, RGBA8, straight (non-premultiplied) alpha

Heights match generate_mp4.py's 1080p layout (432 / 108 / 108). The renderer
scales that layout to whatever the window actually is, so these are a quality
ceiling, not a fixed size.

Usage:  python tools/make_splash_assets.py
"""

import pathlib
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("needs Pillow:  pip install Pillow", file=sys.stderr)
    raise SystemExit(1)

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "build" / "Assets"
DST = ROOT / "splash"

# (source png, output name, target height) -- heights from generate_mp4.py.
IMAGES = [
    ("Eggscantfly.png",      "logo.rspl", 432),
    ("Recompiled By.png",    "by.rspl",   108),
    ("Eggscantfly text.png", "name.rspl", 108),
]

# Played by the animation; copied verbatim and loaded with SDL_LoadWAV.
SOUNDS = ["SLap quack.wav", "BAss.wav"]


def bake(src: pathlib.Path, dst: pathlib.Path, target_h: int) -> None:
    img = Image.open(src).convert("RGBA")
    w, h = img.size
    scale = target_h / h
    img = img.resize((max(1, round(w * scale)), target_h), Image.Resampling.LANCZOS)
    w, h = img.size
    dst.write_bytes(b"RSPL" + struct.pack("<II", w, h) + img.tobytes())
    print(f"  {dst.name:12s} {w}x{h}  ({dst.stat().st_size // 1024} KB)")


def main() -> int:
    if not SRC.is_dir():
        print(f"missing source directory: {SRC}", file=sys.stderr)
        return 1
    DST.mkdir(parents=True, exist_ok=True)

    for png, out, target_h in IMAGES:
        p = SRC / png
        if not p.is_file():
            print(f"missing {p}", file=sys.stderr)
            return 1
        bake(p, DST / out, target_h)

    for wav in SOUNDS:
        p = SRC / wav
        if not p.is_file():
            print(f"missing {p}", file=sys.stderr)
            return 1
        out = DST / wav.replace(" ", "_")
        out.write_bytes(p.read_bytes())
        print(f"  {out.name:12s} ({out.stat().st_size // 1024} KB)")

    print(f"wrote {DST}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
