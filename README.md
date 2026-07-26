# ROBOX Recompiled

A static recompilation of **Robox** (WiiWare, Dreambox Games, 2010) to a native
PC executable. The game's PowerPC code is translated ahead of time into C —
12,831 functions across 51 files — and linked against a host-side
reimplementation of the parts of the Wii it used: the GX graphics pipeline on
OpenGL, the AX audio mixer, the OS threading and heap layer, and the disc/NAND
filesystem.

It is not an emulator. There is no interpreter and no JIT; the game *is* the
executable.

## Getting it running

**You need your own copy of the game.** No game data is distributed here and
none ever will be — see [THIRD-PARTY.md](THIRD-PARTY.md).

Run the executable. On first launch a setup screen offers two routes:

- **Install from a WAD** — point it at your Robox WAD and it does the rest:
  decrypts the contents, decompresses the LZ11 executable, unpacks the ~1,500
  asset files, and builds the soundfont. Drop the WAD beside the exe and it
  finds it without being asked. This route needs a Wii common key (`key.bin`),
  which is not shipped here; the setup will ask for one.
- **Locate files manually** — if you already have the game unpacked, pick the
  `.dol` and then the `Assets` folder. They need not be in the same place.

Either way it happens once and the choice is remembered.

## Building

Building needs two things this repository does not contain: **the recompiler**,
and **your own copy of the game**. The recompiled C is generated here at
configure time rather than committed, because it is a translation of the retail
executable and therefore a derivative of it — the same reason no game data is
here.

```sh
git clone https://github.com/Eggscantfly/WiiRecomp.git ../WiiRecomp
cp "/path/to/Robox USA.dol" private/
```

Then, Windows / MSYS2 mingw64:

```sh
export PATH=/c/msys64/mingw64/bin:$PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

or run `build.bat`, which does the same incrementally.

CMake runs the recompiler on the first configure — 12,831 functions, a few
seconds — and leaves the result alone afterwards. Delete `src/generated/` to
force a regeneration after changing the recompiler.

Both locations are overridable if you keep things elsewhere:

```sh
cmake -S . -B build -G Ninja \
      -DROBOX_RECOMPILER=/path/to/WiiRecomp \
      -DROBOX_DOL="/path/to/Robox USA.dol"
```

Note this is the *build* requirement. A released binary already contains the
recompiled code and only asks for game assets on first run.

**Build Release.** A Debug build cannot hold real-time pace in the softsynth and
MIDI sequencer, and the symptom is held notes rather than an obvious slowdown.

`gcc` needs `C:\msys64\mingw64\bin` on `PATH` — not merely an absolute path to
the binary. Invoked absolutely it fails with exit 1 and *no diagnostics at all*,
because the driver cannot find its own DLLs. That looks exactly like a silent
compiler bug and is not one.

You will also need SDL2 and Python 3; the build runs a few generator scripts.

### Building from a clean clone

The overlay font is extracted from the game's own `Assets/fonts/title.brfna` at
configure time, so **a fresh clone cannot build without game assets present**.
That is a known wart — you need the game unpacked before you can build the
program that unpacks the game. Bundling an openly-licensed font would fix it.

### Other targets

Each non-desktop port has its own repository:

| target | repository |
|---|---|
| Android (iOS planned) | [robox-recompiled-mobile](https://github.com/Eggscantfly/robox-recompiled-mobile) |
| Browser / Emscripten | [robox-recompiled-web](https://github.com/Eggscantfly/robox-recompiled-web) |
| Nintendo 3DS | [robox-recompiled-3ds](https://github.com/Eggscantfly/robox-recompiled-3ds) |

Each holds its packaging and the source files that are only that platform, and
consumes this repository as a submodule. Build from the port repository, not
this one; point this build at a port with `-DROBOX_PLATFORM_DIR=<port>/platform`.

The conditional code stayed here — the `#if defined(__ANDROID__)` /
`__EMSCRIPTEN__` / `__3DS__` blocks are woven through `video.c`, `gx_ogl.c`,
`main.c` and others, and compile to nothing on targets that are not them.
Separating those properly is a refactor across a dozen core files that nobody
could test without all three toolchains at once, so it was not attempted.

