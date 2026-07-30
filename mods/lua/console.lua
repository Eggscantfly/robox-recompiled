-- mods/lua/console.lua -- the drop-down console.  `  opens it.
--
-- The one Quake put on the tilde key: a panel that slides down over the game,
-- eats the keyboard, and runs commands. Everything the Lua API can reach is
-- reachable from here without a rebuild or even a file save -- warp a level,
-- move the robot, peek guest memory, put a track on, evaluate an expression.
--
-- WHY THE C SIDE HAD TO CHANGE
-- A console is not a mod that draws a text box; it is a mod that owns the
-- keyboard. Three things were missing and are now in the runtime:
--
--   robox.input.capture(true)   every key edge and every typed character comes
--                               to this mod and to NOBODY else -- not the other
--                               mods, not the port's own hotkeys, not the game.
--                               Without it, typing "off" would toggle fullscreen
--                               on the f, and Escape would open the settings
--                               menu instead of closing the console.
--
--   the "text" event            SDL_TEXTINPUT, i.e. the character the user
--                               actually typed. Scancodes cannot tell you that:
--                               shift, the symbol row and any non-US layout all
--                               live in the translation, not in the key.
--
--   `  freed in sdk/video.c     it used to be an Escape alias that opened the
--                               settings menu (browsers eat Escape). The menu
--                               kept Escape and gained F1; the console got  ` .
--
-- The font is the game's own (Assets/fonts/title.brfna) and it has a glyph for
-- every printable ASCII character except -- of all things -- ` and ~. Neither
-- can be typed in here anyway: the key that opens a console never types into
-- it. Lua's ~ operators are the one casualty; use bit32-style helpers or spell
-- the expression another way.
--
-- KEYS
--   `            open / close            Escape       close
--   Enter        run                     Tab          complete
--   Up / Down    history                 PgUp / PgDn  scroll
--   Home / End   line start / end        Ctrl+L       clear
--   Ctrl+U       clear line              Ctrl+W       delete word
--   Ctrl+A / E   line start / end        Ctrl+C       abandon line
--
-- Type  help  for the command list, or  = <expr>  to evaluate something.

-- --- tuning ----------------------------------------------------------------
local HEIGHT      = 0.56    -- fraction of the screen the panel covers
local SLIDE       = 0.14    -- seconds to slide down
local SCALE       = 0.62    -- text scale; the font is 33px tall at 1.0
local TRACK       = 0.0     -- extra letter spacing
local LINE_H      = 22      -- pixels per row at SCALE
local PAD         = 16
local MAX_LINES   = 500     -- scrollback kept
local MAX_HISTORY = 60      -- command history kept (and persisted)
local REPEAT_WAIT = 0.42    -- held-key delay before it starts repeating
local REPEAT_RATE = 0.035   -- and the interval after that

local W, H = robox.draw.W, robox.draw.H

-- This mod needs the modal keyboard: robox.input.capture and the "text" event,
-- both of which landed with it. On a port built before them, registering the
-- text handler is a hard error, and the hot-reload watcher would put a red
-- toast on screen every time the file is touched. Say it once instead.
if not robox.input.capture then
    robox.log("console: this build predates robox.input.capture -- "
              .. "rebuild the port and the console comes with it")
    return
end

local COL = {
    out  = { 0.87, 0.89, 0.93 },
    dim  = { 0.45, 0.50, 0.58 },
    ok   = { 0.42, 0.90, 0.55 },
    warn = { 1.00, 0.78, 0.30 },
    err  = { 1.00, 0.45, 0.45 },
    hi   = { 0.55, 0.80, 1.00 },
}

-- --- state -----------------------------------------------------------------
local open, slide = false, 0     -- slide is 0..1, animated
local lines       = {}           -- wrapped output rows, oldest first
local input, caret = "", 0       -- caret = bytes before the cursor (ASCII only)
local hist, hist_i = {}, 0       -- hist_i 0 = editing a fresh line
local stash       = ""           -- the fresh line, parked while browsing history
local scroll      = 0            -- rows scrolled up from the bottom
local held        = {}           -- key -> seconds held, for repeat
local binds, alias = {}, {}
local log_on, log_pos, log_f = false, 0, nil

-- --- output ----------------------------------------------------------------

-- Wrap at push time, not at draw time: the panel is a fixed width in overlay
-- space, a line's wrapping never changes once it is in the buffer, and the
-- alternative is measuring the whole scrollback every frame.
local function wrap(text, budget)
    local rows = {}
    while true do
        if robox.draw.text_width(text, SCALE, TRACK) <= budget then
            rows[#rows + 1] = text
            return rows
        end
        -- Widest prefix that fits, by bisection -- the font is proportional,
        -- so a character count would be wrong for anything but one string.
        local lo, hi = 1, #text
        while lo < hi do
            local mid = (lo + hi + 1) // 2
            if robox.draw.text_width(text:sub(1, mid), SCALE, TRACK) <= budget
                then lo = mid else hi = mid - 1 end
        end
        -- Prefer a word boundary, but never lose ground: a single word longer
        -- than the panel still has to be cut somewhere.
        local cut = lo
        local sp = text:sub(1, lo):find("%s[^%s]*$")
        if sp and sp > lo * 0.5 then cut = sp end
        rows[#rows + 1] = text:sub(1, cut)
        text = text:sub(cut + 1):gsub("^%s+", "")
        if text == "" then return rows end
    end
end

local function out(text, kind)
    local c = COL[kind or "out"] or COL.out
    local rows = wrap(tostring(text), W - PAD * 2)
    for _, row in ipairs(rows) do
        lines[#lines + 1] = { text = row, c = c }
    end
    while #lines > MAX_LINES do table.remove(lines, 1) end
    -- Scrolled up means reading something: hold that view rather than yanking
    -- it down every time a log line lands.
    if scroll > 0 then scroll = math.min(#lines, scroll + #rows) end
end

local function outf(fmt, ...) out(fmt:format(...)) end

-- --- the command table -----------------------------------------------------
--
-- Parked in the real globals rather than in this file's locals so it survives
-- a hot reload of THIS mod, and so any other mod can add to it. A mod that
-- wants its own command does, from its "start" handler (by which point every
-- mod has loaded, whatever order the files came in):
--
--     local con = rawget(_G, "ROBOX_CONSOLE")
--     if con then con.add("party", "", "toggle party mode", function() ... end) end
--
-- fn(args, rest) gets the whitespace-split arguments and the untouched tail,
-- and may return a string to print.
local C = rawget(_G, "ROBOX_CONSOLE")
if not C then
    C = { cmds = {} }
    _G.ROBOX_CONSOLE = C
end
C.print = out
C.printf = outf
function C.add(name, usage, help, fn)
    C.cmds[name] = { usage = usage or "", help = help or "", fn = fn }
end
local cmds = C.cmds

-- --- helpers ---------------------------------------------------------------

local function num(s, what)
    if not s then error(("missing %s"):format(what or "number"), 0) end
    local v = tonumber(s)
    if not v then error(("not a number: %s"):format(s), 0) end
    return v
end

local function addr(s) return num(s, "address") & 0xFFFFFFFF end

local function in_level()
    return robox.player.addr() ~= nil
end

local function need_player()
    local p = robox.player.addr()
    if not p then error("not in a level", 0) end
    return p
end

-- Show a value the way a console should: tables get one level opened out,
-- everything else goes through tostring.
local function show(v, depth)
    depth = depth or 0
    if type(v) ~= "table" or depth > 1 then
        return type(v) == "string" and depth > 0 and ("%q"):format(v) or tostring(v)
    end
    local parts, n = {}, 0
    for i, item in ipairs(v) do
        n = i
        parts[#parts + 1] = show(item, depth + 1)
        if i >= 12 then parts[#parts + 1] = "..." break end
    end
    for k, item in pairs(v) do
        if type(k) ~= "number" or k > n or k < 1 or k % 1 ~= 0 then
            parts[#parts + 1] = ("%s=%s"):format(tostring(k), show(item, depth + 1))
            if #parts >= 16 then parts[#parts + 1] = "..." break end
        end
    end
    return "{ " .. table.concat(parts, ", ") .. " }"
end

-- Split into arguments. Quoted runs stay together, so a path with a space in
-- it -- or a key called "Left Ctrl" -- is one argument.
local function split(s)
    local t, i = {}, 1
    while true do
        local _, b, quoted = s:find('^%s*"([^"]*)"', i)
        if b then
            t[#t + 1] = quoted
            i = b + 1
        else
            local _, b2, word = s:find("^%s*(%S+)", i)
            if not b2 then return t end
            t[#t + 1] = word
            i = b2 + 1
        end
    end
end

-- The rest of the line after its first argument, quotes and all removed.
local function tail(s)
    return (s:match('^%s*"[^"]*"%s*(.*)$') or s:match("^%s*%S+%s*(.*)$") or "")
end

-- --- running a line --------------------------------------------------------

local run   -- forward declaration: aliases and exec re-enter it

local function run_lua(src, print_result)
    -- Compiled in THIS mod's environment, so `robox` and everything else a mod
    -- can see is in scope -- the console is a mod, and its prompt should reach
    -- exactly as far as a mod does.
    local chunk, err = load("return " .. src, "=console", "t", _ENV)
    if not chunk then chunk, err = load(src, "=console", "t", _ENV) end
    if not chunk then out(err, "err") return end
    local res = table.pack(pcall(chunk))
    if not res[1] then out(tostring(res[2]), "err") return end
    if print_result then
        if res.n <= 1 then
            out("ok", "dim")
        else
            local parts = {}
            for i = 2, res.n do parts[#parts + 1] = show(res[i]) end
            out(table.concat(parts, "   "), "hi")
        end
    end
end

run = function(line, echo)
    line = line:gsub("^%s+", ""):gsub("%s+$", "")
    if line == "" then return end
    if echo ~= false then out("] " .. line, "hi") end

    -- `= expr` and `> stmt` are the two shorthands worth having.
    if line:sub(1, 1) == "=" then return run_lua(line:sub(2), true) end
    if line:sub(1, 1) == ">" then return run_lua(line:sub(2), false) end

    local name, rest = line:match("^(%S+)%s*(.*)$")
    local lower = name:lower()

    if alias[lower] then
        -- An alias is text substitution, arguments and all, so
        -- `alias dz "level 100; dance"` behaves like typing it.
        return run(alias[lower] .. (rest ~= "" and (" " .. rest) or ""), false)
    end

    local cmd = cmds[lower]
    if not cmd then
        out(("unknown command: %s   (try  help  or  = %s  to evaluate it)")
            :format(name, line), "err")
        return
    end

    -- A command prints for itself, or just returns a line and lets the console
    -- print it -- which is what makes a one-line command from another mod one
    -- line long.
    local ok, ret = pcall(cmd.fn, split(rest), rest)
    if not ok then out(tostring(ret), "err")
    elseif type(ret) == "string" then out(ret) end
end

-- Semicolons separate commands, which is what makes binds and aliases able to
-- do more than one thing.
local function run_all(line, echo)
    if not line:find(";", 1, true) then return run(line, echo) end
    for piece in line:gmatch("[^;]+") do run(piece, echo) end
end
C.run = run_all

-- --- built-in commands -----------------------------------------------------

C.add("help", "[command]", "this list, or detail on one command", function(a)
    if a[1] then
        local c = cmds[a[1]:lower()]
        if not c then out("no such command: " .. a[1], "err") return end
        outf("%s %s", a[1], c.usage)
        out("  " .. c.help, "dim")
        return
    end
    local names = {}
    for n in pairs(cmds) do names[#names + 1] = n end
    table.sort(names)
    out("commands -- help <name> for detail, cmds for the bare list", "dim")
    for _, n in ipairs(names) do
        outf("  %-12s %s", n, cmds[n].help)
    end
end)

C.add("cmds", "[filter]", "command names only, optionally filtered", function(a)
    local names = {}
    for n in pairs(cmds) do
        if not a[1] or n:find(a[1], 1, true) then names[#names + 1] = n end
    end
    table.sort(names)
    out(table.concat(names, "  "))
end)

C.add("clear", "", "empty the scrollback", function()
    lines, scroll = {}, 0
end)

C.add("echo", "<text>", "print the rest of the line", function(_, rest)
    out(rest)
end)

C.add("history", "", "the commands you have run", function()
    for i, h in ipairs(hist) do outf("%3d  %s", i, h) end
end)

C.add("quit", "", "exit the game", function()
    out("bye", "warn")
    os.exit(0)
end)

-- --- Lua -------------------------------------------------------------------

C.add("lua", "<code>", "run Lua in this mod's environment", function(_, rest)
    run_lua(rest, true)
end)

-- --- level -----------------------------------------------------------------

C.add("where", "", "current level id, name and robot flag", function()
    outf("level %d  %s%s", robox.level.id(), robox.level.name(),
         robox.level.is_robot() and "  (robot interior)" or "")
end)

C.add("level", "<id|name>", "warp to a level", function(a)
    if not a[1] then error("level <id|name> -- see  levels", 0) end
    local list = robox.level.list()
    local id, name = tonumber(a[1]), nil
    if id then
        for _, l in ipairs(list) do if l.id == id then name = l.name end end
    else
        local want = a[1]:lower()
        for _, l in ipairs(list) do                     -- exact name first
            if l.name:lower() == want then id, name = l.id, l.name break end
        end
        if not id then                                  -- then a partial match
            for _, l in ipairs(list) do
                if l.name:lower():find(want, 1, true) then
                    id, name = l.id, l.name
                    break
                end
            end
        end
    end
    if not id then error("no level matching " .. a[1], 0) end
    outf("warping to %d (%s)", id, name or "?")
    robox.level.load(id)
end)

C.add("levels", "[filter]", "list the levels", function(a)
    local row = {}
    for _, l in ipairs(robox.level.list()) do
        if not a[1] or l.name:lower():find(a[1]:lower(), 1, true) then
            row[#row + 1] = ("%d:%s"):format(l.id, l.name)
        end
    end
    if #row == 0 then out("nothing matches", "warn") return end
    out(table.concat(row, "  "))
end)

-- --- the robot -------------------------------------------------------------

local OFF_SEQ    = 0x11C     -- animation sequence, same field party_mode pins
local OFF_GROUND = 0x043     -- 1 = standing on something

C.add("pos", "", "where the robot is", function()
    need_player()
    outf("x %.1f  y %.1f   hp %s", robox.player.get("x"), robox.player.get("y"),
         tostring(robox.player.get("health")))
end)

C.add("tp", "<x> <y>", "teleport the robot", function(a)
    need_player()
    robox.player.set("x", num(a[1], "x"))
    robox.player.set("y", num(a[2], "y"))
    outf("moved to %.0f,%.0f", num(a[1]), num(a[2]))
end)

C.add("hp", "[n]", "read or set health", function(a)
    need_player()
    if a[1] then robox.player.set("health", num(a[1])) end
    outf("health %s", tostring(robox.player.get("health")))
end)

C.add("iframes", "<seconds>", "invulnerability timer", function(a)
    need_player()
    robox.player.set("iframes", num(a[1], "seconds"))
    outf("iframes %.1f", num(a[1]))
end)

C.add("anim", "<n>", "force an animation sequence (4 = BAILA)", function(a)
    local p = need_player()
    robox.mem.write_u32(p + OFF_SEQ, num(a[1], "sequence"))
    outf("sequence %d", num(a[1]))
end)

C.add("dance", "", "the unused BAILA sequence, held down", function()
    run("anim 4", false)
end)

C.add("fields", "", "the player fields the API exposes", function()
    local p = need_player()
    local names = {}
    for name in pairs(robox.player.fields()) do names[#names + 1] = name end
    table.sort(names)
    outf("CPlayer at %08X", p)
    for _, name in ipairs(names) do
        outf("  +%03X  %-9s %s", robox.player.fields()[name], name,
             tostring(robox.player.get(name)))
    end
end)

C.add("freeze", "[on|off]", "take the controls away from the player", function(a)
    local want = a[1] ~= "off"
    robox.input.block(want)
    outf("player input %s", want and "BLOCKED" or "free")
end)

-- --- memory and CPU --------------------------------------------------------

C.add("peek", "<addr> [count]", "hex dump guest memory", function(a)
    local va, n = addr(a[1]), math.min(num(a[2] or "16"), 256)
    if not robox.mem.valid(va, n) then error("not mapped: " .. a[1], 0) end
    for row = 0, math.ceil(n / 16) - 1 do
        local hex, asc = {}, {}
        for i = 0, 15 do
            local off = row * 16 + i
            if off < n then
                local b = robox.mem.u8(va + off)
                hex[#hex + 1] = ("%02x"):format(b)
                asc[#asc + 1] = (b >= 32 and b < 127) and string.char(b) or "."
            end
        end
        outf("%08X  %-47s  %s", va + row * 16,
             table.concat(hex, " "), table.concat(asc))
    end
end)

C.add("poke", "<addr> <value> [u8|u16|u32|f32]", "write guest memory", function(a)
    local va, v, ty = addr(a[1]), num(a[2], "value"), (a[3] or "u32"):lower()
    local w = { u8 = robox.mem.write_u8, u16 = robox.mem.write_u16,
                u32 = robox.mem.write_u32, f32 = robox.mem.write_f32 }
    if not w[ty] then error("type must be u8, u16, u32 or f32", 0) end
    w[ty](va, v)
    outf("%08X = %s (%s)", va, tostring(v), ty)
end)

C.add("str", "<addr>", "read a C string out of guest memory", function(a)
    out(("%q"):format(robox.mem.cstr(addr(a[1])) or "<unreadable>"))
end)

C.add("find", "<hex signature>", "byte scan, ?? wildcards, first hit", function(_, rest)
    if rest == "" then error('find "48 ?? 00 3f"', 0) end
    local hit = robox.mem.find(rest)
    if hit then outf("%08X", hit) else out("no match", "warn") end
end)

C.add("gpr", "[n]", "guest general-purpose registers", function(a)
    if a[1] then
        local n = num(a[1])
        outf("r%d = %08X", n, robox.cpu.gpr(n) & 0xFFFFFFFF)
        return
    end
    for base = 0, 28, 4 do
        local row = {}
        for i = base, base + 3 do
            row[#row + 1] = ("r%-2d %08X"):format(i, robox.cpu.gpr(i) & 0xFFFFFFFF)
        end
        out(table.concat(row, "  "))
    end
    outf("lr  %08X   ctr %08X", robox.cpu.lr() & 0xFFFFFFFF,
         robox.cpu.ctr() & 0xFFFFFFFF)
end)

-- --- audio -----------------------------------------------------------------

C.add("vol", "[0..100]", "master volume", function(a)
    if a[1] then robox.audio.volume(num(a[1])) end
    outf("volume %d", robox.audio.volume())
end)

C.add("music", "[on|off]", "the game's music", function(a)
    if a[1] then robox.audio.music(a[1] ~= "off") end
    outf("music %s", robox.audio.music() and "on" or "off")
end)

C.add("play", "<file>", "play an audio file over the game's music", function(a)
    if not a[1] then error("play <file> -- wav, ogg or rbxs", 0) end
    if robox.audio.music_play(a[1]) then out("playing " .. a[1], "ok")
    else out("could not open " .. a[1], "err") end
end)

C.add("stop", "", "stop a track the console or a mod put on", function()
    robox.audio.music_stop()
    out("stopped", "dim")
end)

C.add("nowplaying", "", "what the replacement-music path is streaming", function()
    local name, mine = robox.audio.music_playing()
    if not name then out("nothing -- the game's own music, or silence", "dim")
    else outf("%s  (%s)", name, mine and "a mod's own track" or "a mapped song") end
end)

-- --- the port itself -------------------------------------------------------

C.add("draws", "", "draw calls the game submitted last frame", function()
    outf("%d", robox.video.draws())
end)

C.add("bg", "<r> <g> <b> | off", "force the colour the game clears to", function(a)
    if not a[1] or a[1] == "off" then robox.video.clear() out("clear colour back to the game's", "dim")
    else robox.video.clear(num(a[1]), num(a[2]), num(a[3])) out("clear colour forced", "ok") end
end)

C.add("mods", "", "which C mods are enabled", function()
    local known = { "music", "wavmusic", "coop", "mario", "discord", "lua" }
    local on, off = {}, {}
    for _, id in ipairs(known) do
        if robox.enabled(id) then on[#on + 1] = id else off[#off + 1] = id end
    end
    out("on:  " .. (#on > 0 and table.concat(on, " ") or "-"), "ok")
    out("off: " .. (#off > 0 and table.concat(off, " ") or "-"), "dim")
end)

C.add("time", "", "uptime and frame count", function()
    outf("%.1f s, frame %d, level %s", robox.time(), robox.frame(),
         robox.level.name())
end)

-- --- binds, aliases, scripts -----------------------------------------------

local save_config      -- defined with the persistence code below

C.add("bind", "<key> <command>", "run a command on a key press", function(a, rest)
    if not a[1] then
        error('bind <key> <command>   -- key names are SDL\'s: F5, Home, "Left Ctrl"', 0)
    end
    local key  = a[1]
    local body = tail(rest)
    if body == "" then error("bind what? give it a command", 0) end
    binds[key] = body
    save_config()
    outf("%s -> %s", key, body)
end)

C.add("unbind", "<key>", "drop a binding", function(a)
    if not a[1] or not binds[a[1]] then error("nothing bound to " .. tostring(a[1]), 0) end
    binds[a[1]] = nil
    save_config()
    out("unbound " .. a[1], "dim")
end)

C.add("binds", "", "every key binding", function()
    local any = false
    for k, v in pairs(binds) do outf("  %-12s %s", k, v) any = true end
    if not any then out("nothing bound", "dim") end
end)

C.add("alias", "<name> <command>", "name a command line", function(a, rest)
    if not a[1] then error("alias <name> <command>", 0) end
    local name = a[1]:lower()
    local body = tail(rest)
    if body == "" then error("alias what? give it a command", 0) end
    if cmds[name] then error("that is already a command: " .. name, 0) end
    alias[name] = body
    save_config()
    outf("%s -> %s", name, body)
end)

C.add("unalias", "<name>", "drop an alias", function(a)
    local name = (a[1] or ""):lower()
    if not alias[name] then error("no alias " .. name, 0) end
    alias[name] = nil
    save_config()
    out("dropped " .. name, "dim")
end)

C.add("aliases", "", "every alias", function()
    local any = false
    for k, v in pairs(alias) do outf("  %-12s %s", k, v) any = true end
    if not any then out("no aliases", "dim") end
end)

C.add("exec", "<file>", "run a file of commands from the mod folder", function(a)
    if not a[1] then error("exec <file>", 0) end
    local path = robox.dir .. "/" .. a[1]
    local f = io.open(path, "r")
    if not f then error("cannot read " .. path, 0) end
    local n = 0
    for line in f:lines() do
        -- # comments only. Not // -- a line like `play https://host/x.ogg`
        -- would lose its URL, which is exactly the sort of thing an exec file
        -- is for.
        line = line:gsub("^%s+", ""):gsub("%s+$", "")
        if line ~= "" and line:sub(1, 1) ~= "#" then run_all(line, false) n = n + 1 end
    end
    f:close()
    outf("ran %d line%s from %s", n, n == 1 and "" or "s", a[1])
