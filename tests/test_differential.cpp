#include "luajit3/lua.h"

#include <iostream>
#include <string>
#include <vector>

// Self-differential: run chunks and check expected results (oracle literals).
// Full PUC-Rio process diff is wired later via tests/differential harness.

struct Case {
  const char* src;
  long long expect;
};

int test_differential() {
  std::vector<Case> cases = {
      {"return 1+2*3", 7},
      {"return (1+2)*3", 9},
      {"local a,b=3,4; return a*b", 12},
      {"if 1 then return 2 else return 3 end", 2},
      {"local x=0; while x < 3 do x = x + 1 end; return x", 3},
      {"local s=\"abcd\"; return #s", 4},
      {"return 10//3", 3},
      {"return 1<<4", 16},
      {"local t={1,2,3}; return t[3]", 3},
      {"local function f(x) return x*x end; return f(7)", 49},
      {"local a=1; local function f() a=a+1; return a end; return f()+f()", 5},
      {"local ok,v=pcall(function() return 9 end); return ok and v or 0", 9},
  };
  int failed = 0;
  for (auto& c : cases) {
    lua_State* L = luaL_newstate();
    int st = luaL_loadstring(L, c.src);
    if (st != LUA_OK) {
      std::cerr << "diff FAIL load: " << c.src << " :: " << lua_tostring(L, -1) << "\n";
      failed++;
      lua_close(L);
      continue;
    }
    st = lua_pcall(L, 0, 1, 0);
    if (st != LUA_OK) {
      std::cerr << "diff FAIL run: " << c.src << " :: " << lua_tostring(L, -1) << "\n";
      failed++;
      lua_close(L);
      continue;
    }
    if (lua_tointeger(L, -1) != c.expect) {
      std::cerr << "diff FAIL expect " << c.expect << " got " << lua_tointeger(L, -1) << " for "
                << c.src << "\n";
      failed++;
    }
    lua_close(L);
  }
  return failed ? 1 : 0;
}
