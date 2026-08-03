-- Dense array write/read
local N = tonumber(arg and arg[1]) or 2e6
local t0 = os.clock()
local t = {}
for i = 1, N do
  t[i] = i * 2
end
local s = 0
for i = 1, N do
  s = s + t[i]
end
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(s), dt))