end)

-- --- the log tail ----------------------------------------------------------
--
-- The port's own running commentary -- [WAV], [video], [lua], and every mod's
-- robox.log -- goes to stderr, and src/main.c points stderr at logs/run.log.
-- So it is a file, and a file can be followed.
--
-- ONLY WITH RECOMP_LOG=1. A normal run sends stderr to the null device on
-- purpose (430-odd call sites of it), and then there is nothing to tail: an
-- old logs/run.log from some earlier debugging session would still open, seek
-- to its end and sit there showing nothing, which is worse than saying so.
-- Off by default even when it does work -- [AXMIX] alone would drown the
-- scrollback. `log on` when you want to watch something happen.

C.add("log", "[on|off]", "follow logs/run.log (needs RECOMP_LOG=1)", function(a)
    if a[1] == "off" then
        log_on = false
        if log_f then log_f:close() log_f = nil end
        out("log tail off", "dim")
        return
    end
    if not os.getenv("RECOMP_LOG") then
        out("nothing to follow: stderr goes to the null device unless the game "
            .. "is started with RECOMP_LOG=1", "warn")
        return
    end
    if log_f then log_f:close() end          -- `log on` twice must not leak it
    log_f = io.open("logs/run.log", "rb")
    if not log_f then out("RECOMP_LOG is set but logs/run.log will not open", "err") return end
    log_f:seek("end")                       -- from here on, not the whole file
    log_pos = log_f:seek()
    log_on  = true
    out("log tail on", "ok")
end)

