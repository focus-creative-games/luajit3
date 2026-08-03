-- Shared: print "ok <fields...> time=<seconds>"
-- Integer arithmetic tight loop
local N = tonumber(arg and arg[1]) or 5e7
local t0 = os.clock()
local s = 0
for i = 1, N do
  s = s + i
  s = s ~ (i << 1)
  s = s + (i * 3)
end
local dt = os.clock() - t0
assert(type(s) == "number")
print(string.format("ok %s time=%.6f", tostring(s), dt))
