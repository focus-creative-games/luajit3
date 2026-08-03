-- Shootout-style binary trees (alloc + GC pressure)
local function BottomUpTree(depth)
  if depth > 0 then
    depth = depth - 1
    local left = BottomUpTree(depth)
    local right = BottomUpTree(depth)
    return { left, right }
  else
    return { false, false }
  end
end

local function ItemCheck(tree)
  if tree[1] then
    return 1 + ItemCheck(tree[1]) + ItemCheck(tree[2])
  else
    return 1
  end
end

local N = tonumber(arg and arg[1]) or 15
local minDepth = 4
local maxDepth = math.max(minDepth + 2, N)
local stretchDepth = maxDepth + 1

local t0 = os.clock()
local check = ItemCheck(BottomUpTree(stretchDepth))
local longLivedTree = BottomUpTree(maxDepth)

for depth = minDepth, maxDepth, 2 do
  local iterations = 2 ^ (maxDepth - depth + minDepth)
  check = 0
  for _ = 1, iterations do
    check = check + ItemCheck(BottomUpTree(depth))
  end
end

check = ItemCheck(longLivedTree)
local dt = os.clock() - t0
print(string.format("ok %d %d time=%.6f", check, maxDepth, dt))
