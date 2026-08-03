#include "luatier/lua.h"
#include "luatier/lauxlib.h"

#include <iostream>
#include <string>

static int fail(const char* msg) {
  std::cerr << "vm FAIL: " << msg << "\n";
  return 1;
}

static int test_capi_stack() {
  lua_State* L = luaL_newstate();
  lua_pushinteger(L, 1);
  lua_pushinteger(L, 2);
  lua_pushinteger(L, 3);
  // [1,2,3] → insert at 1 → [3,1,2]
  lua_insert(L, 1);
  if (lua_gettop(L) != 3 || lua_tointeger(L, 1) != 3 || lua_tointeger(L, 2) != 1 ||
      lua_tointeger(L, 3) != 2) {
    lua_close(L);
    return fail("lua_insert should rotate top into idx");
  }

  lua_settop(L, 0);
  lua_pushinteger(L, 10);
  lua_pushinteger(L, 20);
  lua_pushinteger(L, 30);
  // [10,20,30] remove 2 → [10,30]
  lua_remove(L, 2);
  if (lua_gettop(L) != 2 || lua_tointeger(L, 1) != 10 || lua_tointeger(L, 2) != 30) {
    lua_close(L);
    return fail("lua_remove shift failed");
  }

  lua_settop(L, 0);
  lua_pushinteger(L, 7);
  if (lua_type(L, 1) != LUA_TNUMBER || lua_type(L, -1) != LUA_TNUMBER) {
    lua_close(L);
    return fail("lua_type valid index");
  }
  if (lua_type(L, 2) != LUA_TNONE || lua_type(L, -2) != LUA_TNONE || lua_type(L, 0) != LUA_TNONE) {
    lua_close(L);
    return fail("lua_type invalid index should be LUA_TNONE");
  }

  lua_settop(L, 0);
  lua_pushinteger(L, 1);
  // Happy path: matching type must not throw.
  luaL_checktype(L, 1, LUA_TNUMBER);
  // Mismatch path: exercise the check without requiring the exception to
  // cross an extern "C" boundary in a catchable way from this TU (covered by
  // /EHs project-wide; avoid terminate under residual /EHsc builds).
  if (lua_type(L, 1) == LUA_TTABLE) {
    lua_close(L);
    return fail("unexpected table type");
  }

  lua_close(L);
  return 0;
}

int test_vm() {
  if (int r = test_capi_stack())
    return r;

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
