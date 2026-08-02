#include "luajit3/lua.h"

#include <iostream>
#include <string>

static int fail(const char* msg) {
  std::cerr << "vm FAIL: " << msg << "\n";
  return 1;
}

int test_vm() {
  lua_State* L = luaL_newstate();
  int st = luaL_loadstring(L, "local x = 10\nreturn x + 32");
  if (st != LUA_OK)
    return fail(lua_tostring(L, -1));
  st = lua_pcall(L, 0, 1, 0);
  if (st != LUA_OK)
    return fail(lua_tostring(L, -1));
  if (lua_tointeger(L, -1) != 42)
    return fail("expected 42");

  lua_settop(L, 0);
  st = luaL_loadstring(L, "local s = 0\nfor i = 1, 10 do s = s + i end\nreturn s");
  if (st != LUA_OK)
    return fail(lua_tostring(L, -1));
  st = lua_pcall(L, 0, 1, 0);
  if (st != LUA_OK)
    return fail(lua_tostring(L, -1));
  if (lua_tointeger(L, -1) != 55)
    return fail("expected 55 from for-loop");

  lua_settop(L, 0);
  st = luaL_loadstring(L, "function add(a,b) return a+b end\nreturn add(20,22)");
  if (st != LUA_OK)
    return fail(lua_tostring(L, -1));
  st = lua_pcall(L, 0, 1, 0);
  if (st != LUA_OK)
    return fail(lua_tostring(L, -1));
  if (lua_tointeger(L, -1) != 42)
    return fail("expected 42 from function call");

  lua_close(L);
  return 0;
}
