#include "luajit3/lua.h"

#include "vm/state.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace lj3;

struct lua_State {
  State* st;
};

static void* default_alloc(void*, void* ptr, size_t, size_t nsize) {
  if (nsize == 0) {
    std::free(ptr);
    return nullptr;
  }
  return std::realloc(ptr, nsize);
}

lua_State* lua_newstate(void* (*)(void*, void*, size_t, size_t), void*) {
  auto* ls = new lua_State;
  ls->st = new_state().release();
  return ls;
}

void lua_close(lua_State* L) {
  delete L->st;
  delete L;
}

lua_State* luaL_newstate(void) { return lua_newstate(default_alloc, nullptr); }

int luaL_loadstring(lua_State* L, const char* s) {
  return L->st->load_string(s ? s : "", "=string");
}

int luaL_loadfile(lua_State* L, const char* filename) {
  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    L->st->push(TValue::obj(ValueTag::String, L->st->intern("cannot open file")));
    return LUA_ERRSYNTAX;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return L->st->load_string(ss.str(), std::string("@") + filename);
}

int lua_pcall(lua_State* L, int nargs, int nresults, int) {
  int func_idx = L->st->gettop() - nargs;
  try {
    if (!L->st->at(func_idx)->is_function()) {
      L->st->settop(func_idx - 1);
      L->st->push(TValue::obj(ValueTag::String, L->st->intern("pcall: not a function")));
      return LUA_ERRRUN;
    }
    Closure* cl = L->st->at(func_idx)->as_closure();
    return L->st->resume_call(cl, nargs, nresults);
  } catch (const Lj3Error& e) {
    L->st->settop(func_idx - 1);
    L->st->push(TValue::obj(ValueTag::String, L->st->intern(e.what())));
    return LUA_ERRRUN;
  }
}

int lua_gettop(lua_State* L) { return L->st->gettop(); }

void lua_settop(lua_State* L, int idx) { L->st->settop(idx); }

void lua_pushnil(lua_State* L) { L->st->push(TValue::nil()); }
void lua_pushboolean(lua_State* L, int b) { L->st->push(TValue::boolean(b != 0)); }
void lua_pushinteger(lua_State* L, long long n) { L->st->push(TValue::integer(n)); }
void lua_pushnumber(lua_State* L, double n) { L->st->push(TValue::number(n)); }
void lua_pushstring(lua_State* L, const char* s) {
  L->st->push(TValue::obj(ValueTag::String, L->st->intern(s ? s : "")));
}

int lua_toboolean(lua_State* L, int idx) { return L->st->at(idx)->is_truthy() ? 1 : 0; }
long long lua_tointeger(lua_State* L, int idx) {
  auto* v = L->st->at(idx);
  if (v->is_int())
    return v->as_int();
  if (v->is_float())
    return static_cast<long long>(v->as_float());
  return 0;
}
double lua_tonumber(lua_State* L, int idx) {
  auto* v = L->st->at(idx);
  if (v->is_number())
    return v->to_number();
  return 0;
}
const char* lua_tostring(lua_State* L, int idx) {
  auto* v = L->st->at(idx);
  if (!v->is_string())
    return nullptr;
  return v->as_string()->data;
}

int lua_type(lua_State* L, int idx) {
  if (idx > L->st->gettop() || idx == 0)
    return LUA_TNONE;
  switch (L->st->at(idx)->tag()) {
  case ValueTag::Nil: return LUA_TNIL;
  case ValueTag::Bool: return LUA_TBOOLEAN;
  case ValueTag::Int:
  case ValueTag::Float: return LUA_TNUMBER;
  case ValueTag::String: return LUA_TSTRING;
  case ValueTag::Table: return LUA_TTABLE;
  case ValueTag::Function: return LUA_TFUNCTION;
  case ValueTag::Userdata: return LUA_TUSERDATA;
  case ValueTag::Thread: return LUA_TTHREAD;
  case ValueTag::LightUserdata: return LUA_TLIGHTUSERDATA;
  default: return LUA_TNIL;
  }
}

const char* lua_typename(lua_State*, int tp) {
  switch (tp) {
  case LUA_TNIL: return "nil";
  case LUA_TBOOLEAN: return "boolean";
  case LUA_TNUMBER: return "number";
  case LUA_TSTRING: return "string";
  case LUA_TTABLE: return "table";
  case LUA_TFUNCTION: return "function";
  case LUA_TUSERDATA: return "userdata";
  case LUA_TTHREAD: return "thread";
  default: return "no value";
  }
}
