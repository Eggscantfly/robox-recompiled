-- mods/lua/hud.lua -- a live readout of the player and the level. F9 toggles.
--
-- Shows the four things most mods are built out of: draw, player, level and
-- input, plus a setting that survives a reload by going through robox.config.

local cfg = robox.config.load()
if cfg.visible == nil then cfg.visible = true end

local W, H = robox.draw.W, robox.draw.H

-- Rolling frame time, so the number does not flicker faster than it can be read.
local ft, ft_n, ft_shown = 0, 0, 0

robox.on("key", function(key, down)
    if key == "F9" and down then
        cfg.visible = not cfg.visible
        robox.config.save(cfg)
        robox.notify("hud " .. (cfg.visible and "on" or "off"))
    end
end)

robox.on("frame", function(dt)
    ft, ft_n = ft + dt, ft_n + 1
    if ft >= 0.25 then
        ft_shown = ft / ft_n
        ft, ft_n = 0, 0
    end
end)

local function row(y, label, value)
    robox.draw.text(36, y, label, 0.65, 0.72, 0.80, 1, 0.5)
    robox.draw.text(170, y, value, 1, 1, 1, 1, 0.5)
end

robox.on("draw", function()
    if not cfg.visible then return end

    local p = robox.player.addr()
    local rows = 8

    robox.draw.rect(24, 60, 330, 24 * rows + 24, 0, 0, 0, 0.55)
    robox.draw.outline(24, 60, 330, 24 * rows + 24, 1.5, 0.4, 0.6, 0.9, 0.5)

    local y = 84
    row(y, "level", string.format("%s  (id %d)", robox.level.name(), robox.level.id()))
    y = y + 24
    row(y, "section", robox.level.is_robot() and "robot interior" or "outside")
    y = y + 24
    row(y, "frame time", string.format("%.2f ms  (%.0f fps)",
        ft_shown * 1000, ft_shown > 0 and 1 / ft_shown or 0))
    y = y + 24

    if not p then
        row(y, "player", "not in a level")
        return
    end

    row(y, "player", string.format("0x%08X", p))                       y = y + 24
    row(y, "position", string.format("%.1f, %.1f",
        robox.player.get("x"), robox.player.get("y")))                 y = y + 24
    row(y, "health", tostring(robox.player.get("health")))             y = y + 24
    row(y, "state", tostring(robox.player.get("state")))               y = y + 24

    -- Which buttons the game is actually seeing, injected presses included.
    local held = {}
    for name in pairs(robox.input.buttons()) do
        if robox.input.held(name) then held[#held + 1] = name end
    end
    table.sort(held)
    row(y, "input", #held > 0 and table.concat(held, " ") or "-")
end)

robox.on("start", function()
    robox.log("F9 toggles the HUD")
end)
