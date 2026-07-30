-- mods/lua/god_mode/tuning.lua -- pulled in with require("god_mode.tuning").
--
-- package.path already covers mods/lua/, so a folder mod's own modules are
-- reachable as <folder>.<file> with no setup. Saving this file reloads the
-- whole mod, since every .lua in a mod folder is watched.

return {
    toggle_key = "F10",
    health     = 3,      -- what health is pinned to
    iframes    = 60.0,   -- invulnerability timer, in the game's own units
}
