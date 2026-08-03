-- Lua→Lua function calls
local N = tonumber(arg and arg[1]) or 2e7
local function add(a, b)
  return a + b
end
local t0 = os.clock()
local s = 0
for i = 1, N do
  s = add(s, i)
end
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(s), dt))
