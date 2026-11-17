-- LuaJIT on PeOS (Balatro, phase 2): the interpreter and the JIT give the
-- same results, the JIT is much faster (it compiles traces: code written
-- at run time, pages RW then RX), errors unwind, coroutines and FFI work.
local ok_all = true
local function check(ok, what)
  print(("luajit: %s %s"):format(ok and "ok  " or "FAIL", what))
  if not ok then ok_all = false end
end

print("luajit: " .. jit.version .. " on " .. jit.os .. "/" .. jit.arch)

local function work(n)          -- floating point and integers, a hot loop
  local s, x = 0, 0.5
  for i = 1, n do
    x = x * 1.0000001 + (i % 7) * 0.25
    s = s + math.floor(x) % 13
  end
  return s
end
local N = 20000000

jit.off(work, true)
local t0 = os.clock()
local slow = work(N)
local t_interp = os.clock() - t0

jit.on(work, true)
jit.flush()
t0 = os.clock()
local fast = work(N)
local t_jit = os.clock() - t0

print(("luajit: interpreter %.3f s, JIT %.3f s, %.1fx"):format(t_interp, t_jit, t_interp / math.max(t_jit, 1e-6)))
check(slow == fast, "the same result with and without the JIT (" .. tostring(fast) .. ")")
check(jit.status(), "the JIT is on")
check(t_jit * 3 < t_interp, "the JIT is at least 3 times faster")

-- errors through pcall, and through C (string.gsub calling back)
local ok, err = pcall(function() error("boom") end)
check(not ok and tostring(err):find("boom"), "error and pcall")
ok = pcall(string.gsub, "abc", "%w", function() error("inside") end)
check(not ok, "an error unwinding through C")

-- coroutines
local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1); return b * 2 end)
check(co(1) == 2 and co(10) == 20, "coroutines")

-- FFI: types, arrays, pointers (what LÖVE's own FFI code uses; ffi.C's
-- lookup by name needs dlopen, and PeOS programs are static: refused)
local ffi = require("ffi")
ffi.cdef("typedef struct { float x, y; uint8_t r, g, b, a; } vertex_t;")
local v = ffi.new("vertex_t[4]")
v[3].x, v[3].a = 1.5, 255
local p = ffi.cast("vertex_t *", v)
check(p[3].x == 1.5 and p[3].a == 255 and ffi.sizeof("vertex_t") == 12, "FFI structs and pointers")
check(not pcall(function() return ffi.C.abs end), "ffi.C refused (no symbols to look up)")
local arr = ffi.new("int[16]")
for i = 0, 15 do arr[i] = i * i end
check(arr[15] == 225, "FFI arrays")

-- a big table, the GC
local t = {}
for i = 1, 2000000 do t[i] = {i} end
t = nil
collectgarbage()
check(collectgarbage("count") < 64 * 1024, "the GC gives memory back (" .. math.floor(collectgarbage("count")) .. " KB)")

print("luajit: " .. (ok_all and "PASS" or "FAIL"))
