-- String concat + length + sub
local N = tonumber(arg and arg[1]) or 5e5
local t0 = os.clock()
local s = ""
for i = 1, N do
  s = s .. "x"
  if #s > 64 then
    s = string.sub(s, -32)
  end
end
local dt = os.clock() - t0
print(string.format("ok %d %d time=%.6f", #s, string.byte(s, 1), dt))
