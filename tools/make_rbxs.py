#!/usr/bin/env python3
"""Build a .rbxs music file for the ROBOX Recomp music-packs mod.

A .rbxs is a small Robox sound header in front of an OGG Vorbis (or WAV)
payload. The header is what lets the mod loop with LOOP POINTS: the intro
before `loop start` plays once, then the loop region repeats forever --
something a bare WAV/OGG (which just loops whole) can't express.

Header layout (fixed 24 bytes, little-endian, ":P" always at offset 0xE):
   0x00  "RBXS"
   0x04  u32  sample rate   0 = use the audio's own rate
   0x08  u32  channels      1 = downmix to mono, else as-is
   0x0C  u16  loop flag     0 = play once, 1 = loop
   0x0E  ":P"               the header must smile
   0x10  u32  loop start    both points always present [frames];
   0x14  u32  loop end      ignored unless looping; 0,0 = whole song
   0x18  the OGG/WAV bytes, unchanged

Usage:
    make_rbxs.py song.ogg out.rbxs                  loop the whole song
    make_rbxs.py song.ogg out.rbxs --loop 88200 441000
                                                    intro once, then loop
                                                    frames 88200..441000
    make_rbxs.py song.ogg out.rbxs --once           play once, no loop
    make_rbxs.py song.ogg out.rbxs --mono           downmix to mono
    make_rbxs.py song.ogg out.rbxs --rate 32000     force playback rate
    make_rbxs.py song.brstm out.rbxs                BRSTM: transcoded to OGG
                                                    via ffmpeg, loop points
                                                    carried over automatically

Loop points are in FRAMES (samples per channel) -- what Audacity shows when
the selection format is set to samples. seconds * sample rate = frames.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys


def ogg_vorbis_rate(data: bytes):
    """Sample rate from the Vorbis identification header, for the hint."""
    i = data.find(b"\x01vorbis")
    if 0 <= i and i + 16 <= len(data):
        return struct.unpack_from("<I", data, i + 12)[0]
    return None


def wav_rate(data: bytes):
    i = data.find(b"fmt ")
    if 0 <= i and i + 16 <= len(data):
        return struct.unpack_from("<I", data, i + 12)[0]
    return None


def brstm_info(data: bytes):
    """(loop_flag, sample_rate, loop_start, total_samples) from a BRSTM HEAD."""
    head_off = struct.unpack_from(">I", data, 0x10)[0]
    p1 = struct.unpack_from(">I", data, head_off + 0x0C)[0] + head_off + 8
    loop_flag = data[p1 + 1]
    rate = struct.unpack_from(">H", data, p1 + 4)[0]
    loop_start, total = struct.unpack_from(">II", data, p1 + 8)
    return loop_flag, rate, loop_start, total


def brstm_to_ogg(path: str, out_ogg: str):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("error: BRSTM input needs ffmpeg on PATH to transcode",
              file=sys.stderr)
        sys.exit(1)
    subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", path,
                    "-c:a", "libvorbis", "-q:a", "6", out_ogg], check=True)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="wrap an OGG/WAV in a Robox .rbxs sound header")
    ap.add_argument("input", help="source .ogg or .wav")
    ap.add_argument("output", help=".rbxs to write")
    ap.add_argument("--loop", nargs=2, type=int, metavar=("START", "END"),
                    help="loop points in frames (intro plays once)")
    ap.add_argument("--once", action="store_true",
                    help="play once instead of looping")
    ap.add_argument("--mono", action="store_true",
                    help="downmix to mono at playback")
    ap.add_argument("--rate", type=int, default=0,
                    help="force playback sample rate (default: the audio's own)")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    tmp_ogg = None
    if data[:4] == b"RSTM":
        loop_flag, rate, ls, total = brstm_info(data)
        print(f"{args.input}: BRSTM rate={rate} loop_flag={loop_flag} "
              f"loop={ls}..{total} -- transcoding via ffmpeg")
        tmp_ogg = args.output + ".tmp.ogg"
        brstm_to_ogg(args.input, tmp_ogg)
        data = open(tmp_ogg, "rb").read()
        # The BRSTM's own header wins unless the caller overrides.
        if not args.loop and not args.once:
            if loop_flag:
                args.loop = [ls, total]
            else:
                args.once = True

    if data[:4] == b"OggS":
        src_rate = ogg_vorbis_rate(data)
    elif data[:4] == b"RIFF":
        src_rate = wav_rate(data)
    else:
        print(f"error: {args.input} is not an OGG, WAV, or BRSTM "
              f"(starts {data[:4]!r})", file=sys.stderr)
        return 1

    if args.once and args.loop:
        print("error: --once and --loop are mutually exclusive", file=sys.stderr)
        return 1

    start, end = args.loop if args.loop else (0, 0)
    hdr = b"RBXS"
    hdr += struct.pack("<I", args.rate)
    hdr += struct.pack("<I", 1 if args.mono else 0)
    hdr += struct.pack("<H", 0 if args.once else 1)
    hdr += b":P"
    hdr += struct.pack("<II", start, end)
    assert len(hdr) == 24 and hdr[0x0E:0x10] == b":P"

    open(args.output, "wb").write(hdr + data)
    if tmp_ogg:
        os.remove(tmp_ogg)

    rate = args.rate or src_rate
    loop = ("play once" if args.once
            else f"loop {args.loop[0]}..{args.loop[1]}" if args.loop
            else "loop whole song")
    secs = ""
    if args.loop and rate:
        secs = f" ({args.loop[0] / rate:.2f}s..{args.loop[1] / rate:.2f}s)"
    print(f"{args.output}: {len(hdr)}-byte header + {len(data)} bytes "
          f"{'OGG' if data[:4] == b'OggS' else 'WAV'}, "
          f"rate={'auto (%s)' % (src_rate or '?') if not args.rate else args.rate}, "
          f"{'mono' if args.mono else 'channels as-is'}, {loop}{secs} :P")
    return 0


if __name__ == "__main__":
    sys.exit(main())
