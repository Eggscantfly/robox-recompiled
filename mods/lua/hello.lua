-- mods/lua/hello.lua -- the smallest useful mod.
--
-- Save this file while the game is running and watch the text change. That is
-- the whole point of the runtime: no rebuild, no restart.
--
-- Rename it to _hello.lua to turn it off.

robox.on("start", function()
    robox.notify("hello from " .. robox.name)
    robox.log("loaded from", robox.file)
end)

robox.on("draw", function()
    local t = string.format("%.1f s   frame %d", robox.time(), robox.frame())
    robox.draw.text(24, 24, "ROBOX + Lua   " .. t, 1, 1, 1, 0.9, 0.6)
end)

-- A handler the GAME's own Lua can call. See mods/lua/guest/hello.lua for the
-- other half; nothing happens unless a guest script is present.
robox.game.handle("player_pos", function()
    local x, y = robox.player.get("x"), robox.player.get("y")
    return { x = x or 0, y = y or 0 }
end)
