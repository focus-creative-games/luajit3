-- table.sort on mixed integers
local N = tonumber(arg and arg[1]) or 2e5
local t = {}
for i = 1, N do
  t[i] = (i * 2654435761) % 2147483647
end
local t0 = os.clock()
table.sort(t)
local dt = os.clock() - t0
print(string.format("ok %s %s time=%.6f", tostring(t[1]), tostring(t[N]), dt))
