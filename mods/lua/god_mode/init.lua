-- mods/lua/god_mode/init.lua -- a folder mod: several files, hot-reloaded
-- together, with its own config.cfg beside them.
--
-- Anything in this directory that ends in .lua is watched, so saving
-- tuning.lua reloads the mod exactly the same way saving this file does.

local tuning = require("god_mode.tuning")

local cfg = robox.config.load()
if cfg.enabled == nil then cfg.enabled = false end

local last_health

robox.on("key", function(key, down)
    if key ~= tuning.toggle_key or not down then return end
    cfg.enabled = not cfg.enabled
    robox.config.save(cfg)
    robox.notify("god mode " .. (cfg.enabled and "ON" or "off"))
end)

robox.on("frame", function()
    if not cfg.enabled then return end
    if not robox.player.addr() then return end

    -- Pinned every frame rather than set once: the game writes this field
    -- itself, so a single write would last exactly until the next hit.
    local h = robox.player.get("health")
    if h and h < tuning.health then
        robox.player.set("health", tuning.health)
        if last_health and h < last_health then
            robox.log(string.format("absorbed a hit (%d -> %d)", h, tuning.health))
        end
    end
    last_health = tuning.health

    -- Invulnerability frames, the same field the settings menu shows.
    robox.player.set("iframes", tuning.iframes)
end)

robox.on("draw", function()
    if not cfg.enabled then return end
    robox.draw.text(robox.draw.W - 150, robox.draw.H - 40, "GOD MODE",
                    1, 0.85, 0.3, 0.9, 0.6)
end)

robox.on("start", function()
    robox.log(tuning.toggle_key .. " toggles god mode; currently " ..
              (cfg.enabled and "on" or "off"))
end)
