_U=true
_soft=true
_port=true
_nomsg=true
debug=nil
math.randomseed(0)
print("path", package.path)
local initclock = os.clock()
local lastclock = initclock
local walltime = os.time()
print("clocks", initclock, lastclock, walltime)
print("setlocale", os.setlocale("C"))
local T,print,format,write,assert,type,unpack,floor =
      T,print,string.format,io.write,assert,type,table.unpack,math.floor
print("locals", type(format), type(write), type(unpack), type(floor))
