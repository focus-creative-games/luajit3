-- Floating-point arithmetic loop
local N = tonumber(arg and arg[1]) or 3e7
local t0 = os.clock()
local x = 1.0
for i = 1, N do
  x = x * 1.0000001 + 0.5
  x = x / 1.00000005
end
local dt = os.clock() - t0
print(string.format("ok %.10g time=%.6f", x, dt))
