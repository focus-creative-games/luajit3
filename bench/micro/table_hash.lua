-- Hash-part table insert/lookup
local N = tonumber(arg and arg[1]) or 4e5
local t0 = os.clock()
local t = {}
for i = 1, N do
  t["k" .. i] = i
end
local s = 0
for i = 1, N do
  s = s + t["k" .. i]
end
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(s), dt))
