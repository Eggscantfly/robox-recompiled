-- mods/lua/party_mode.lua -- PARTY MODE. F6 toggles it.
--
-- Warps to 01a, finds a spot in the void outside the level, teleports the
-- robot there and pins him to the floor, then brings the lights up on him
-- dancing against a black nothing.
--
-- WHERE THE DANCE CAME FROM
-- Assets/anim/robot.ani carries 98 NAMED sequences -- PARADO, camina, doble
-- salto, muerte, zombie, ERUCTO, CAGA, PEDO -- and at index 4, BAILA. It
-- shipped in the retail game and nothing ever plays it.
--
-- Every address below was found by scripting, not by disassembly:
--
--   +0x11C  current animation sequence. Sampled every word of CPlayer for
--           forty seconds and kept the ones that stayed in range and moved;
--           this one came back 0, 11, 12, 14, 15, 24, 27, 28 -- PARADO, se
--           agacha, en el aire salto, aterriza salto, trote, camina, camina 2
--           -- exactly what the robot was doing. Writing 4 to it made the
--           keyframe counter at +0x124 jump from 0..5 to 0..19, BAILA being
--           much longer than PARADO.
--
--   +0x043  on the ground, one byte. Scanned every BYTE for a 0/1 value and
--           scored each against "is the current sequence one of the airborne
--           ones (12/13/14/16/17)". This agreed 86% of the time -- the missing
--           14% is the landing frames, where the animation and the actual
--           contact do not line up. Nothing else came close.
--
--   0x802086EC / F0  the camera. Two-pass value scan: pin the player at a
--           known X, sweep 48 MB of guest RAM for floats near it, move the pin
--           5000 and keep whatever followed. Eight survived out of 971 and
--           seven were inside CPlayer; this was the odd one out. The globals
--           around it are 12-byte (vtable, x, y) vectors, so Y is the next
--           word. Walking the player out in 700-unit steps, it tracked to
--           14916 and stopped dead while he carried on to 17443 -- the level's
--           camera-limit polygon. Hence pinning it by hand.
--
-- The engine reasserts all of these every frame, so they are pins, not pokes.
-- Stop writing and everything is normal again on the next frame.

-- --- tuning ---------------------------------------------------------------
local BUILDUP     = 5.0    -- seconds of darkness before the drop
local FADE_IN     = 2.0    -- seconds for the lights to come up after the drop

