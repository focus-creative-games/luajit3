-- string.find / string.gsub pattern work
local N = tonumber(arg and arg[1]) or 1e5
local src = string.rep("abc123XYZ", 20)
local t0 = os.clock()
local hits = 0
for i = 1, N do
  local a, b = string.find(src, "%d+")
  hits = hits + (a or 0) + (b or 0)
  local r, n = string.gsub(src, "%a+", "Z")
  hits = hits + n + #r
end
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(hits), dt))
