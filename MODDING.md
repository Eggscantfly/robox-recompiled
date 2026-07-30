# Writing mods for ROBOX Recompiled

A mod is a `.lua` file in `mods/lua/`. Save it and the game picks it up — no
rebuild, no restart, no toolchain. This is the whole reason the scripting
runtime exists: the C mods in `sdk/` (co-op, Mario physics, music packs) each
cost a full recompile of ~3000 generated translation units to change one line.

```lua
-- mods/lua/first.lua
robox.on("draw", function()
  robox.draw.text(24, 24, "hello")
end)
```

Start the game. It says hello. Change the string, save, and the text changes
while you watch — the runtime stat()s every mod file a few times a second and
re-runs the ones that moved.

Turn a mod off by renaming it `_first.lua`, or move it into `mods/lua/disabled/`.
Turn the whole runtime off with `lua = 0` in `mods/mods.cfg`. Freeze the file
watcher (but keep mods loaded) with `ROBOX_LUA_RELOAD=0`.

**Mods are trusted code.** A mod can write any byte of the game's memory and
replace any function in it. There is no sandbox, and the standard library is
open — `io` and `os` included. Install mods the way you install any other
program.

---

## Two Lua VMs

There is Lua on both sides of this port, and they are not the same thing.

|  | **host mods** | **guest scripts** |
|---|---|---|
| where | `mods/lua/*.lua` | `mods/lua/guest/*.lua` |
| VM | Lua 5.4, bundled with the port | Lua 5.1, shipped inside `Robox USA.dol` |
| runs | every frame, forever | once, during the game's boot |
| reach | guest memory, function hooks, input, HUD | the engine's own asset bindings |
| hot reload | yes | no — restart the game |

