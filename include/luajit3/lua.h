#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define LUA_VERSION_MAJOR "5"
#define LUA_VERSION_MINOR "3"
#define LUA_VERSION_RELEASE "0"
#define LJ3_VERSION "0.1.0"

#define LUA_MULTRET (-1)
#define LUA_OK 0
#define LUA_YIELD 1
#define LUA_ERRRUN 2
#define LUA_ERRSYNTAX 3
#define LUA_ERRMEM 4
#define LUA_ERRERR 5

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State* L);

lua_State* lua_newstate(void* (*alloc)(void* ud, void* ptr, size_t osize, size_t nsize), void* ud);
void lua_close(lua_State* L);
lua_State* luaL_newstate(void);

int luaL_loadstring(lua_State* L, const char* s);
int luaL_loadfile(lua_State* L, const char* filename);
int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc);
int lua_gettop(lua_State* L);
void lua_settop(lua_State* L, int idx);
void lua_pushnil(lua_State* L);
void lua_pushboolean(lua_State* L, int b);
void lua_pushinteger(lua_State* L, long long n);
void lua_pushnumber(lua_State* L, double n);
void lua_pushstring(lua_State* L, const char* s);
int lua_toboolean(lua_State* L, int idx);
long long lua_tointeger(lua_State* L, int idx);
double lua_tonumber(lua_State* L, int idx);
const char* lua_tostring(lua_State* L, int idx);
int lua_type(lua_State* L, int idx);
const char* lua_typename(lua_State* L, int tp);

#define LUA_TNONE (-1)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TLIGHTUSERDATA 2
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
#define LUA_TTABLE 5
#define LUA_TFUNCTION 6
#define LUA_TUSERDATA 7
#define LUA_TTHREAD 8

#ifdef __cplusplus
}
#endif
