-- mods/lua/spawn_counter.lua -- what the level just spawned, by entity type.
--
-- The one that shows what hooks are for. func_800b3144 is the engine's generic
-- entity init: r3 is the new entity, r4 the spawn-parameter blob it is being
-- built from, and it writes the entity's type id to `this + 0x1d8`. Every
-- object in a level goes through it, which makes it the cheapest possible
-- window onto what a level is made of.
--
-- (Same function sdk/robox_coop.c hooks to find the player. Type 0x32 IS the
-- player -- the game has no player singleton, just an entity whose level data
-- happens to say 50. Which is exactly what the engine's author said it would
-- be, so it is a good check that we are reading the right field.)
--
-- F10 clears the tally.

local ENT_INIT   = 0x800b3144
local OFF_TYPE   = 0x1d8
local TYPE_PLAYER = 0x32

local counts, total = {}, 0

-- Names for the handful of types that have been identified so far. Everything
-- else is reported by number; adding a row here is how it stops being one.
local NAMES = {
    [0x32]  = "player",
    [0x12c] = "amber",        -- 300, per the engine author's own example
}

robox.on("start", function()
    -- "after", not "before": the type id we want is written by the function
    -- we are hooking, so there is nothing to read until it has run.
    robox.hook_after(ENT_INIT, function(this)
        if not robox.mem.valid(this, OFF_TYPE + 4) then return end
        local t = robox.mem.u32(this + OFF_TYPE)
        counts[t] = (counts[t] or 0) + 1
        total = total + 1
    end)
    robox.log(string.format("hooked entity init at 0x%08X", ENT_INIT))
end)

robox.on("level", function(id, name)
    counts, total = {}, 0
    robox.log(string.format("level %s (%d) -- tally reset", name, id))
end)

robox.on("key", function(key, down)
    if key == "F10" and down then
        counts, total = {}, 0
        robox.notify("spawn tally cleared")
    end
end)

robox.on("draw", function()
    if total == 0 then return end

    -- Sort by count so the interesting rows are at the top, not wherever the
    -- hash iteration happens to put them.
    local rows = {}
    for t, n in pairs(counts) do rows[#rows + 1] = { t = t, n = n } end
    table.sort(rows, function(a, b) return a.n > b.n end)

    local shown = math.min(#rows, 12)
    local x, y = robox.draw.W - 250, 60

    robox.draw.rect(x, y, 226, 26 * (shown + 1) + 16, 0, 0, 0, 0.55)
    robox.draw.text(x + 12, y + 18, string.format("spawned  %d", total),
                    0.6, 0.9, 0.7, 1, 0.5)

    for i = 1, shown do
        local r = rows[i]
        local label = NAMES[r.t] or string.format("type %d (0x%x)", r.t, r.t)
        local py = y + 18 + i * 26
        local hot = r.t == TYPE_PLAYER
        robox.draw.text(x + 12, py, label,
                        hot and 1 or 0.85, hot and 0.8 or 0.85, hot and 0.4 or 0.85,
                        1, 0.48)
        robox.draw.text(x + 190, py, tostring(r.n), 1, 1, 1, 1, 0.48)
    end
end)

-- Hooks are released automatically when this file is saved and reloaded --
-- the runtime restores every dispatch-table entry the mod patched. This
-- handler is only here for anything the runtime cannot know about.
robox.on("unload", function()
    robox.log("dropping entity-init hook")
end)