Almost everything you want is a **host mod**. The rest of this document is
about those; guest scripts get [their own section](#guest-scripts) at the end.

---

## Events

```lua
robox.on("frame", function(dt) end)
```

| event | when | arguments |
|---|---|---|
| `start` | after every mod has loaded, and after this mod reloads | — |
| `frame` | once per host frame | `dt` — seconds, clamped to 0.25 |
| `draw` | once per presented frame, inside the HUD batch | — |
| `level` | the level id changed | `id`, `name`, `is_robot` |
| `key` | a key went down or up | `name` (SDL name, e.g. `"F9"`), `down` |
| `text` | a character was typed, while you hold `input.capture` | `s` — UTF-8 |
| `unload` | this mod is about to be reloaded or removed | — |

You can register as many handlers per event as you like. `robox.draw.*` only
works inside a `draw` handler; call it anywhere else and it says so.

---

## The API

Everything lives on a per-mod `robox` table. `robox.name`, `robox.dir` and
`robox.file` describe the mod that is asking.

### Memory

Guest RAM is MEM1 at `0x80000000` (24 MB) and MEM2 at `0x90000000` (64 MB).
Reads outside those return `nil` rather than a plausible zero; writes raise.

```lua
robox.mem.u8(va)   robox.mem.u16(va)   robox.mem.u32(va)
robox.mem.i8(va)   robox.mem.i16(va)   robox.mem.i32(va)
robox.mem.f32(va)  robox.mem.f64(va)

robox.mem.write_u8(va, v)   robox.mem.write_u16(va, v)
robox.mem.write_u32(va, v)  robox.mem.write_f32(va, v)
robox.mem.write_f64(va, v)

robox.mem.read(va, n)       --> string of n raw bytes
robox.mem.write(va, str)
robox.mem.cstr(va [, max])  --> NUL-terminated string
robox.mem.valid(va [, n])   --> is this really RAM?
robox.mem.follow(va, off1, off2, ...)  --> pointer chain, nil if it leaves RAM
robox.mem.find("48 ?? 00 3f" [, from, to])  --> signature scan, ?? wildcards
```

Byte swapping is handled: the Wii is big-endian, you get host-order numbers.

`find` defaults to all of MEM1, which is 24 MB of byte-at-a-time comparison —
give it a range once you know roughly where to look.

### The player

```lua
robox.player.addr()          --> CPlayer pointer, or nil outside a level
robox.player.get("x")        --> x  y  health  state  iframes  anim
robox.player.set("health", 3)
robox.player.fields()        --> { x = 0x38, y = 0x3c, ... }
```

Offsets come from the project's Dolphin watch list, and are the same ones the
in-game settings menu's debug tab reads. If you find a field this table does
not know about, `robox.player.addr()` plus `robox.mem.*` gets you there — and
then please add a row to `PLAYER_FIELD` in `sdk/robox_lua_api.c`.

### Input

Names match `controls.cfg` and the settings menu: `up down left right jump
action plus a b minus home tiltl tiltr shake`.

```lua
robox.input.held("jump")      robox.input.pressed("jump")
robox.input.released("jump")
robox.input.press("right" [, frames])   -- inject; default 1 frame
robox.input.release("right")
robox.input.block(true|false)           -- take control away from the player
robox.input.capture(true|false)         -- take the whole keyboard
robox.input.mask()            --> held, raw   (bitmasks)
robox.input.key("F9")         --> raw keyboard, by SDL key name
robox.input.stick()           --> x, y  in -1..1
robox.input.buttons()         --> { jump = 0x100, ... }
```

Injected presses are merged in before the d-pad rotation the port applies
inside the robot sections, so `press("left")` means the direction the player
sees, not a raw hardware bit.

`block` drops the real buttons *and* the stick on the way to the game, but
leaves injected presses working — so "take control away and drive him
yourself" is one call plus `press`. Host keys (Esc, F6, F9…) are unaffected, so
a mod can never lock someone out of the menu that turns it off, and the block
is released automatically whenever a mod reloads.

`capture` is the bigger hammer, and it is what a text prompt needs: while one
mod holds it, **every key edge and every typed character goes to that mod
alone** — the other mods' `key` handlers do not run, the port's own hotkeys
(Escape, the F-keys, fullscreen) stand down, and the guest sees no input at
all. Typing `off` at a prompt should not toggle fullscreen on the `f`.

Held by one mod at a time, and dropped automatically if that mod reloads, so
the keyboard can never end up owned by something that no longer exists. Key
*ups* still reach the port while captured, deliberately: they only clear held
bits, and swallowing them would leave a direction latched when you let go.

Typed text arrives as its own event, because a scancode is not a character —
shift, the symbol row and any non-US layout live in the translation:

```lua
robox.on("text", function(s) ... end)   -- one SDL_TEXTINPUT chunk, UTF-8
```

It only fires while you hold capture. `mods/lua/console.lua` is the worked
example; see [The console](#the-console).

### Drawing

A virtual 1280×720 space (`robox.draw.W`, `robox.draw.H`), rescaled to
whatever the window actually is. Colours are 0..1 floats.

```lua
robox.draw.rect(x, y, w, h [, r, g, b, a])
robox.draw.outline(x, y, w, h [, thickness, r, g, b, a])
robox.draw.text(x, y, str [, r, g, b, a, scale, tracking])
robox.draw.text_width(str [, scale, tracking])
robox.draw.tri(x0,y0, x1,y1, x2,y2, r,g,b,a [, ...corner 1, ...corner 2])
```

The font is the game's own. `y` is the **top** of the text, not the baseline —
at scale 1.7 the glyphs are about 85 tall, which matters when you put something
underneath them.

`tri` is the primitive everything non-rectangular is built from: pass one
colour for a flat triangle, or three for a gradient. The overlay's vertex
format has always been per-vertex colour — `rect` is just two triangles at the
white texel — so this exposes what the batcher could already do rather than
adding a renderer path. Circles, glows and light beams are a few lines of Lua
on top; `mods/lua/party_mode.lua` has `glow()` and `beam()` written out.

### Hooking guest functions

This is the powerful one. Every function in the recompiled game is reachable
by its original Wii address, and any of them can be intercepted.

```lua
robox.hook(va, fn)         -- replace it; fn may call robox.original()
robox.hook_before(va, fn)  -- fn first; return false to skip the original
robox.hook_after(va, fn)   -- the original first, then fn
robox.unhook(va)
robox.original()           -- inside a replace hook: run what you displaced
```

The handler is called as `fn(r3, r4, r5, r6)` — the first four PowerPC
argument registers, snapshotted *before* the original ran, so an `after` hook
still sees the arguments and not the return value. Return an integer to set
the function's return value (`r3`); return `false` from a `before` hook to
suppress the original entirely.

```lua
-- Count what a level spawns. func_800b3144 is the engine's generic entity
-- init; it writes the entity's type id to this+0x1d8.
robox.hook_after(0x800b3144, function(this)
  local t = robox.mem.u32(this + 0x1d8)
  counts[t] = (counts[t] or 0) + 1
end)
```

Hooks are released automatically when the mod reloads. There are 64 slots.
Re-entry is guarded: if your handler ends up calling the function it hooked,
the inner call runs the original rather than recursing.

### Calling into the game

```lua
local r3, f1 = robox.call(va, ...)          -- ints, booleans and strings
local r3, f1 = robox.call_ex(va, {ints}, {floats})
```

Integer and boolean arguments go into `r3..r10`; a string argument is copied
into a scratch area carved out of the guest stack and its address passed
instead. `call_ex` reaches `f1..f8` for the cases the EABI needs both.

The whole register file is saved and restored around the call — the frame hook
this runs from is itself inside guest execution, so anything left modified
would land in the middle of whatever function was interrupted.

There is no protection against calling a function with the wrong arguments.
That crashes the game, exactly as it would in C.

### Registers

```lua
robox.cpu.gpr(3)        --> value; robox.cpu.gpr(3, v) writes and returns the old
robox.cpu.fpr(1)        --> as a number
robox.cpu.lr()   robox.cpu.ctr()
```

Only meaningful from inside a hook, where the guest is mid-call.

### Level

```lua
robox.level.id()   robox.level.name()   robox.level.is_robot()
robox.level.load(id [, arg])   -- warp: a real level transition
```

`load` does exactly what the game's own play / load-save path does: request the
level (`func_80064954`), then clear the game-state singleton's mode field
(`func_800ddd34`), which is what re-arms the transition. The engine then runs
the change itself, on its own fade, from whatever state it was in.

That second step is not optional. The consumer gates on `mode != 2`, and mode 2
means "not in gameplay" — the main menu. Set the request without clearing it
and the menu throws the request away every frame, which looks exactly like
nothing happening. Getting this right is what makes warping work from the title
screen and not just mid-level.

Call it from a `frame` handler, never from `draw`. Level 100 is `01a`; ids
`0xa0..0xb3` are the robot interiors and get mode 1 instead of 0, matching the
engine's own test at `0x80063e94`.

### Audio

```lua
robox.audio.volume([0..100])   robox.audio.music([bool])
robox.audio.music_set(song, path)   -- map a song at runtime; nil path unmaps
robox.audio.music_play(path [, loop])  --> true if it opened; play a file NOW
robox.audio.music_playing()            --> nil, or name, is_mine
robox.audio.music_stop()
robox.audio.level()   --> 0..1, how loud it is right now
robox.audio.beat()    --> 0..1, transient — spikes on hits
robox.audio.spectrum([bands])  --> array of 0..1, bass first (default 24)
```

`music_set` replaces one of the **game's** songs and only takes effect the next
time the game starts one, which is no use to a mod that has to change track
every three minutes. `music_play` is the other direction: it puts a file on
immediately — same decoders as a music pack, so WAV, Ogg Vorbis or RBXS.

Without `loop` the track plays once and stops, and that stop is what makes a
**playlist** possible: `music_playing` goes nil at the end of the track and the
mod puts the next one on. Its second return says whether the stream is *yours* —
the game starting its own song (a level load) reads as playing too, and a queue
that cannot tell the difference stops advancing the moment a level changes.

While your track runs, the game's own music is **muted, not stopped**, so
`music_stop` hands the soundtrack back mid-bar with nothing to put back.
`mods/lua/party_mode.lua` is the worked example: a download queue feeding
`music_play`, with F7 to skip.

`level` and `beat` come off the real mixer, so they follow whatever is playing:
the game's music, a music-pack track, or a sound effect. Driving a visual from
`beat` puts it in time with the song by construction, instead of a tempo
written into the mod that slides out of sync over three minutes.

`spectrum` is a Hann-windowed 1024-point FFT of the live mix, folded into
log-spaced bands (an octave is an octave, whether it is 80 Hz or 8 kHz) with a
3 dB/octave pink tilt so the treble bands are not permanently flat. It is
**auto-gained**: a slow-falling, fast-rising reference tracks the loudest band,
so the display fills its range at any volume and you never have to turn the
game down to make a visualiser look right. One FFT per frame at most — the
result is cached on the frame counter, so calling it from both `frame` and
`draw` costs nothing extra.

### Video

```lua
robox.video.draws()            --> draw calls the game submitted last frame
robox.video.clear(r, g, b)     -- force the EFB clear colour
robox.video.clear()            -- hand it back to the game
```

`draws` is the honest answer to "is anything actually being rendered", which
you need whenever you move the player or camera somewhere unusual — a black
screen and a correctly-drawn black scene look identical otherwise.

`clear` matters because the overlay draws *over* the finished frame: an opaque
rectangle hides the player along with the background. To change what shows
where the game draws nothing, you have to change the colour it clears to.
Robox's is `0x802b94` — purple — which you only ever see outside the level.

### Network

For a mod that wants data it does not ship — a track, a texture pack, a table.

```lua
local h = robox.net.fetch(url, dest_path)      --> handle, or nil
local state, got, total, err = robox.net.status(h)   -- "working"|"done"|"error"
robox.net.release(h)
```

It **fetches and caches**; it does not stream. The download runs on its own
thread and you poll it, because the frame handler is inside guest execution and
must never block on a socket. `dest` is written atomically via a `.part` file,
so a half-finished download is never mistaken for a cached one.

MP3 in, WAV out: the payload is sniffed after download and transcoded with
dr_mp3 if it is MPEG audio, so what lands on disk is always something
`robox.audio.music_set` can play. WAV, Ogg and RBXS pass through untouched.

**Windows only** for now — it's built on WinHTTP, which ships with the OS and
handles TLS and redirects. Elsewhere `fetch` returns a handle that immediately
reports `error`, so write the fallback path and a mod stays portable.

A URL in a mod is a distribution channel: everyone who installs it fetches
whatever is there. Point it at something you have the right to use and pass on
— your own files, or Creative Commons / public-domain material. "It was already
online" is not a licence, and neither is the host.

### Other mods, time

```lua
robox.enabled("coop")                   -- is another mod on?
robox.time()   robox.frame()
```

### Logging and settings

```lua
robox.log(...)             -- to stderr, tagged with the mod name
robox.notify(text [, secs])-- on screen
print(...)                 -- same as robox.log

local cfg = robox.config.load()   -- flat table from <mod>.cfg / <dir>/config.cfg
cfg.enabled = true
robox.config.save(cfg)            -- strings, numbers and booleans
```

A reload throws every Lua value away. Anything that should survive one goes
through `robox.config`.

```lua
robox.watch("some/other/file.txt")  -- reload this mod when that file moves
```

---

## Mod layout

```
mods/lua/hello.lua              one file
mods/lua/god_mode/init.lua      a folder: every .lua in it is watched
mods/lua/god_mode/tuning.lua      require("god_mode.tuning")
mods/lua/god_mode/config.cfg      written by robox.config.save()
mods/lua/guest/*.lua            guest scripts (see below)
mods/lua/disabled/              ignored
```

`package.path` already covers `mods/lua/`, so a folder mod's own modules are
`require`-able as `<folder>.<file>` with no setup. `require` normally caches
forever; the runtime clears this mod's entries on reload, so editing a
`require`d file works the same as editing the main one.

Errors are reported three ways: a red toast on screen with the first line, the
full traceback on stderr, and the mod is left loaded so you can fix it and
save. A mod that errors every frame is rate-limited after the first few.

---

## The console

`` ` `` drops a Quake-style console over the game. It is a Lua mod like any
other — `mods/lua/console.lua`, about 700 lines — and it can reach anything
the API can, which means anything the game can.

```
] level 01b            warp
] tp 500 250           move the robot
] anim 4               the unused BAILA sequence
] peek 0x801FADA4 16   hex dump guest memory
] = robox.player.get("x") * 2
] bind F5 "level 100; dance"
```

`help` lists everything, `Tab` completes, `Up`/`Down` walk the history,
`PgUp`/`PgDn` scroll, and `= <expr>` evaluates Lua in the console's own mod
environment. Binds, aliases and history persist in `mods/lua/console.cfg`.
`log on` follows `logs/run.log` in the scrollback, so the port's own
`[WAV]`/`[video]`/`[lua]` chatter — and every mod's `robox.log` — scrolls past
live while the game runs. That needs the game started with `RECOMP_LOG=1`; a
normal run points stderr at the null device, and the command says so rather
than tailing a stale file.

**Escape opens the settings menu, `` ` `` opens the console.** `` ` `` used to
be an Escape alias (browsers eat Escape); that alias is now **F1**.

### Adding commands from your own mod

The command table lives in the real globals, so it survives the console
reloading and any mod can add to it. Register from `start` — every mod's chunk
has run by then, so file order does not matter:

```lua
robox.on("start", function()
    local con = rawget(_G, "ROBOX_CONSOLE")
    if not con then return end          -- console not installed, fine
    con.add("boom", "[size]", "blow something up", function(args, rest)
        return "boom: " .. (args[1] or "medium")   -- returned strings print
    end)
end)
```

`con.print(text, kind)` and `con.printf(fmt, ...)` write to the scrollback
(`kind` is `out`, `dim`, `ok`, `warn`, `err` or `hi`), and `con.run(line)`
runs a command line as if it were typed. `mods/lua/party_mode.lua` registers
`party`, `skip` and `playlist` this way.

---

## Guest scripts

The game itself embeds a complete Lua 5.1 — `LUA_PATH`, the whole standard
library, and the engine's own bindings — and only ever uses it to hold tileset
tables and translated strings:

```lua
textureDir  = "media/spr/";
textureList = { "bone" };
```

So there is a scripting engine inside the game, wired to the renderer and the
asset loader, that has never run a line of gameplay code. Put a `.lua` in
`mods/lua/guest/` and it runs inside that VM during the game's own boot,
loaded in filename order.

No patching is involved. Every `.lua` the guest reads — `script/game.lua` and
everything its `dofile()` pulls in — goes through the port's content system, so
the port serves a `script/game.lua` with your scripts appended to it. It works
on a stock DOL and survives a recompile.

What is in scope there:

- the engine's bindings: `loadMap`, `loadMap2`, `newFpg`, `unloadFpg`,
  `unloadTiles`, `loadTileset`, `cargaAnimacion`, `renderQuad`, `getText`,
  `nameForDebug`
- what `game.lua` set up before you: `textureDir`, `textureList`,
  `loaded_fpg`, `IMAGE_EXT`, `loadBoneset`, and the strings from
  `script/lang/lang_*.lua`
- Lua **5.1**: no `goto`, no integer division, no `<close>`

It runs **once**, at boot. The engine never calls back into Lua afterwards, so
there is no frame hook on that side. Wrapping an engine function is the useful
shape:

```lua
local original = loadBoneset
loadBoneset = function(b)
  robox.log("loading " .. tostring(b))
  return original(b)
