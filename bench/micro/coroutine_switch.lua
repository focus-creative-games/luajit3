-- Coroutine create/resume churn
local N = tonumber(arg and arg[1]) or 3e5
local t0 = os.clock()
local s = 0
for i = 1, N do
  local co = coroutine.create(function(x)
    coroutine.yield(x + 1)
    return x + 2
  end)
  local ok, v = coroutine.resume(co, i)
  s = s + (ok and v or 0)
  ok, v = coroutine.resume(co)
  s = s + (ok and v or 0)
end
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(s), dt))
