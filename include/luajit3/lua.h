#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define LUA_VERSION_MAJOR "5"
#define LUA_VERSION_MINOR "3"
#define LUA_VERSION_RELEASE "4"
#define LUA_VERSION_NUM 503
#define LUA_VERSION "Lua 5.3"
#define LJ3_VERSION "0.1.0"

#define LUA_MULTRET (-1)
#define LUA_OK 0
#define LUA_YIELD 1
#define LUA_ERRRUN 2
#define LUA_ERRSYNTAX 3
#define LUA_ERRMEM 4
#define LUA_ERRERR 5
#define LUA_ERRFILE 6

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State* L);

lua_State* lua_newstate(void* (*alloc)(void* ud, void* ptr, size_t osize, size_t nsize), void* ud);
void lua_close(lua_State* L);
lua_State* luaL_newstate(void);

void luaL_openlibs(lua_State* L);

int luaL_loadstring(lua_State* L, const char* s);
int luaL_loadfile(lua_State* L, const char* filename);
int luaL_loadbuffer(lua_State* L, const char* buff, size_t sz, const char* name);
int luaL_dofile(lua_State* L, const char* filename);
int luaL_dostring(lua_State* L, const char* s);

int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc);
void lua_call(lua_State* L, int nargs, int nresults);

int lua_gettop(lua_State* L);
void lua_settop(lua_State* L, int idx);
void lua_pop(lua_State* L, int n);
void lua_pushvalue(lua_State* L, int idx);
void lua_remove(lua_State* L, int idx);
void lua_insert(lua_State* L, int idx);

void lua_pushnil(lua_State* L);
void lua_pushboolean(lua_State* L, int b);
void lua_pushinteger(lua_State* L, long long n);
void lua_pushnumber(lua_State* L, double n);
void lua_pushstring(lua_State* L, const char* s);
void lua_pushcfunction(lua_State* L, lua_CFunction f);

void lua_setglobal(lua_State* L, const char* name);
void lua_getglobal(lua_State* L, const char* name);
void lua_setfield(lua_State* L, int idx, const char* k);
void lua_getfield(lua_State* L, int idx, const char* k);
void lua_rawseti(lua_State* L, int idx, long long n);
void lua_rawgeti(lua_State* L, int idx, long long n);
void lua_createtable(lua_State* L, int narr, int nrec);
#define lua_newtable(L) lua_createtable(L, 0, 0)

int lua_isnil(lua_State* L, int idx);
int lua_isboolean(lua_State* L, int idx);
int lua_isnumber(lua_State* L, int idx);
int lua_isstring(lua_State* L, int idx);
int lua_istable(lua_State* L, int idx);
int lua_isfunction(lua_State* L, int idx);

int lua_toboolean(lua_State* L, int idx);
long long lua_tointeger(lua_State* L, int idx);
double lua_tonumber(lua_State* L, int idx);
const char* lua_tostring(lua_State* L, int idx);
size_t lua_rawlen(lua_State* L, int idx);

int lua_type(lua_State* L, int idx);
const char* lua_typename(lua_State* L, int tp);
int lua_error(lua_State* L);

void luaL_requiref(lua_State* L, const char* modname, lua_CFunction openf, int glb);

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

#define LUA_REGISTRYINDEX (-10000)
#define LUA_GLOBALSINDEX (-10002)

#ifdef __cplusplus
}
#endif
