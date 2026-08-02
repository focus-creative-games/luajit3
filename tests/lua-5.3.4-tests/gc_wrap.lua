local real_assert = assert
local n = 0
function assert(v, m)
  n = n + 1
  if not v then
    local info = debug.getinfo(2, "Sl")
    print("ASSERT FAIL at", info.short_src, info.currentline, m or "")
    return real_assert(v, m)
  end
  return real_assert(v, m)
end
dofile("gc.lua")