All three arrive with their game data already in place — the APK extracts it,
the 3DS reads RomFS, the web build preloads MEMFS — so the setup screen
compiles out to a no-op on each.

## Mods

`mods/mods.cfg` turns them on and off; `ROBOX_MOD_<ID>=0|1` overrides it for a
single run. A mod whose data file is missing is forced off and says which file
it wanted.

| id | what it does |
|---|---|
| `music` | Host MIDI + DLS sampler, replacing the game's wavetable synth |
| `wavmusic` | Music packs — swap any song for a WAV/OGG/RBXS file |
| `mario` | Bit-exact Super Mario Bros. movement physics |
| `discord` | Rich Presence |
| `coop` | Local / online co-op (work in progress, off by default) |

`.rbxs` is a small container: a 24-byte header carrying loop points in front of
an OGG or WAV payload. Loop points are the thing a bare OGG cannot express —
intro once, then the loop region forever. Build one with `tools/make_rbxs.py`.

## Environment variables

| variable | effect |
|---|---|
| `RECOMP_WINDOWED=1` | Do not launch fullscreen (F11 toggles at runtime) |
| `RECOMP_FORCE_SETUP=1` | Show the setup screen even with a good install |
| `RECOMP_NO_SETUP=1` | Never show it |
| `RECOMP_NO_SPLASH=1` | Skip the intro |
| `RECOMP_ASSETS=<dir>` | Use an `Assets` tree somewhere else |
| `RECOMP_LOG=1` | Write the engine log to `logs/run.log`. Off by default — a normal run is silent and leaves no files |
| `ROBOX_AXDUMP=1` | Dump the audio mixer output to `axmix.wav` (~23 MB for the 2-minute cap) |

A crash still writes `logs/crash.log` regardless of `RECOMP_LOG` — the fatal
handlers reopen stderr onto it before dumping registers and the guest
backtrace, so a quiet build still leaves a post-mortem behind.

## Layout

```
sdk/            host-side Wii: GX→OpenGL, AX audio, OS/threads, filesystem, mods
src/            entry point and the PPC runtime
src/generated/  recompiled PowerPC — generated, never committed
quirks/         per-title behaviour patches
tools/          build-time generators and standalone tests
mods/           default mod configuration
Setup/          the setup screen's own music and sound
splash/         the intro animation's stills and cues
private/        where you put your own .dol — generated, never committed
```

The recompiler itself is a separate repository, in the same arrangement
XenonRecomp has with UnleashedRecomp.

## A note on AI assistance

Parts of this project were written with Claude (Anthropic). Commits carrying a
`Co-Authored-By: Claude` trailer had AI involvement.

This is stated plainly for two reasons. One is attribution — it should be clear
which work was not written unaided. The other matters more if you are reading
the code: AI-written code can be confidently wrong in ways that read as
authoritative, and its comments are no exception. Where a comment here explains
*why* something is done a particular way, treat it as a claim to check rather
than a citation.

That said, the load-bearing parts are not taken on trust. `tools/` holds
standalone tests that check the WAD extraction against real dumps and the
soundfont generator byte-for-byte against the Python it was ported from, and
the recompiler's output was diffed against a known-good tree before the
generated code was allowed to become a build step.

## Credits

The approach this project takes comes from
**[I Built a PS1 Static Recompiler With No Prior Experience (and Claude Code)](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/)**
by Matthew Stanley (March 2026), which describes building
[psxrecomp](https://github.com/mstan/psxrecomp) — a static recompiler for the
original PlayStation — with Claude Code alongside Ghidra.

What is borrowed is the method that article sets out: translate the binary
ahead of time, then close the gap with an iterative loop of build, watch it
fail, fix, and check the fix against the disassembly. The console and the
instruction set are different here; the approach is not. This project would
not exist in this form without that write-up.

## Licence

**GPL-2.0-or-later.** Parts of the renderer are ported from Dolphin, which is
GPL, and a port is a derivative work. Full text in [LICENSE](LICENSE); every
component and its terms in [THIRD-PARTY.md](THIRD-PARTY.md).
