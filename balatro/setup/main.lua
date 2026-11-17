-- balatro.app's setup (docs/game-ports-plan.md, Balatro phase 5, question
-- 3): the game is the user's own. This screen looks for it on the USB
-- sticks (/media1 ... /media4) and in /home: the
-- Balatro.love of the Mac's Steam copy, or the Balatro.exe of Windows'
-- (LÖVE with the game's zip at its end: only the zip is copied). The copy
-- goes to Balatro's own save directory, /starlight/config/love/Balatro
-- (its saves go there too); then LÖVE restarts and app.ini's "--first"
-- finds it.

local DEST_DIR = "/starlight/config/love/Balatro"
local DEST = DEST_DIR .. "/Balatro.love"
local PLACES = { "/media1", "/media2", "/media3", "/media4", "/home" }
local NAMES = { "Balatro.love", "Balatro.exe", "Balatro/Balatro.love", "Balatro/Balatro.exe",
    "Balatro.app/Contents/Resources/Balatro.love" }
local CHUNK = 1024 * 1024

local text = {}
local state = "looking"   -- looking, copying, done, failed
local detail = ""
local copy = nil          -- { src, out, left, total }
local next_look = 0
local font_big, font

local function say(s) io.stderr:write("balatro-setup: " .. s .. "\n") end

-- The bundle's sentences (resources/<language>/String.lang, key=text),
-- beside this setup folder in the bundle.
local function load_text()
    local lang = os.getenv("PEOS_LANG") or "en"
    local bundle = love.filesystem.getSource():gsub("/[^/]*$", "")
    for _, l in ipairs({ lang, "en" }) do
        local f = io.open(bundle .. "/resources/" .. l .. "/String.lang", "r")
        if f then
            for line in f:lines() do
                local k, v = line:match("^([%w%.%_]+)=(.*)$")
                if k and not text[k] then text[k] = v end
            end
            f:close()
        end
    end
end

local function t(key) return text[key] or key end

local function u32(s, at)   -- little-endian, at is 1-based
    local a, b, c, d = s:byte(at, at + 3)
    return a + b * 256 + c * 65536 + d * 16777216
end

-- Where the game's zip starts: 0 for a .love, after LÖVE's code in an .exe
-- (from the zip's end record: where it is, minus the directory's size and
-- its offset in the zip).
local function zip_start(f)
    local size = f:seek("end")
    local tail_len = math.min(size, 65536 + 22)
    f:seek("set", size - tail_len)
    local tail = f:read(tail_len)
    if not tail then return nil end
    for i = #tail - 21, 1, -1 do
        if tail:byte(i) == 0x50 and tail:sub(i, i + 3) == "PK\5\6" then
            local eocd = size - tail_len + (i - 1)
            local start = eocd - u32(tail, i + 12) - u32(tail, i + 16)
            if start < 0 then return nil end
            f:seek("set", start)
            if f:read(4) ~= "PK\3\4" then return nil end
            return start, size
        end
    end
    return nil
end

local function look()
    for _, place in ipairs(PLACES) do
        for _, name in ipairs(NAMES) do
            local path = place .. "/" .. name
            local f = io.open(path, "rb")
            if f then
                local start, size = zip_start(f)
                if start then
                    f:seek("set", start)
                    local out = io.open(DEST .. ".part", "wb")
                    if not out then
                        f:close()
                        state, detail = "failed", t("setup.cannot_write") .. " " .. DEST_DIR
                        return
                    end
                    say("found " .. path .. " (the zip: " .. (size - start) .. " bytes from " .. start .. ")")
                    copy = { src = f, out = out, left = size - start, total = size - start, path = path }
                    state, detail = "copying", path
                    return
                end
                say(path .. ": not a LÖVE game (no zip in it)")
                f:close()
            end
        end
    end
end

function love.load()
    load_text()
    love.window.setTitle("Balatro")
    font_big = love.graphics.newFont(28)
    font = love.graphics.newFont(16)
    -- Balatro's save directory, made by LÖVE (it makes the folders on the
    -- first write), where the copy goes
    love.filesystem.setIdentity("Balatro")
    if not love.filesystem.getInfo("peos-setup.txt") then
        love.filesystem.write("peos-setup.txt", "Balatro.love here is the user's copy (balatro.app's setup)\n")
    end
    say("looking for Balatro.love or Balatro.exe")
end

function love.update()
    if state == "looking" then
        local now = love.timer.getTime()
        if now >= next_look then
            next_look = now + 2   -- a stick put in meanwhile is mounted by PeOS on /mediaN
            look()
        else
            love.timer.sleep(0.05)
        end
    elseif state == "copying" then
        local n = math.min(CHUNK, copy.left)
        local data = copy.src:read(n)
        if not data or #data ~= n or not copy.out:write(data) then
            copy.src:close()
            copy.out:close()
            os.remove(DEST .. ".part")
            state, detail = "failed", t("setup.copy_failed") .. " " .. copy.path
            say("the copy failed")
            return
        end
        copy.left = copy.left - n
        if copy.left == 0 then
            copy.src:close()
            copy.out:close()
            os.remove(DEST)
            local ok = os.rename(DEST .. ".part", DEST)
            if ok then
                state = "done"
                say("copied to " .. DEST .. ": restarting")
                love.event.quit("restart")
            else
                state, detail = "failed", t("setup.cannot_write") .. " " .. DEST
            end
        end
    else
        love.timer.sleep(0.1)
    end
end

function love.draw()
    local w, h = love.graphics.getDimensions()
    love.graphics.clear(0.12, 0.14, 0.22)
    love.graphics.setColor(1, 1, 1)
    love.graphics.setFont(font_big)
    love.graphics.printf("Balatro", 0, h * 0.22, w, "center")
    love.graphics.setFont(font)
    local body
    if state == "looking" then
        body = t("setup.looking")
    elseif state == "copying" then
        body = t("setup.copying") .. "\n" .. detail
        local done = 1 - copy.left / copy.total
        local bw = math.min(420, w - 80)
        love.graphics.setColor(0.25, 0.28, 0.40)
        love.graphics.rectangle("fill", (w - bw) / 2, h * 0.62, bw, 14, 7)
        love.graphics.setColor(0.35, 0.80, 1.0)
        love.graphics.rectangle("fill", (w - bw) / 2, h * 0.62, bw * done, 14, 7)
        love.graphics.setColor(1, 1, 1)
    elseif state == "done" then
        body = t("setup.done")
    else
        body = detail
    end
    love.graphics.printf(body, 40, h * 0.36, w - 80, "center")
end

function love.keypressed(key)
    if key == "escape" then love.event.quit() end
end
