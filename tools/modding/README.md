# Modding / reverse-engineering tools

Written during the co-op mod work. All are standalone Python, run from the
repo root (they hardcode paths to `Robox USA.dol` and `src/generated/`).

## gsym.py — generated C <-> guest addresses
The workhorse. The recompiler emits one C function per guest function with
`// <addr>: <disasm>` comments, but label-splitting means a "function" in the
generated file often is not the whole guest function.

```sh
python tools/modding/gsym.py grep '<regex>'      # find pattern -> func + PC per hit
python tools/modding/gsym.py body <addr> [n]     # print a function's body
python tools/modding/gsym.py at <file> <line>    # which func/PC is this line
```

`body` stops at the next function definition — without that, dumps spill into
neighbours and any call histogram you build from them is contaminated.

Typical use: find every reader/writer of an SDA global.
```sh
python tools/modding/gsym.py grep 'MEM_W32\(g_cpu\.gpr\[13\] \+ \(-0x4780\)'
```
Remember SDA offsets are relative to **r13 = 0x801FF460**, so
`0x801FACE0` is `SDA[-0x4780]`. Recompute; never assume the base.

## dol.py — read guest memory by virtual address
Parses the DOL section table properly (7 text @0x00/0x48/0x90, 11 data
@0x1c/0x64/0xac). A VA->offset mapping is only valid inside the section that
contains it.

```sh
python tools/modding/dol.py 801CF190 10   # read 10 words (e.g. a jump table)
python tools/modding/dol.py secs          # list sections
```

## doldiff.py — diff two DOLs, report guest VAs
For reverse-engineering someone else's byte-patch mod. Groups differences into
contiguous patch sites and decodes common opcodes, so injected code caves and
`bne`->`b` / `->nop` disables are obvious.

Edit the `A` / `B` constants at the top to pick the two DOLs.

## leafscan.py — find missed LEAF function starts
Function discovery looks for `stwu r1,-N(r1)` / `mflr r0` prologues, which
leaf functions do not have — they get absorbed into a neighbour's body and any
`bctrl` to them is a fatal "indirect call to unknown addr".

This scans for the other signal: a 4-aligned address right after a `blr`
(+ padding) that is not already known and decodes as plausible code.

```sh
python tools/modding/leafscan.py   # writes leaf_candidates.json
```

**Do not bulk-add all ~643 candidates.** A false function start truncates its
predecessor via the `addr+size` fallthrough rule. Add the confirmed crash
target, verify the preceding word really is `blr`, then regen (~5 s).
