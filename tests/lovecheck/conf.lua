function love.conf(t)
    t.identity = "lovecheck"
    t.window.title = "LÖVE check"
    t.window.width = 640
    t.window.height = 480
    t.window.resizable = true
    t.window.vsync = 1
    t.modules.physics = false
    t.modules.video = false
end