-- NO AUDIO FILE REQUIRED. The lights, the banner bump and the analyser all
-- come from robox.audio.beat/level/spectrum, and those read the FINAL MIX in
-- sdk/peripherals.c -- sampled after robox_synth_render (the game's own music)
-- and robox_wav_render (music packs) have both been added in. So this reacts
-- to whatever is playing: the game's original soundtrack out of the box, a
-- track mapped in mods/wav_music.cfg, or the playlist below. Nothing in the
-- light rig knows or cares which.
--
-- Fallback tempo, used only while the audio is silent -- a loading gap, or
-- music switched off in the settings menu. 120 is the port's own music clock,
-- so it never visibly fights the game's soundtrack, and any real track takes
-- over the moment there is a transient to follow.
local BPM         = 120

-- --- the playlist ----------------------------------------------------------
--
-- Direct links to audio files. Each one is downloaded ONCE into
-- mods/wavs/.cache and played from there ever after, so the mod stays a text
-- file with nothing to ship alongside it. They play in order, back to back,
-- for as long as the party runs; F7 skips to the next one.
--
-- WAV, Ogg Vorbis, MP3 or RBXS. MP3 is decoded to WAV on download (a
-- three-minute track lands around 30 MB in the cache -- worth knowing before
-- queueing twenty of them). Ogg must be Vorbis-ONLY: a file carrying a video
-- stream on page 0 (album art counts) fails to open, and .opus needs a real
-- re-encode rather than a rename.
--
-- MUSIC_QUEUE ships EMPTY on purpose. Point it at things you have the right
-- to use and redistribute -- your own recordings, or Creative Commons or
-- public-domain tracks. A link is a distribution channel: whatever is put
-- here is fetched by everyone who installs the mod.
--
-- Empty, the party simply runs on the game's own music, exactly as before.
local MUSIC_QUEUE = {
    -- "https://example.com/your%20track.mp3",
}
local SHUFFLE     = false  -- true = shuffle the order once, at startup
local MUSIC_DIR   = "mods/wavs/.cache/"   -- cached files land here, named by
                                          -- URL hash and no extension: the
                                          -- player detects format by content

-- How the playlist gets heard. NOT mods/wav_music.cfg and NOT music_set: both
-- of those replace one of the GAME's songs, and only take effect the next time
-- the game starts one -- no use to something that has to change track every
-- three minutes. robox.audio.music_play puts a file on directly, and while it
-- runs the game's own music is muted (not stopped) in sdk/peripherals.c. So
-- turning the party off hands the soundtrack straight back, mid-bar, with
-- nothing to put back -- the same "pins, not pokes" deal as everything above.
local FORCE_LEVEL = 100    -- warp here first (100 = 01a). nil = party in place.
local TELEPORT_Y  = 0

-- How far out to stand. A Robox level is a 50 x 50 grid of 420 x 320 sectors
-- (straight from the .lev header, exactly as the engine's author described
-- it), so the world is 21000 x 16000 -- but the 14 layers in 01a each scroll
-- at their own parallax rate and the z=50 backdrop runs at 0.100, so THAT one
-- does not run out until 21000 / 0.1 = 210000.
--
-- Go too far, though, and the engine stops drawing the robot as well: past the
-- sector grid he is not in any sector to be drawn from. So the distance is not
-- a constant to guess at -- it is measured. During the build-up, while the
-- screen is black anyway, each candidate is tried for a third of a second and
-- robox.video.draws() says how much geometry the game actually submitted. The
-- winner is the one with the FEWEST non-zero draws: nothing left of the level,
-- but something still on screen, and that something is the robot.
local CANDIDATES = { 220000, 120000, 60000, 30000, 24000, 22000, 1500 }
local PROBE_TIME = 0.34

-- --- guest addresses ------------------------------------------------------
local OFF_GROUND   = 0x043          -- CPlayer, u8, 1 = standing on something
local OFF_SEQ      = 0x11C          -- CPlayer, animation sequence index
local SEQ_BAILA    = 4              -- "BAILA"
local CAM_X, CAM_Y = 0x802086EC, 0x802086F0

local BEAT = 60 / BPM

local enabled = true
local state   = "idle"        -- idle | buildup | party
local t       = 0
local home, spot, cam_off, warped = nil, nil, nil, false
local cal_i, cal_t, cal_draws = 1, 0, {}
local now_playing = nil       -- name of the playlist track on now, for the
                              -- overlay. Declared up here because the draw
                              -- handler below closes over it and the queue
                              -- that sets it is written further down.

-- --- shapes ---------------------------------------------------------------
--
-- robox.draw.tri is the only non-rectangular primitive the overlay exposes: a
-- triangle with a colour per corner. Everything below is built out of it in
-- script, which is the right place for it -- these are composition, not
-- rendering, and they are cheap to change without a rebuild.

-- Soft round glow: a fan whose centre is opaque and whose rim is transparent,
-- so it falls off instead of ending. The thing rectangles cannot fake.
local function glow(cx, cy, radius, r, g, b, a, segs)
    segs = segs or 20
    local prev_x, prev_y
    for i = 0, segs do
        local th = i / segs * math.pi * 2
        local x, y = cx + math.cos(th) * radius, cy + math.sin(th) * radius
        if prev_x then
            robox.draw.tri(cx, cy, prev_x, prev_y, x, y,
                           r, g, b, a,      -- centre: full
                           r, g, b, 0,      -- rim: gone
                           r, g, b, 0)
        end
        prev_x, prev_y = x, y
    end
end

-- Light beam: a quad that starts narrow and bright and opens out into nothing,
-- the way a spotlight in smoke actually looks.
local function beam(x0, y0, w0, x1, y1, w1, r, g, b, a0, a1)
    local dx, dy = x1 - x0, y1 - y0
    local len = math.sqrt(dx * dx + dy * dy)
    if len < 0.001 then return end
    local nx, ny = -dy / len, dx / len          -- unit normal
    local ax, ay = x0 + nx * w0, y0 + ny * w0
    local bx, by = x0 - nx * w0, y0 - ny * w0
    local cx, cy = x1 + nx * w1, y1 + ny * w1
    local dx2, dy2 = x1 - nx * w1, y1 - ny * w1
    robox.draw.tri(ax, ay, bx, by, cx, cy, r,g,b,a0, r,g,b,a0, r,g,b,a1)
    robox.draw.tri(bx, by, dx2, dy2, cx, cy, r,g,b,a0, r,g,b,a1, r,g,b,a1)
end

local function hsv(h, s, v)
    h = (h % 1) * 6
    local i = math.floor(h)
    local f = h - i
    local p, q, w = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    if     i == 0 then return v, w, p
    elseif i == 1 then return q, v, p
    elseif i == 2 then return p, v, w
    elseif i == 3 then return p, q, v
    elseif i == 4 then return w, p, v
    else               return v, p, q end
end

-- The title screen and every loading gap have no CPlayer -- the pointer at
-- 0x801FACE0 is null until a level is up. That is the whole in-a-level test.
local function in_level() return robox.player.addr() ~= nil end

local function go_idle(put_him_back)
    if put_him_back and home and robox.player.addr() then
        robox.player.set("x", home.x)
        robox.player.set("y", home.y)
    end
    -- Hand the clear colour back to the game, and give the player his controls
    -- back. The camera needs nothing put back: stop writing and the engine
    -- owns it again next frame.
    robox.video.clear()
    robox.input.block(false)
    -- And the music. Only ever OUR track: the game's own song is merely muted
    -- while the playlist runs, so it is audible again on the very next frame,
    -- and a music-pack song someone mapped in wav_music.cfg is not ours to
    -- stop. Second return of music_playing is exactly that question.
    local _, mine = robox.audio.music_playing()
    if mine then robox.audio.music_stop() end
    now_playing = nil
    state, t, home, spot, cam_off = "idle", 0, nil, nil, nil
    cal_i, cal_t, cal_draws = 1, 0, {}
end

-- Everything that has to be reasserted every frame, wherever he is standing.
local function pin(x, y)
    local p = robox.player.addr()
    if not p then return end
    robox.player.set("x", x)
    robox.player.set("y", y)
    robox.mem.write_u8(p + OFF_GROUND, 1)     -- never falls, so never dies
    robox.player.set("iframes", 60.0)
    robox.mem.write_u32(p + OFF_SEQ, SEQ_BAILA)
    if cam_off then                            -- after the engine's own clamp
        robox.mem.write_f32(CAM_X, x + cam_off.x)
        robox.mem.write_f32(CAM_Y, y + cam_off.y)
    end
end

robox.on("key", function(key, down)
    if key ~= "F6" or not down then return end
    enabled = not enabled
    if not enabled then
        go_idle(true)
        warped = false
        robox.notify("party over")
    else
        robox.notify(in_level() and "PARTY MODE" or "party mode armed -- start a level")
    end
end)

robox.on("level", function() if state ~= "idle" then go_idle(false) end end)

robox.on("frame", function(dt)
    if not enabled then return end

    if not in_level() then
        if state ~= "idle" then go_idle(false) end
        return
    end

    -- Party in one known place. robox.level.load() is the engine's own level
    -- change (func_80060bf0), the same call its restart path makes, so this is
    -- a real transition. Guarded so a target that never arrives is not a loop.
    if FORCE_LEVEL and robox.level.id() ~= FORCE_LEVEL then
        if not warped then
            warped = true
            robox.log(("forcing level %d (was %d, %s)")
                      :format(FORCE_LEVEL, robox.level.id(), robox.level.name()))
            if state ~= "idle" then go_idle(false) end
            robox.level.load(FORCE_LEVEL)
        end
        return
    end
    warped = false

    if state == "idle" then state, t, home = "buildup", 0, nil end

    t = t + dt
    if state == "buildup" and t >= BUILDUP then
        state, t = "party", 0
        -- Controls off, but only now -- during the build-up you can still walk
        -- around, and the warp and the staging both need the game behaving
        -- normally. Esc and F6 are host keys and keep working, so this is
        -- never a lock-in.
        robox.input.block(true)
        robox.log("controls off for the party (F6 or Esc still work)")
    end

    -- Latch where he is and how the camera frames him -- but only once the
    -- camera is alive. CPlayer appears a beat before it does, so reading it on
    -- the first frame in a level gets 0,0 and an "offset" of minus the whole
    -- level.
    if not home then
        local px, py = robox.player.get("x"), robox.player.get("y")
        local cx, cy = robox.mem.f32(CAM_X), robox.mem.f32(CAM_Y)
        local sane = px and cx and cy and cx ~= 0
                     and math.abs(cx - px) < 3000 and math.abs(cy - py) < 3000
        if not (sane or t > 2.0) then return end     -- still waiting, stay black
        home    = { x = px, y = py }
        cam_off = sane and { x = cx - px, y = cy - py } or { x = 0, y = 0 }
        -- Black from here on. Out past the level there is no geometry left, so
        -- what shows is the game's own EFB clear colour -- and Robox sets that
        -- to 0x802b94, purple. It cannot be covered with an overlay rectangle,
        -- because the overlay draws over the finished frame and would hide the
        -- robot with it. So change the colour the game clears TO.
        robox.video.clear(0, 0, 0)
        robox.log(("party staging from %.0f,%.0f (camera offset %.0f,%.0f)")
                  :format(px, py, cam_off.x, cam_off.y))
    end

    -- Measure, do not guess. See CANDIDATES above.
    if not spot then
        local dx = CANDIDATES[cal_i]
        pin(home.x + dx, home.y + TELEPORT_Y)
        cal_t = cal_t + dt
        if cal_t >= PROBE_TIME then
            cal_draws[cal_i] = robox.video.draws()
            cal_i, cal_t = cal_i + 1, 0
            if cal_i > #CANDIDATES then
                local best, best_draws
                for i, off in ipairs(CANDIDATES) do
                    local d = cal_draws[i] or 0
                    if d > 0 and (not best_draws or d < best_draws) then
                        best, best_draws = off, d
                    end
                end
                local parts = {}
                for i, off in ipairs(CANDIDATES) do
                    parts[#parts + 1] = ("+%d:%d"):format(off, cal_draws[i] or 0)
                end
                robox.log("draw counts by distance -- " .. table.concat(parts, "  "))
                spot = { x = home.x + (best or 1500), y = home.y + TELEPORT_Y }
                robox.log(("party at %.0f,%.0f  (+%d, %d draws -- the robot and "
                           .. "nothing else)"):format(spot.x, spot.y,
                                                      best or 1500, best_draws or 0))
            end
        end
        return
    end

    pin(spot.x, spot.y)
end)

robox.on("draw", function()
    if not enabled or state == "idle" then return end

    local W, H  = robox.draw.W, robox.draw.H
    local beat  = t / BEAT
    local phase = beat % 1

    -- The bump. robox.audio.beat() is a transient detector on the real mix, so
    -- this is genuinely on the song's hits, not on a clock that happens to
    -- start in the right place and slide out of it over three minutes. The
    -- free-running clock is kept only as a floor, so the lights still move
    -- during a silent stretch.
    local punch = math.max(robox.audio.beat(), ((1 - phase) ^ 3) * 0.30)
    local loud  = robox.audio.level()

    -- ---- the build-up: black, with strobes coming up underneath ----------
    if state == "buildup" then
        local k = t / BUILDUP
        if phase < 0.05 + 0.05 * k then
            local r, g, b = hsv(t * 0.6, 0.5 * k, 1)
            robox.draw.rect(0, 0, W, H, r, g, b, 0.25 + 0.5 * k)
        end
        robox.draw.rect(0, 0, W, H, 0, 0, 0, 1 - k * k)

        local label = "PARTY MODE"
        local scale = 1.2 + 0.9 * k
        local w = robox.draw.text_width(label, scale, 6)
        local r, g, b = hsv(t * 0.9, 0.7, 1)
        robox.draw.text((W - w) / 2, H * 0.42, label, r, g, b, k * k, scale, 6)
        return
    end

    -- ---- the party -------------------------------------------------------
    -- Fade the whole rig up rather than snapping it on at the drop.
    local fade = math.min(1, t / FADE_IN)

    -- Everything here is translucent on purpose: the robot is drawn by the
    -- GAME, into the frame this paints over, so anything opaque erases him.

    -- Six moving-head spots swinging from the ceiling, each opening out as it
    -- goes down. Beams, not stripes -- they have a source and a spread.
    for i = 0, 5 do
        local sway = math.sin(t * 0.9 + i * 1.7) * 0.5 + 0.5
        local ox   = (i + 0.5) / 6 * W
        local tx   = sway * W
        local r, g, b = hsv(i / 6 + t * 0.12, 0.95, 1)
        beam(ox, -20, 14, tx, H + 40, 120,
             r, g, b, (0.16 + 0.14 * loud) * fade, 0)
        glow(ox, 0, 46 + 26 * punch, r, g, b, 0.5 * fade, 16)
    end

    local r, g, b = hsv(t * 0.22, 0.85, 1) -- the whole room takes a colour
    robox.draw.rect(0, 0, W, H, r, g, b, (0.07 + 0.13 * punch) * fade)

    if punch > 0.55 and phase < 0.5 then   -- strobe on the loud hits only
        robox.draw.rect(0, 0, W, H, 1, 1, 1, 0.16 * punch * fade)
    end

    for i = 0, 3 do                        -- corner lamps, off-phase
        local pulse = math.max(0, math.sin((beat + i * 0.25) * math.pi * 2))
        pulse = math.max(pulse * 0.4, punch)
        local cr, cg, cb = hsv(t * 0.4 + i / 4, 0.9, 1)
        glow(i % 2 == 0 and 0 or W, i < 2 and 0 or H,
             200 + 90 * pulse, cr, cg, cb, 0.30 * pulse * fade, 18)
    end

    -- The banner. No panel behind it -- a black box reads as UI chrome, and
    -- this is meant to be part of the scene -- so readability over a bright
    -- disco comes from outlining the glyphs instead.
    -- The text bumps on the audio: the scale IS the transient, so it kicks on
    -- the snare and settles between hits.
    local label = "PARTY MODE"
    local blink = (0.75 + 0.25 * loud) * fade
    local scale = 1.6 + 0.55 * punch
    local w     = robox.draw.text_width(label, scale, 6)
    local x     = (W - w) / 2
    local y     = 90 + 10 * math.sin(t * 5) - 18 * punch

    for k = 0, 7 do
        local a = k * math.pi / 4
        robox.draw.text(x + math.cos(a) * 4, y + math.sin(a) * 4, label,
                        0, 0, 0, 0.5 * blink, scale, 6)
    end
    local tr, tg, tb = hsv(t * 0.9, 0.7, 1)
    robox.draw.text(x, y, label, tr, tg, tb, blink, scale, 6)

    -- Equaliser, clear of the glyphs: y is the TOP of the text and it is about
    -- 85 tall at this scale, which is why an earlier strip at y+34 drew
    -- straight through the middle of the word.
    -- The original equaliser strip: 16 flat bars the width of the banner,
    -- solid colour, growing up from a fixed baseline. Same geometry as before
    -- -- only the heights changed hands. They used to come from a sine; now
    -- each one is a band of robox.audio.spectrum(), a Hann-windowed
    -- 1024-point FFT of the real mix folded into log-spaced bands. Bass on the
    -- left. Real data, old look.
    local BANDS = 16
    local spec  = robox.audio.spectrum(BANDS)
    local bw    = (w + 80) / BANDS
    for i = 0, BANDS - 1 do
        local h = 6 + 46 * spec[i + 1]
        local gr, gg, gb = hsv(i / BANDS + t * 0.5, 0.85, 1)
        robox.draw.rect(x - 40 + i * bw + 3, y + 122 + (52 - h), bw - 6, h,
                        gr, gg, gb, 0.85 * fade)
    end

    -- What is on. Under the analyser, small, outlined the same way the banner
    -- is -- there is no panel to read it against, only a moving disco.
    if now_playing then
        local ny = y + 190
        local nw = robox.draw.text_width(now_playing, 0.75, 6)
        local nx = (W - nw) / 2
        for k = 0, 3 do
            local a = k * math.pi / 2
            robox.draw.text(nx + math.cos(a) * 3, ny + math.sin(a) * 3,
                            now_playing, 0, 0, 0, 0.6 * fade, 0.75, 6)
        end
        robox.draw.text(nx, ny, now_playing, 1, 1, 1, 0.85 * fade, 0.75, 6)
    end
end)

robox.on("unload", function() go_idle(true) end)

-- --- the queue -------------------------------------------------------------
--
-- Each entry is downloaded once and cached, and the whole thing is
-- fire-and-forget: the party never waits on the network. It starts on
-- whatever has already arrived -- including nothing, in which case the lights
-- run on the game's own soundtrack -- and folds the rest in as they land.

local q       = {}      -- { url, file, name, ready, bad } in play order
local q_i     = 0       -- index of the track on now (0 = none yet)
local q_retry = 0       -- seconds since the last attempt to put one on
local dl, dl_i          -- the one download in flight, and whose it is

local function have_file(p)
    local f = io.open(p, "rb")
    if f then f:close() return true end
    return false
end

-- Cache name. FNV-1a of the URL, so a file is reused across edits to the list
-- (reordering, inserting) and a CHANGED link is a different file rather than
-- the old audio under a new name. No extension: format is detected by content.
local function cache_name(url)
    local h = 2166136261
    for i = 1, #url do
        h = ((h ~ url:byte(i)) * 16777619) % 4294967296
    end
    return ("%sparty_%08x"):format(MUSIC_DIR, h)
end

local function urldecode(s)
    return (s:gsub("%%(%x%x)", function(hex)
        return string.char(tonumber(hex, 16))
    end))
end

-- Something to put on screen. The last path segment, percent-decoded, minus
-- the extension and minus a leading track number ("92. ", "1-42. ", "1.10 ").
local function track_name(url)
    local path = url:gsub("[?#].*$", "")
    local base = urldecode(path:match("([^/]+)$") or path)
    base = base:gsub("%.%w+$", ""):gsub("^%d[%d%-%.]*%s+", "")
    return base ~= "" and base or url
end

for _, url in ipairs(MUSIC_QUEUE) do
    q[#q + 1] = { url = url, file = cache_name(url), name = track_name(url),
                  ready = have_file(cache_name(url)) }
end
if SHUFFLE then
    math.randomseed(os.time())
    for i = #q, 2, -1 do                      -- Fisher-Yates, once, at load
        local j = math.random(i)
        q[i], q[j] = q[j], q[i]
    end
end

local function put_on(i)
    local e = q[i]
    if not e or not robox.audio.music_play(e.file) then
        if e then e.bad = true end            -- cached file will not open
        return false
    end
    q_i, now_playing = i, e.name
    robox.notify("now playing: " .. e.name)
    robox.log(("track %d/%d -- %s"):format(i, #q, e.name))
    return true
end

-- The next track that has actually arrived, wrapping. Deliberately skips the
-- ones still downloading rather than waiting on them, so a slow track at
-- position 2 does not stall the party -- it gets picked up on the next lap.
local function next_ready(from)
    for k = 1, #q do
        local i = (from + k - 1) % #q + 1
        if q[i].ready and not q[i].bad then return i end
    end
end

robox.on("frame", function(dt)
    -- One download at a time, in play order. NET_MAX is 4 slots shared by
    -- every mod, and a dozen parallel transfers would only make each of them
    -- slower anyway.
    if dl then
        local st, got, _, err = robox.net.status(dl)
        if st ~= "working" then
            if st == "done" then
                q[dl_i].ready = true
                robox.log(("cached %s (%d bytes) -> %s")
                          :format(q[dl_i].name, got, q[dl_i].file))
            else
                q[dl_i].bad = true
                robox.log(("download failed for %s: %s")
                          :format(q[dl_i].name, tostring(err)))
            end
            robox.net.release(dl)
            dl, dl_i = nil, nil
        end
    elseif enabled then                        -- party off: stop pulling more
        for i, e in ipairs(q) do
            if not e.ready and not e.bad then
                dl = robox.net.fetch(e.url, e.file)
                if dl then
                    dl_i = i
                    robox.log("downloading " .. e.name .. "...")
                end
                break                          -- no slot: try again next frame
            end
        end
    end

    if #q == 0 or not enabled or state == "idle" then return end

    -- Is OUR track still running? music_playing's second return is the whole
    -- point: the game starting its own song (a level load) reads as "playing"
    -- too, and that must not be mistaken for the playlist still going.
    local _, mine = robox.audio.music_playing()
    if mine then return end

    q_retry = q_retry + dt                     -- a file that will not open must
    if q_retry < 0.5 then return end           -- not be retried every frame
    q_retry = 0
    local nxt = next_ready(q_i)
    if nxt then put_on(nxt) else now_playing = nil end
end)

-- Skip. next_ready wraps, so with one cached track this restarts it.
local function skip()
    if #q == 0 then return "the playlist is empty" end
    local nxt = next_ready(q_i)
    if not nxt then
        robox.notify("nothing in the queue has downloaded yet")
        return "nothing has downloaded yet"
    end
    if not put_on(nxt) then
        robox.audio.music_stop()               -- the file would not open
        return "that file would not open"
    end
    return "skipped to " .. q[nxt].name
end

-- F7, but ONLY while a party is running: sdk/video.c owns F7 the rest of the
-- time (d-pad rotation for the robot-interior levels), and two things on one
-- key is a bug even when the two can rarely happen at once. During a party the
-- level is forced to 01a, which is not a robot interior, so there is no overlap
-- left at all.
robox.on("key", function(key, down)
    if key == "F7" and down and state ~= "idle" then skip() end
end)

robox.on("start", function()
    local have = 0
    for _, e in ipairs(q) do if e.ready then have = have + 1 end end
    if #q > 0 then
        robox.log(("playlist: %d track%s, %d already cached")
                  :format(#q, #q == 1 and "" or "s", have))
    end
    robox.log("F6 toggles PARTY MODE -- it waits until you are in a level"
              .. (#q > 0 and ".  F7 skips a track." or ""))

    -- Console commands, if mods/lua/console.lua is installed. Registered from
    -- "start" rather than at load: every mod's chunk has run by then, so this
    -- works whichever order the two files were discovered in.
    local con = rawget(_G, "ROBOX_CONSOLE")
    if not con then return end

    con.add("party", "[on|off]", "toggle PARTY MODE", function(a)
        local want = a[1] and a[1] ~= "off" or not enabled
        if want == enabled then return ("already %s"):format(want and "on" or "off") end
        enabled = want
        if not enabled then
            go_idle(true)
            warped = false
        end
        return enabled and (in_level() and "PARTY MODE" or "armed -- start a level")
                        or "party over"
    end)

    con.add("skip", "", "next track in the party playlist", function()
        return skip()
    end)

    con.add("playlist", "", "the party playlist and what has downloaded",
    function()
        if #q == 0 then return "MUSIC_QUEUE is empty" end
        for i, e in ipairs(q) do
            con.printf("%s%2d  %-9s %s", i == q_i and ">" or " ", i,
                       e.bad and "FAILED" or e.ready and "cached" or "...",
                       e.name)
        end
    end)
end)
