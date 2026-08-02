_U=true
_soft=true
_port=true
_nomsg=true
debug=nil
math.randomseed(0)
print("1 path", package.path)
local initclock = os.clock()
local lastclock = initclock
local walltime = os.time()
print("2 clocks ok", initclock, walltime)
assert(os.setlocale"C")
print("3 locale ok")
local T,print,format,write,assert,type,unpack,floor =
      T,print,string.format,io.write,assert,type,table.unpack,math.floor
print("4 aliases", format, write, unpack, floor)
local function F (m)
  local function round (m)
    m = m + 0.04999
    return format("%.1f", m)
  end
  if m < 1000 then return m
  else
    m = m / 1000
    if m < 1000 then return round(m).."K"
    else
      return round(m/1000).."M"
    end
  end
end
print("5 F", F(10), F(1500))
