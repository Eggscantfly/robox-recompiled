"""
dolphin_db.py -- Port Dolphin's signature-DB matcher to our pipeline.

Dolphin ships `totaldb.dsy` -- a 1.4 MB binary database of PowerPC function
signatures (Nintendo SDK, popular middleware, CRT helpers) mapped to names.
Its entry format (from Source/Core/Core/PowerPC/SignatureDB/DSYSignatureDB.cpp):

    u32  fcount                     // little-endian
    struct FuncDesc {               // repeated fcount times (136 bytes)
        u32  checksum;              // little-endian
        u32  size;                  // little-endian
        char name[128];             // NUL-padded
    }

The checksum is computed over the function's instruction words. It deliberately
masks out register fields and most immediates so the hash stays stable across
different compilations of the same source. The algorithm is a 17-bit-rotate +
XOR mix (see ComputeCodeChecksum in SignatureDB.cpp).

This module loads the DB and provides `match_function(dol, addr, size)` which
returns the Nintendo SDK name if the function matches, else None.

CLI:
    python dolphin_db.py                      # summarise DB
    python dolphin_db.py "Robox USA.dol"      # match our funcs against DB
    python dolphin_db.py "Robox USA.dol" --apply
                                              # write results to identified_funcs.json
"""

from __future__ import annotations

import struct
import json
import argparse
import sys
import os
from pathlib import Path


DSY_PATH = Path(__file__).parent / "dolphin-master" / "Data" / "Sys" / "totaldb.dsy"
DSY_ENTRY_SIZE = 4 + 4 + 128      # checksum + size + name


# ---------------------------------------------------------------------------
# Checksum (exact port of Dolphin's ComputeCodeChecksum)
# ---------------------------------------------------------------------------

def dolphin_checksum(words: list[int]) -> int:
    """Port of HashSignatureDB::ComputeCodeChecksum (SignatureDB.cpp:161).

    For each instruction word:
      * Primary opcode always contributes (bits 31..26, mask 0xFC000000)
      * op2 / op3 pull in extended-opcode bits for the instruction forms
        that Dolphin cares about -- others keep registers/imms as zero so
        they don't disturb the hash.
      * sum = rotl17(sum) ^ (op | op2 | op3)

    All arithmetic is 32-bit unsigned.
    """
    s = 0
    for opcode in words:
        opcode &= 0xFFFFFFFF
        op = opcode & 0xFC000000
        auxop = op >> 26
        op2 = 0
        op3 = 0

        if auxop == 4:                           # paired singles
            op2 = opcode & 0x0000003F
            if op2 in (0, 8, 16, 21, 22):
                op3 = opcode & 0x000007C0
        elif auxop in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = opcode & 0x03FF0000
        elif auxop in (19, 31, 63):
            op2 = opcode & 0x000007FF
        elif auxop == 59:
            op2 = opcode & 0x0000003F
            if op2 < 16:
                op3 = opcode & 0x000007C0
        else:
            if 32 <= auxop < 56:                 # d-form loads/stores
                op2 = opcode & 0x03FF0000

        # 17-bit rotate left, then XOR in the masked opcode
        s = (((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x0001FFFF)) & 0xFFFFFFFF
        s = (s ^ (op | op2 | op3)) & 0xFFFFFFFF

    return s


# ---------------------------------------------------------------------------
# DSY loader
# ---------------------------------------------------------------------------

def load_dsy(path: Path = DSY_PATH) -> dict[int, tuple[int, str]]:
    """Return {checksum -> (size, name)}."""
    if not path.exists():
        raise FileNotFoundError(f"{path} not found; keep dolphin-master/ alongside the DOL")
    data = path.read_bytes()
    (fcount,) = struct.unpack_from("<I", data, 0)
    if len(data) != 4 + fcount * DSY_ENTRY_SIZE:
        print(f"warn: DB size mismatch -- header says {fcount} entries "
              f"(would be {4 + fcount*DSY_ENTRY_SIZE} bytes) but file is {len(data)}")
    out: dict[int, tuple[int, str]] = {}
    for i in range(fcount):
        off = 4 + i * DSY_ENTRY_SIZE
        checksum, size = struct.unpack_from("<II", data, off)
        name_bytes = data[off + 8 : off + 8 + 128]
        name = name_bytes.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        out[checksum] = (size, name)
    return out


# ---------------------------------------------------------------------------
# Match a function against the DB
# ---------------------------------------------------------------------------

# Matching a function smaller than MIN_MATCH_BYTES produces too many false
# positives: tiny single-return thunks hash to the same checksum across
# unrelated symbols. But a handful of critical OS helpers are exactly 8-12
# bytes (OSDisableInterrupts, OSEnableInterrupts). Drop to 8 bytes -- still
# excludes the 4-byte `blr` thunks that cause the worst false positives.
MIN_MATCH_BYTES = 8


def match_function(dol, addr: int, size: int,
                    db: dict[int, tuple[int, str]]) -> str | None:
    """Return the SDK name if Dolphin's DB has this function, else None.

    A match requires both checksum AND size to agree. Functions smaller than
    MIN_MATCH_BYTES are skipped to avoid false positives.
    """
    if size < MIN_MATCH_BYTES:
        return None
    nwords = size // 4
    if nwords == 0:
        return None
    words = [dol.read_u32_be(addr + 4 * i) for i in range(nwords)]
    h = dolphin_checksum(words)
    hit = db.get(h)
    if hit is None:
        return None
    db_size, name = hit
    if db_size != size:
        return None
    # Some DB names carry a trailing tab-separated "libname.a obj.o" suffix --
    # strip it to get a bare identifier we can use as a C symbol if needed.
    return name.split("\t", 1)[0].strip()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dol", nargs="?", help="DOL to match (omit to just dump DB info)")
    ap.add_argument("--apply", action="store_true",
                    help="write identified_funcs.json next to the DOL")
    ap.add_argument("--dsy", default=str(DSY_PATH), help="Dolphin .dsy file path")
    args = ap.parse_args(argv[1:])

    db = load_dsy(Path(args.dsy))
    print(f"Loaded {len(db):,} signatures from {args.dsy}")

    # Sample a few names so we can eyeball the DB content
    sample = list(db.items())[:8]
    for cksum, (sz, name) in sample:
        print(f"  sig {cksum:08x} size={sz:>5}  {name}")

    if not args.dol:
        return 0

    from dol_loader import DolFile
    dol = DolFile(args.dol)
    root = Path(args.dol).resolve().parent
    func_list_path = root / "func_list.json"
    if not func_list_path.exists():
        print("error: func_list.json missing; run recompile_wii.py first", file=sys.stderr)
        return 2
    funcs = json.loads(func_list_path.read_text())

    matched: dict[str, str] = {}            # "0xADDR" -> name
    size_mismatch = 0
    for fn in funcs:
        addr = int(fn["addr"], 16)
        size = fn["size"]
        if size == 0 or size > 0x10000:
            continue
        try:
            name = match_function(dol, addr, size, db)
        except Exception:
            continue
        if name:
            matched[fn["addr"]] = name

    print(f"\nMatched {len(matched):,} / {len(funcs):,} functions against the DB "
          f"({100*len(matched)/len(funcs):.1f}%)")

    # Show a few matches
    for i, (addr, name) in enumerate(sorted(matched.items())):
        if i < 30:
            print(f"  {addr} -> {name}")
    if len(matched) > 30:
        print(f"  ... ({len(matched)-30:,} more)")

    if args.apply:
        out_path = root / "identified_funcs.json"
        out_path.write_text(json.dumps(matched, indent=2))
        print(f"\nWrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
