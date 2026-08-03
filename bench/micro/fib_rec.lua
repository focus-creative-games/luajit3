-- Naive recursive Fibonacci (call + stack pressure)
local N = tonumber(arg and arg[1]) or 35
local function fib(n)
  if n < 2 then
    return n
  end
  return fib(n - 1) + fib(n - 2)
end
local t0 = os.clock()
local v = fib(N)
local dt = os.clock() - t0
print(string.format("ok %s time=%.6f", tostring(v), dt))
