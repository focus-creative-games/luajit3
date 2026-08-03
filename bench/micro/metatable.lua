-- Metamethod __index / __newindex / __add
local N = tonumber(arg and arg[1]) or 1e6
local store = {}
local mt = {
  __index = function(_, k)
    return store[k] or 0
  end,
  __newindex = function(_, k, v)
    store[k] = v
  end,
  __add = function(a, b)
    return (a.n or 0) + (b.n or 0)
  end,
}
local o = setmetatable({ n = 1 }, mt)
local t0 = os.clock()
local s = 0
for i = 1, N do
  o[i] = i
  s = s + o[i]
  s = s + (setmetatable({ n = i }, mt) + o)
end
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(s), dt))
