-- make test-love: LÖVE 11.5 on PeOS, the parts Balatro uses, each checked
-- (docs/game-ports-plan.md, Balatro phase 3). The lines go to stderr (the
-- kernel's log); "lovecheck: PASS" only when every check passed, else
-- "lovecheck: FAIL <what>". Then the window draws for a few seconds (the
-- frames a second are logged) and the game quits.

io.stderr:setvbuf("no")
local function say(s) io.stderr:write("lovecheck: " .. s .. "\n") end
local failed = nil
local function check(what, ok, detail)
    if ok then
        say("ok " .. what .. (detail and (" (" .. detail .. ")") or ""))
    else
        say("bad " .. what .. (detail and (" (" .. detail .. ")") or ""))
        failed = failed or what
    end
end

-- A shader as Balatro writes them: extern values, the effect function.
local SHADER = [[
extern number shade;
extern vec2 size;
vec4 effect(vec4 colour, Image tex, vec2 tc, vec2 sc) {
    vec4 t = Texel(tex, tc);
    return vec4(t.r * shade, sc.x / size.x, t.b, 1.0) * colour;
}
]]

local THREAD = [[
local inbox, outbox = ...
local sum = 0
while true do
    local v = inbox:demand()
    if v == "end" then break end
    sum = sum + v
end
outbox:push(sum)
]]

local font, canvas, shader, sound, stream
local frames, started, report_at, quit_at = 0, 0, 0, 0
local spin = 0

function love.load()
    say("LÖVE " .. table.concat({ love.getVersion() }, ".", 1, 3) .. " on " .. love.system.getOS() ..
        ", " .. love.system.getProcessorCount() .. " CPUs")
    local name, version, vendor, device = love.graphics.getRendererInfo()
    say("renderer " .. tostring(name) .. " " .. tostring(version) .. " / " .. tostring(vendor) .. " / " .. tostring(device))
    check("JIT on", jit and jit.status() == true, jit and jit.version)

    -- graphics: a canvas drawn through a shader, read back
    local img = love.image.newImageData(4, 4)
    img:mapPixel(function() return 1, 0, 0.5, 1 end)
    local tex = love.graphics.newImage(img)
    canvas = love.graphics.newCanvas(64, 64)
    local ok, err = pcall(function() shader = love.graphics.newShader(SHADER) end)
    check("shader compiled", ok, not ok and tostring(err) or nil)
    if ok then
        shader:send("shade", 0.5)
        shader:send("size", { 64, 64 })
        love.graphics.setCanvas(canvas)
        love.graphics.clear(0, 0, 0, 1)
        love.graphics.setShader(shader)
        love.graphics.draw(tex, 0, 0, 0, 16, 16)
        love.graphics.setShader()
        love.graphics.setCanvas()
        local back = canvas:newImageData()
        local r, g, b = back:getPixel(48, 10)
        check("shader drew", math.abs(r - 0.5) < 0.02 and math.abs(g - 48.5 / 64) < 0.02 and math.abs(b - 0.5) < 0.02,
            string.format("%.3f %.3f %.3f", r, g, b))
    end

    -- fonts: the default one, and text measured
    font = love.graphics.newFont(20)
    check("font", font:getWidth("Balatro") > 30, "width " .. font:getWidth("Balatro"))

    -- threads and channels
    local inbox, outbox = love.thread.newChannel(), love.thread.newChannel()
    local th = love.thread.newThread(THREAD)
    th:start(inbox, outbox)
    for i = 1, 100 do inbox:push(i) end
    inbox:push("end")
    local sum = outbox:demand(10)
    th:wait()
    check("thread", sum == 5050 and not th:getError(), tostring(sum or th:getError()))

    -- data: compressed and back, as Balatro's saves
    local text = string.rep("PeOS plays Balatro. ", 200)
    local packed = love.data.compress("string", "deflate", text, 9)
    local back = love.data.decompress("string", "deflate", packed)
    check("deflate", back == text and #packed < #text / 10, #text .. " -> " .. #packed)
    check("md5", love.data.encode("string", "hex", love.data.hash("md5", "abc")) == "900150983cd24fb0d6963f7d28e17f72")

    -- the save directory: written, listed, read back
    love.filesystem.setIdentity("lovecheck")
    local saved = love.filesystem.write("save.jkr", packed)
    local readback = saved and love.filesystem.read("save.jkr")
    check("save", saved and readback == packed, love.filesystem.getSaveDirectory())

    -- sound: a tone made here, and the Ogg Vorbis in the game (mkogg's)
    local rate = 44100
    local sd = love.sound.newSoundData(math.floor(rate / 4), rate, 16, 1)
    for i = 0, sd:getSampleCount() - 1 do sd:setSample(i, 0.3 * math.sin(2 * math.pi * 660 * i / rate)) end
    sound = love.audio.newSource(sd)
    sound:play()
    check("sound plays", sound:isPlaying())
    local okd, dec = pcall(love.sound.newDecoder, "tone.ogg")
    check("ogg decoder", okd and dec:getSampleRate() == 44100 and dec:getChannelCount() == 2 and
        math.abs(dec:getDuration() - 1) < 0.05, okd and string.format("%d Hz, %d ch, %.2f s", dec:getSampleRate(),
        dec:getChannelCount(), dec:getDuration()) or tostring(dec))
    if okd then
        stream = love.audio.newSource("tone.ogg", "stream")
        stream:setVolume(0.5)
        stream:play()
        check("ogg stream plays", stream:isPlaying())
    end

    say(failed and ("FAIL " .. failed) or "PASS")
    started = love.timer.getTime()
    report_at = started + 4
    quit_at = started + 6
end

function love.update(dt)
    spin = spin + dt
    local now = love.timer.getTime()
    if report_at > 0 and now >= report_at then
        say(string.format("%d frames a second (%d in %.1f s), window %dx%d", math.floor(frames / (now - started)),
            frames, now - started, love.graphics.getWidth(), love.graphics.getHeight()))
        report_at = 0
    end
    if now >= quit_at then love.event.quit() end
end

function love.draw()
    frames = frames + 1
    local w, h = love.graphics.getDimensions()
    love.graphics.clear(0.10, 0.12, 0.20)
    love.graphics.setColor(1, 1, 1)
    love.graphics.draw(canvas, w / 2 - 64, h / 2 - 64, 0, 2, 2)
    love.graphics.push()
    love.graphics.translate(w / 2, h / 2)
    love.graphics.rotate(spin)
    love.graphics.setColor(0.35, 0.8, 1.0)
    love.graphics.rectangle("line", -90, -90, 180, 180, 12)
    love.graphics.pop()
    love.graphics.setFont(font)
    love.graphics.setColor(1, 1, 1)
    love.graphics.print("LÖVE on PeOS", 20, 20)
end

function love.keypressed(key, scancode)
    say("key " .. key .. " (" .. scancode .. ")")
end

function love.mousepressed(x, y, button)
    say(string.format("click %d at %d,%d", button, x, y))
end

function love.quit()
    say("quit")
end
