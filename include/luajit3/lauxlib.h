#pragma once

#include "luajit3/lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_REGISTRYINDEX (-10000)
#define LUA_ENVIRONINDEX (-10001)
#define LUA_GLOBALSINDEX (-10002)

void luaL_openlibs(lua_State* L);
void luaL_requiref(lua_State* L, const char* modname, lua_CFunction openf, int glb);

void lua_pushvalue(lua_State* L, int idx);
void lua_setfield(lua_State* L, int idx, const char* k);
void lua_getfield(lua_State* L, int idx, const char* k);
void lua_setglobal(lua_State* L, const char* name);
void lua_getglobal(lua_State* L, const char* name);
void lua_rawgeti(lua_State* L, int idx, long long n);
void lua_rawseti(lua_State* L, int idx, long long n);

#define luaL_checkstring(L, n) lua_tostring(L, n)
#define luaL_optstring(L, n, d) (lua_type(L, n) == LUA_TSTRING ? lua_tostring(L, n) : (d))
#define luaL_checkinteger(L, n) lua_tointeger(L, n)
#define luaL_checknumber(L, n) lua_tonumber(L, n)
#define luaL_checktype(L, n, t) ((void)((lua_type(L, n) == (t)) ? 0 : 0))
#define luaL_error(L, fmt) (lua_pushstring(L, fmt), lua_error(L))
int lua_error(lua_State* L);

#ifdef __cplusplus
}
#endif
