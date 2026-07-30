-- mods/lua/level_loader.lua -- warp to any level.
--
-- Hold BOTH mouse buttons for 5 seconds to open it. One page per world, click
-- a level to load it, right-click to close.
--
-- The chord is deliberately awkward. This loads any of the game's 54 levels
-- instantly, which is wonderful for poking at the game and ruinous if you
-- trigger it by accident mid-playthrough, so it wants a gesture nothing else
-- uses and nobody performs by mistake.
--
-- Laid out, coloured and animated after the port's own settings menu
-- (sdk/robox_menu.c): a deep scrim rather than a panel, muted greys, a tab
-- strip whose underline SLIDES between tabs instead of jumping, and a hairline
-- frame on the highlighted entry with no fill and no accent colour -- which is
-- how the GAME marks "play" on its main menu.
--
-- KNOWN LIMIT: this does not work from the main menu. The menu is not a level,
-- it is a different state machine, and changing the level underneath it leaves
-- it running the menu over the new data. Start a level first.

local HOLD_TIME = 5.0

local COL_ON    = { 1.00, 1.00, 1.00 }
local COL_OFF   = { 0.62, 0.65, 0.68 }
local COL_FAINT = { 0.42, 0.45, 0.48 }
local COL_AMBER = { 1.00, 0.62, 0.18 }

local MARGIN    = 78
local RIGHT     = 1280 - 44
local TAB_Y     = 200
local GRID_TOP  = 286
local GRID_COLS = 5
local CELL_W    = (RIGHT - MARGIN - 4 * 26) / GRID_COLS
local CELL_H    = 44
local ROW_PITCH = 74

local open, hold, armed = false, 0, true
local anim  = 0               -- 0..1, eases the whole panel in and out
local page, page_prev, page_dir, page_anim = 1, 1, 1, 1
local levels, worlds = nil, nil
local hover, hover_tab = nil, nil
local click_was_down = false
local flash, flash_name = 0, nil

-- You are necessarily still holding both buttons at the instant this opens --
-- that is what opened it. Without a latch the right-button test closed the
-- menu on the very next frame ("appears then disappears"), and releasing left
-- would have loaded whatever the cursor happened to be sitting on.
local wait_release = false

-- The game's own Wii pointer, with its pressed state. cr_ = cursor;
-- "pulsado" is Spanish for pressed.
local cur_normal, cur_pressed

local function smoothstep(t) return t * t * (3 - 2 * t) end

