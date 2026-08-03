-- Closure create + call (upvalue capture)
local N = tonumber(arg and arg[1]) or 1e6
local t0 = os.clock()
local s = 0
for i = 1, N do
  local x = i
  local f = function()
    return x + 1
  end
  s = s + f()
end
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(s), dt))
