# Third-party components

Everything this project ships or links, and the terms it comes under. If you
add a dependency, add it here.

The project as a whole is **GPL-2.0-or-later** — see `LICENSE`. The reason is
the first entry below: parts of the renderer are ported from Dolphin, and a
port is a derivative work, so the combined result inherits Dolphin's licence.

---

## Dolphin — GPL-2.0-or-later

<https://dolphin-emu.org> · <https://github.com/dolphin-emu/dolphin>

**This is what makes the project GPL.** Parts of `sdk/gx_ogl.c` are ported
from Dolphin's `VideoCommon`:

| What | Ported from |
|---|---|
| GX texgen path | `VideoCommon/VertexShaderGen.cpp` |
| Integer TEV colour combiner | `VideoCommon/PixelShaderGen.cpp` |
| BP register decoding | `VideoCommon/BPStructs.cpp` |

These are translations of Dolphin's logic into this renderer's own structures
rather than verbatim copies. That is still a derivative work — a translation
of a program is exactly what copyright calls one — so it does not change the
obligation. `sdk/gx_ogl.c` carries a notice saying so.

`sdk/peripherals.c` also cites Dolphin, but for **hardware facts** — BP
register numbers, FIFO opcode encodings, the shape of the GX command stream.
Those are descriptions of what the Wii's GPU does, learned by reading a good
reference. That is not the same thing as porting code, and is noted here for
honesty rather than because it creates an obligation.

No Dolphin source is vendored in this repository.

**What GPL-2.0-or-later requires of anyone distributing this:** ship the
licence text (`LICENSE`), keep the copyright and licence notices intact, say
what you changed, and make the complete source available to anyone you give
binaries to. Publishing the source on GitHub alongside a release satisfies the
last one.

---

## SDL2 — Zlib licence

<https://libsdl.org>

Linked, not vendored — CMake finds it on the system (`find_package(SDL2)`).
Windowing, input, audio and the GL context. The Zlib licence asks that you not
misrepresent the origin of the software and that modified versions be marked
as such; this project does not modify SDL.

The 3DS target links devkitPro's prebuilt SDL2 instead, same licence.

---

## stb_vorbis — Public domain, or MIT at your option

Sean Barrett · <http://nothings.org/stb_vorbis/>

Vendored at `sdk/stb_vorbis.c`. Decodes the OGG Vorbis inside `.rbxs` files —
the setup's music and sound, the splash cues, and the music-pack mod. Dual
licensed; either arm is compatible with GPL-2.0-or-later. The file's own
licence text is at the bottom of it and must stay there.

---

## fmidi — Boost Software License 1.0

<https://github.com/jpcima/fmidi>

Vendored at `fmidi-master/`. The host-side MIDI sequencer behind the music
mod. BSL-1.0 is a permissive licence compatible with the GPL; it does not
require attribution in binary distributions, but the licence file stays in the
tree regardless.

---

## discord-rpc — MIT

Copyright 2017 Discord, Inc. · <https://github.com/discord/discord-rpc>

Vendored at `discord-rpc-master/`. Rich Presence for the `discord` mod. MIT
requires the copyright notice and permission notice be kept with any
distribution; `discord-rpc-master/LICENSE` is that notice.

---

## dr_mp3 — Public domain (or MIT-0 at your option)

David Reid · <https://github.com/mackron/dr_libs> · based on
[minimp3](https://github.com/lieff/minimp3) by lieff

Vendored at `vendor/dr_mp3.h`, upstream and unmodified. Used by
`sdk/robox_net.c` to turn a downloaded MP3 into a WAV once, at download time,
so `sdk/robox_wav.c` never has to learn a third codec. Dual-licensed public
domain / MIT-0; neither requires attribution, and the licence text at the
bottom of the header stays with it regardless.

---

## Lua 5.4.7 — MIT

Copyright © 1994–2024 Lua.org, PUC-Rio · <https://www.lua.org>

Vendored at `vendor/lua/`, upstream sources unmodified, minus `lua.c` and
`luac.c` (the standalone interpreter and compiler, both of which define
`main()`). It is the VM behind the `lua` mod — see `MODDING.md`. MIT requires
the copyright notice and permission notice be kept with any distribution;
`vendor/lua/README` and the header of every source file carry it.

Note that this is the *port's* Lua, not the game's. Robox itself embeds a
Lua 5.1 of its own inside `Robox USA.dol`, which is retail game data and is
not distributed here — see below.

---

## What is NOT in this repository

Retail game data, and anything derived from it. None of the following is
distributed here, and none of it should ever be committed:

- `Robox USA.dol` and the `Assets/` tree — extracted from the user's own WAD
  by the setup at first run.
- `src/robox_font_a8.h`, `src/robox_font_metrics.h` — the menu font, extracted
  from `Assets/fonts/title.brfna` at build time.
- `mods/robox.dls` — the soundfont, built from `Assets/music/robox.wt` and
  `robox.pcm` by `sdk/robox_dls.c` at first run.
- `mods/mario.chr` — SMB1 character data. Optional; the mario mod runs without
  it and only loses its sprite swap.
- The Wii common key. The setup asks the user for a `key.bin`; none ships here.

The splash artwork under `Assets/splash/`, the setup audio under `Setup/`, and
the mod configs under `mods/` **are** this project's own work and are
committed.