local log_accum = 0
local function log_poll(dt)
    if not log_on or not log_f then return end
    log_accum = log_accum + dt
    if log_accum < 0.25 then return end
    log_accum = 0
    -- Re-seeking is what clears the EOF the last read set; without it the
    -- handle never notices the writer has appended anything.
    log_f:seek("set", log_pos)
    local chunk = log_f:read(16384)
    if not chunk or chunk == "" then return end
    log_pos = log_pos + #chunk
    for line in chunk:gmatch("[^\r\n]+") do out(line, "dim") end
end

-- --- persistence -----------------------------------------------------------

save_config = function()
    local t = { log = log_on }
    for k, v in pairs(binds) do t["bind:" .. k] = v end
    for k, v in pairs(alias) do t["alias:" .. k] = v end
    for i = math.max(1, #hist - MAX_HISTORY + 1), #hist do
        t[("hist:%03d"):format(i)] = hist[i]
    end
    robox.config.save(t)
end

local want_log = false          -- was the tail on last time? see the start handler

local function load_config()
    local t = robox.config.load()
    local h = {}
    for k, v in pairs(t) do
        local b = k:match("^bind:(.+)$")
        local a = k:match("^alias:(.+)$")
        local n = k:match("^hist:(%d+)$")
        if b then binds[b] = tostring(v)
        elseif a then alias[a] = tostring(v)
        elseif n then h[#h + 1] = { tonumber(n), tostring(v) }
        elseif k == "log" then want_log = (v == true) end
    end
    table.sort(h, function(x, y) return x[1] < y[1] end)
    for _, pair in ipairs(h) do hist[#hist + 1] = pair[2] end
end

-- --- editing ---------------------------------------------------------------

local function insert(s)
    input = input:sub(1, caret) .. s .. input:sub(caret + 1)
    caret = caret + #s
    hist_i = 0
end

local function complete()
    local head, word = input:sub(1, caret):match("^(.-)([%w_]*)$")
    if word == "" then return end
    local hits = {}
    for n in pairs(cmds) do if n:sub(1, #word) == word then hits[#hits + 1] = n end end
    for n in pairs(alias) do if n:sub(1, #word) == word then hits[#hits + 1] = n end end
    table.sort(hits)
    if #hits == 0 then return end
    if #hits == 1 then
        local tail = input:sub(caret + 1)
        input = head .. hits[1] .. " " .. tail
        caret = #head + #hits[1] + 1
        return
    end
    -- Several: extend to the longest common prefix and show the field, which
    -- is the behaviour every shell has trained everyone to expect.
    local pre = hits[1]
    for _, h in ipairs(hits) do
        while h:sub(1, #pre) ~= pre do pre = pre:sub(1, #pre - 1) end
    end
    if #pre > #word then
        input = head .. pre .. input:sub(caret + 1)
        caret = #head + #pre
    end
    out(table.concat(hits, "  "), "dim")
end

local function submit()
    local line = input
    input, caret, hist_i, scroll = "", 0, 0, 0
    if line:gsub("%s", "") == "" then return end
    if hist[#hist] ~= line then
        hist[#hist + 1] = line
        while #hist > MAX_HISTORY do table.remove(hist, 1) end
        save_config()
    end
    run_all(line)
end

local function browse(dir)
    if #hist == 0 then return end
    if hist_i == 0 then
        if dir > 0 then return end
        stash, hist_i = input, #hist
    else
        hist_i = hist_i + dir
        if hist_i > #hist then
            input, caret, hist_i = stash, #stash, 0
            return
        end
        if hist_i < 1 then hist_i = 1 end
    end
    input = hist[hist_i] or ""
    caret = #input
end

local function ctrl()
    return robox.input.key("Left Ctrl") or robox.input.key("Right Ctrl")
end

-- Everything that a held key should keep doing. Returns true if it handled the
-- key, which is also the "may repeat" answer.
local function edit_key(k)
    if k == "Backspace" then
        if caret > 0 then
            input = input:sub(1, caret - 1) .. input:sub(caret + 1)
            caret = caret - 1
        end
    elseif k == "Delete" then
        input = input:sub(1, caret) .. input:sub(caret + 2)
    elseif k == "Left" then
        caret = math.max(0, caret - 1)
    elseif k == "Right" then
        caret = math.min(#input, caret + 1)
    elseif k == "Up" then
        browse(-1)
    elseif k == "Down" then
        browse(1)
    elseif k == "PageUp" then
        scroll = math.min(#lines, scroll + 3)
    elseif k == "PageDown" then
        scroll = math.max(0, scroll - 3)
    else
        return false
    end
    return true
end

local function console_key(k)
    if k == "Escape" or k == "`" then
        C.close()
    elseif k == "Return" or k == "Keypad Enter" then
        submit()
    elseif k == "Tab" then
        complete()
    elseif k == "Home" then
        caret = 0
    elseif k == "End" then
        caret = #input
    elseif ctrl() and k == "L" then
        lines, scroll = {}, 0
    elseif ctrl() and k == "U" then
        input, caret = input:sub(caret + 1), 0
    elseif ctrl() and k == "C" then
        out("] " .. input, "dim")
        input, caret, hist_i = "", 0, 0
    elseif ctrl() and k == "A" then
        caret = 0
    elseif ctrl() and k == "E" then
        caret = #input
    elseif ctrl() and k == "W" then
        local head = input:sub(1, caret):gsub("%s*[^%s]*$", "")
        input = head .. input:sub(caret + 1)
        caret = #head
    elseif edit_key(k) then
        held[k] = -REPEAT_WAIT
    end
end

-- --- open / close ----------------------------------------------------------

function C.open()
    if open then return end
    open = true
    robox.input.capture(true)      -- from here the keyboard is ours alone
    scroll, held = 0, {}
end

function C.close()
    if not open then return end
    open = false
    robox.input.capture(false)
    held = {}
end

function C.toggle()
    if open then C.close() else C.open() end
end

C.add("close", "", "close the console", function() C.close() end)

-- --- events ----------------------------------------------------------------

robox.on("key", function(k, down)
    if not down then
        held[k] = nil
        return
    end
    if open then
        console_key(k)
    elseif k == "`" then
        C.open()
    elseif binds[k] then
        run_all(binds[k], false)
    end
end)

robox.on("text", function(s)
    -- The key that opens a console never types into it. (The font has no glyph
    -- for either character anyway -- see the header.)
    if not open or s == "`" or s == "~" then return end
    -- ASCII only, deliberately: the caret is a byte offset and the font has no
    -- glyph above 127, so accepting UTF-8 would mean a cursor that can land in
    -- the middle of a character to draw nothing.
    if s:find("[\128-\255]") then return end
    insert(s)
end)

robox.on("frame", function(dt)
    slide = math.max(0, math.min(1, slide + (open and dt or -dt) / SLIDE))
    log_poll(dt)

    if not open then return end
    -- Key repeat. The runtime reports edges, not repeats, so holding backspace
    -- would otherwise delete exactly one character.
    for k, t in pairs(held) do
        if not robox.input.key(k) then
            held[k] = nil                    -- missed the up edge somehow
        else
            t = t + dt
            if t >= REPEAT_RATE then
                edit_key(k)
                t = t - REPEAT_RATE
            end
            held[k] = t
        end
    end
end)

robox.on("unload", function() C.close() end)

-- --- drawing ---------------------------------------------------------------

local function ease(x) return 1 - (1 - x) * (1 - x) end

robox.on("draw", function()
    if slide <= 0 then return end
    local h  = H * HEIGHT
    local y0 = -h + h * ease(slide)          -- slides down from off the top
    local y1 = y0 + h

    robox.draw.rect(0, y0, W, h, 0.04, 0.05, 0.07, 0.90)
    robox.draw.rect(0, y1 - 2, W, 2, 0.30, 0.70, 1.00, 0.85)

    -- Title row: what this is on the left, what the game is doing on the right.
    local title = "ROBOX CONSOLE"
    robox.draw.text(PAD, y0 + 8, title, 0.30, 0.70, 1.00, 0.75, SCALE, TRACK)
    local right = ("%s  %d draws  %.0fs")
        :format(in_level() and robox.level.name() or "no level",
                robox.video.draws(), robox.time())
    robox.draw.text(W - PAD - robox.draw.text_width(right, SCALE * 0.9, TRACK),
                    y0 + 8, right, 0.45, 0.50, 0.58, 0.9, SCALE * 0.9, TRACK)

    -- Output, newest at the bottom, above the prompt.
    local top     = y0 + 8 + LINE_H + 4
    local prompt_y = y1 - LINE_H - 8
    local rows    = math.max(1, math.floor((prompt_y - top) / LINE_H))
    local last    = #lines - scroll
    local first   = math.max(1, last - rows + 1)
    local y       = prompt_y - LINE_H * (last - first + 1)
    for i = first, last do
        local l = lines[i]
        if l then
            robox.draw.text(PAD, y, l.text, l.c[1], l.c[2], l.c[3], 0.95, SCALE, TRACK)
        end
        y = y + LINE_H
    end

    if scroll > 0 then
        local tag = ("%d more below -- PgDn"):format(scroll)
        robox.draw.text(W - PAD - robox.draw.text_width(tag, SCALE * 0.9, TRACK),
                        prompt_y - LINE_H, tag, 1.0, 0.78, 0.30, 0.9,
                        SCALE * 0.9, TRACK)
    end

    -- The prompt. Quake's ] because this is that console.
    robox.draw.rect(0, prompt_y - 3, W, 1, 0.30, 0.70, 1.00, 0.25)
    local pre = "] " .. input:sub(1, caret)
    robox.draw.text(PAD, prompt_y, "] " .. input, 1, 1, 1, 1, SCALE, TRACK)
    if (robox.time() * 2) % 1 < 0.6 then
        robox.draw.rect(PAD + robox.draw.text_width(pre, SCALE, TRACK),
                        prompt_y + 2, 9, LINE_H - 8, 0.30, 0.70, 1.00, 0.85)
    end
end)

-- --- start -----------------------------------------------------------------

robox.on("start", function()
    load_config()
    out("ROBOX console -- ` opens and closes it", "hi")
    out("help lists the commands, = <expr> evaluates Lua, Tab completes", "dim")
    local n = 0
    for _ in pairs(binds) do n = n + 1 end
    if n > 0 then outf("%d key bind%s loaded", n, n == 1 and "" or "s") end
    -- The tail is a saved setting, but it has to be turned on through the
    -- command: that is what opens the file and seeks to the end.
    if want_log then run("log on", false) end
    robox.log("console ready -- ` opens it (Escape still opens the settings menu)")
end)
