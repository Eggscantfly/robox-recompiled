-- mods/lua/guest/hello.lua -- runs inside the GAME's Lua, not the port's.
--
-- This file is Lua 5.1, in the VM that Robox itself ships. It is appended to
-- script/game.lua on the way to the engine, so it runs during the game's own
-- boot with the engine's bindings already in scope:
--
--     loadMap, loadMap2, newFpg, unloadFpg, unloadTiles,
--     loadTileset, cargaAnimacion, renderQuad, getText, nameForDebug
--
-- and the globals game.lua set up (textureDir, textureList, loaded_fpg,
-- IMAGE_EXT, BARRA_DIRECTORIOS, and the translated strings from
-- script/lang/lang_*.lua).
--
-- It is 5.1: no goto, no integer division, no <close>. string, table, math,
-- io, os, debug and coroutine are all there.
--
-- NOTE the difference from the mods one directory up. Those run on the host,
-- every frame, with guest memory and function hooks. This runs ONCE, at boot,
-- inside the game -- there is no frame hook here, because the engine never
-- calls back into Lua after startup. What it is good for is the engine's own
-- asset machinery, and asking the host things.

robox.log("hello from the game's own Lua " .. _VERSION)

-- What the engine set up before we got here.
if textureDir then
    robox.log("current tileset dir: " .. tostring(textureDir))
end
if loaded_fpg then
    local n = 0
    for _ in pairs(loaded_fpg) do n = n + 1 end
    robox.log("boneset FPGs already loaded: " .. n)
end

-- Calling the host. robox.host(name, ...) invokes a handler a host-side mod
-- registered with robox.game.handle(name, fn) and returns what it returned.
-- mods/lua/hello.lua registers "player_pos".
--
-- It works by dofile()ing a path that is not a file: the port answers the read
-- with a generated chunk. Cheap enough to do at boot, far too slow for a loop.
local pos = robox.host("player_pos")
if type(pos) == "table" then
    robox.log(string.format("host says the player is at %.1f, %.1f",
                            pos.x or 0, pos.y or 0))
else
    robox.log("no player yet -- nothing has loaded a level at boot time")
end

-- Wrapping an engine binding. loadBoneset is defined in script/anim.lua,
-- which game.lua has already run by the time this file is appended, so it is
-- there to be replaced. Every animation the game loads from here on says so.
if type(loadBoneset) == "function" then
    local original = loadBoneset
    loadBoneset = function(boneset)
        robox.log("loading boneset " .. tostring(boneset))
        return original(boneset)
    end
end