end
```

### Talking to the host

Guest scripts get a `robox.host(name, ...)` that calls a handler a host mod
registered, and returns what it returned:

```lua
-- host: mods/lua/hello.lua
robox.game.handle("player_pos", function()
  return { x = robox.player.get("x"), y = robox.player.get("y") }
end)

-- guest: mods/lua/guest/hello.lua
local p = robox.host("player_pos")
robox.log(p.x, p.y)
```

Under the hood it is a `dofile()` of a path that is not a file — the port
answers the read with a generated chunk. Strings, numbers, booleans and
one-level tables make the trip. It costs a file write and a parse per call, so
it is for asking something once at boot, not for a loop.

---

## Where this lives in the tree

| file | what |
|---|---|
| `sdk/robox_lua.c` | VM, mod discovery, hot reload, event dispatch, toasts |
| `sdk/robox_lua_api.c` | the `robox.*` surface |
| `sdk/robox_lua_guest.c` | the guest VM bridge and the RPC |
| `vendor/lua/` | Lua 5.4.7, unmodified |

Call sites: `sdk/video.c` (per-frame tick, injected input),
`sdk/gx_ogl.c` (the draw batch), `sdk/robox_io.c` (the guest-script seam),
`sdk/robox_mods.c` (the registry row).

Not built for the 3DS — that port has no overlay renderer and no memory to
spare. The `sdk/robox_lua*.c` files compile to no-ops there.