-- Group levels by world once. Names are the engine's own and always start
-- with the two-digit world, so this needs no table.
local function ensure()
    if worlds then return worlds end
    levels = robox.level.list()
    worlds = {}
    for _, lv in ipairs(levels) do
        local w = tonumber(lv.name:sub(1, 2)) or 0
        worlds[w] = worlds[w] or { n = w, items = {} }
        local t = worlds[w].items
        t[#t + 1] = lv
    end
    -- compact to a dense array, and place each world's cells
    local out = {}
    for w = 1, 9 do
        if worlds[w] then
            out[#out + 1] = worlds[w]
            for i, lv in ipairs(worlds[w].items) do
                local c, r = (i - 1) % GRID_COLS, math.floor((i - 1) / GRID_COLS)
                lv.x = MARGIN + c * (CELL_W + 26)
                lv.y = GRID_TOP + r * ROW_PITCH
            end
        end
    end
    worlds = out
    return worlds
end

local function tab_label(i) return ("world %d"):format(ensure()[i].n) end

-- Tab strip geometry, needed by both the hit test and the draw.
local function tab_rects()
    local xs, ws, x = {}, {}, MARGIN
    for i = 1, #ensure() do
        ws[i] = robox.draw.text_width(tab_label(i), 0.66, 1.5)
        xs[i] = x
        x = x + ws[i] + 34
    end
    return xs, ws
end

local function cursor(mx, my, pressed)
    local img = (pressed and cur_pressed) or cur_normal
    if not img then return end
    local S = 46
    robox.draw.image(img, mx - S / 2, my - S / 2, S, S, 1, 1, 1, 1)
end

local function set_page(p)
    if p == page or p < 1 or p > #ensure() then return end
    page_prev, page_dir, page_anim = page, (p > page) and 1 or -1, 0
    page = p
end

robox.on("frame", function(dt)
    local mx, my, left, right = robox.input.mouse()

    if flash > 0 then flash = flash - dt end
    if page_anim < 1 then page_anim = math.min(1, page_anim + dt * 6) end

    if not open then
        if anim > 0 then anim = math.max(0, anim - dt * 10) end
        if left and right and armed then
            hold = hold + dt
            if hold >= HOLD_TIME then
                open, hold, armed = true, 0, false
                wait_release, click_was_down = true, false
                anim, page_anim = 0, 1
                robox.input.block(true)
                robox.log("level loader open")
            end
        else
            hold = 0
            if not left and not right then armed = true end
        end
        return
    end

    if anim < 1 then anim = math.min(1, anim + dt * 7) end

    -- Nothing counts until the chord that opened this has been let go of.
    if wait_release then
        if not left and not right then wait_release = false end
        click_was_down = false
        return
    end

    -- Hover: tabs first, then the current page's cells.
    hover, hover_tab = nil, nil
    local xs, ws = tab_rects()
    for i = 1, #ensure() do
        if mx >= xs[i] - 8 and mx < xs[i] + ws[i] + 8
           and my >= TAB_Y - 6 and my < TAB_Y + 30 then
            hover_tab = i
        end
    end
    if not hover_tab then
        for _, lv in ipairs(ensure()[page].items) do
            if mx >= lv.x - 6 and mx < lv.x + CELL_W + 2
               and my >= lv.y - 4 and my < lv.y + CELL_H then
                hover = lv
            end
        end
    end

    if right then
        open = false
        robox.input.block(false)
        robox.log("level loader closed")
    elseif click_was_down and not left then
        if hover_tab then
            set_page(hover_tab)
        elseif hover then
            robox.log(("loading %s (id %d)"):format(hover.name, hover.id))
            flash, flash_name = 1.2, hover.name
            open = false
            robox.input.block(false)
            robox.level.load(hover.id)
        end
    end
    click_was_down = left
end)

robox.on("draw", function()
    -- A number in the corner while the chord is held. Nothing follows the
    -- cursor and nothing covers the game.
    if not open and anim <= 0 then
        if hold > 0 then
            local k = hold / HOLD_TIME
            robox.draw.text(24, 692, ("%.0f%%"):format(k * 100),
                            0.6 + 0.4 * k, 1, 0.6 + 0.4 * k, 0.95, 0.6)
        end
        if flash > 0 and flash_name then
            robox.draw.text(24, 692, "loading " .. flash_name,
                            1, 1, 1, math.min(1, flash), 0.55)
        end
        return
    end

    local a   = anim
    local top = (1 - a) * 18          -- the panel drifts up into place
    local mx, my, left = robox.input.mouse()

    robox.draw.rect(0, 0, 1280, 720, 0, 0, 0, 0.90 * a)

    robox.draw.text(MARGIN, 120 + top, "level loader",
                    COL_ON[1], COL_ON[2], COL_ON[3], a, 1.25, 5)
    robox.draw.rect(MARGIN, 162 + top, RIGHT - MARGIN, 1,
                    COL_ON[1], COL_ON[2], COL_ON[3], 0.28 * a)

    -- Tab strip. The underline slides between tabs rather than jumping, which
    -- is most of what makes the change read as one motion.
    local ts = smoothstep(page_anim)
    local xs, ws = tab_rects()
    for i = 1, #ensure() do
        local lit = (i == page) and ts or ((i == page_prev) and 1 - ts or 0)
        if hover_tab == i then lit = math.max(lit, 0.75) end
        local c = 0.42 + 0.58 * lit
        robox.draw.text(xs[i], TAB_Y + top, tab_label(i),
                        c, c + 0.03, c + 0.06, a, 0.66, 1.5)
    end
    do
        local ul = TAB_Y + 33 * 0.66 + 5
        local x0 = xs[page_prev] + (xs[page] - xs[page_prev]) * ts
        local w0 = ws[page_prev] + (ws[page] - ws[page_prev]) * ts
        robox.draw.rect(x0, ul + top, w0, 2,
                        COL_ON[1], COL_ON[2], COL_ON[3], 0.9 * a)
    end

    -- Cells slide in from the side the page change came from, and fade as
    -- they arrive.
    local slide = (1 - ts) * 46 * page_dir
    local ca    = a * (0.25 + 0.75 * ts)
    local current = robox.level.id()

    for _, lv in ipairs(ensure()[page].items) do
        local x = lv.x + slide
        local is_hover = (hover == lv)
        local is_now   = (lv.id == current)

        if is_hover then
            robox.draw.outline(x - 8, lv.y - 5, CELL_W + 10, CELL_H,
                               1.0, COL_ON[1], COL_ON[2], COL_ON[3], 0.85 * ca)
        end

        local c = is_hover and COL_ON or (is_now and COL_AMBER or COL_OFF)
        local short, tag = lv.name:match("^([^_]+)_?(.*)$")
        robox.draw.text(x, lv.y, short, c[1], c[2], c[3], ca, 0.80, 1.5)
        if tag and tag ~= "" then
            -- The suffix says what the level actually is (jefe = boss), so it
            -- is kept, just quieter.
            robox.draw.text(x, lv.y + 26, tag,
                            COL_FAINT[1], COL_FAINT[2], COL_FAINT[3],
                            ca, 0.42, 1.0)
        end
    end

    robox.draw.rect(MARGIN, 666 + top, RIGHT - MARGIN, 1,
                    COL_ON[1], COL_ON[2], COL_ON[3], 0.18 * a)
    robox.draw.text(MARGIN, 690 + top,
                    "click load     tabs switch world     right-click close",
                    COL_FAINT[1], COL_FAINT[2], COL_FAINT[3], a, 0.50, 1.5)
    if hover then
        local s = ("%s   id %d"):format(hover.name, hover.id)
        local w = robox.draw.text_width(s, 0.50, 1.5)
        robox.draw.text(RIGHT - w, 690 + top, s,
                        COL_ON[1], COL_ON[2], COL_ON[3], a, 0.50, 1.5)
    end

    -- The game's own pointer: the port hides the system cursor.
    cursor(mx, my, left)
end)

robox.on("unload", function()
    if open then robox.input.block(false) end
end)

robox.on("start", function()
    cur_normal  = robox.draw.load("media/gui/botones/cr_normal.tpl")
    cur_pressed = robox.draw.load("media/gui/botones/cr_pulsado.tpl")
    robox.log(("hold both mouse buttons for %.0fs to open the level loader "
               .. "(cursor %s)"):format(HOLD_TIME,
               cur_normal and "loaded" or "MISSING"))
end)
