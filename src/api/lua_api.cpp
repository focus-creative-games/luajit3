#include "luajit3/lua.h"
#include "luajit3/lauxlib.h"

#include "lib/libs.hpp"
#include "vm/meta.hpp"
#include "vm/interpreter.hpp"
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

void luaL_openlibs(lua_State* L) { open_libs(L->st); }

void luaL_requiref(lua_State* L, const char* modname, lua_CFunction openf, int glb) {
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "loaded");
  lua_getfield(L, -1, modname);
  if (!lua_isnil(L, -1)) {
    lua_remove(L, -2);
    lua_remove(L, -2);
    return;
  }
  lua_pop(L, 1);
  openf(L);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, modname);
  lua_pop(L, 2);
  if (glb) {
    lua_pushvalue(L, -1);
    lua_setglobal(L, modname);
  }
}

int luaL_loadstring(lua_State* L, const char* s) {
  return L->st->load_string(s ? s : "", "=string");
}

int luaL_loadbuffer(lua_State* L, const char* buff, size_t sz, const char* name) {
  return L->st->load_string(std::string(buff ? buff : "", sz), name ? name : "=(load)");
}

int luaL_loadfile(lua_State* L, const char* filename) {
  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    L->st->push(TValue::obj(ValueTag::String, L->st->intern("cannot open file")));
    return LUA_ERRFILE;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return L->st->load_string(ss.str(), std::string("@") + filename);
}

int luaL_dofile(lua_State* L, const char* filename) {
  int st = luaL_loadfile(L, filename);
  if (st != LUA_OK)
    return st;
  return lua_pcall(L, 0, LUA_MULTRET, 0);
}

int luaL_dostring(lua_State* L, const char* s) {
  int st = luaL_loadstring(L, s);
  if (st != LUA_OK)
    return st;
  return lua_pcall(L, 0, LUA_MULTRET, 0);
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

void lua_call(lua_State* L, int nargs, int nresults) {
  if (lua_pcall(L, nargs, nresults, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    panic(msg ? msg : "lua_call failed");
  }
}

int lua_gettop(lua_State* L) { return L->st->gettop(); }

void lua_settop(lua_State* L, int idx) { L->st->settop(idx); }

void lua_pop(lua_State* L, int n) { L->st->settop(L->st->gettop() - n); }

void lua_pushvalue(lua_State* L, int idx) { L->st->push(*L->st->at(idx)); }

void lua_remove(lua_State* L, int idx) {
  idx = L->st->absindex(idx);
  for (int i = idx; i < L->st->gettop(); ++i)
    L->st->current->stack[static_cast<size_t>(i - 1)] = *L->st->at(i + 1);
  L->st->settop(L->st->gettop() - 1);
}

void lua_insert(lua_State* L, int idx) {
  idx = L->st->absindex(idx);
  TValue v = *L->st->at(L->st->gettop());
  for (int i = L->st->gettop(); i > idx; --i)
    L->st->current->stack[static_cast<size_t>(i)] = *L->st->at(i - 1);
  L->st->current->stack[static_cast<size_t>(idx - 1)] = v;
}

void lua_pushnil(lua_State* L) { L->st->push(TValue::nil()); }
void lua_pushboolean(lua_State* L, int b) { L->st->push(TValue::boolean(b != 0)); }
void lua_pushinteger(lua_State* L, long long n) { L->st->push(TValue::integer(n)); }
void lua_pushnumber(lua_State* L, double n) { L->st->push(TValue::number(n)); }
void lua_pushstring(lua_State* L, const char* s) {
  L->st->push(TValue::obj(ValueTag::String, L->st->intern(s ? s : "")));
}

void lua_pushcfunction(lua_State* L, lua_CFunction f) {
  L->st->push(TValue::obj(ValueTag::Function,
                          closure_new_c(L->st, reinterpret_cast<CFunction>(f))));
}

void lua_setglobal(lua_State* L, const char* name) {
  L->st->globals->set(L->st, TValue::obj(ValueTag::String, L->st->intern(name)), *L->st->at(L->st->gettop()));
  L->st->pop();
}

void lua_getglobal(lua_State* L, const char* name) {
  L->st->push(L->st->globals->get(TValue::obj(ValueTag::String, L->st->intern(name))));
}

static Table* idx_table(lua_State* L, int idx) {
  if (idx == LUA_REGISTRYINDEX)
    return L->st->registry;
  if (idx == LUA_GLOBALSINDEX)
    return L->st->globals;
  return L->st->at(idx)->as_table();
}

void lua_setfield(lua_State* L, int idx, const char* k) {
  Table* t = idx_table(L, idx);
  t->set(L->st, TValue::obj(ValueTag::String, L->st->intern(k)), *L->st->at(L->st->gettop()));
  L->st->pop();
}

void lua_getfield(lua_State* L, int idx, const char* k) {
  Table* t = idx_table(L, idx);
  L->st->push(t->get(TValue::obj(ValueTag::String, L->st->intern(k))));
}

void lua_rawseti(lua_State* L, int idx, long long n) {
  Table* t = idx_table(L, idx);
  t->set_int(L->st, n, *L->st->at(L->st->gettop()));
  L->st->pop();
}

void lua_rawgeti(lua_State* L, int idx, long long n) {
  Table* t = idx_table(L, idx);
  L->st->push(t->get_int(n));
}

void lua_createtable(lua_State* L, int narr, int nrec) {
  L->st->push(TValue::obj(ValueTag::Table, table_new(L->st, narr, nrec)));
}

int lua_isnil(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TNIL; }
int lua_isboolean(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TBOOLEAN; }
int lua_isnumber(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TNUMBER; }
int lua_isstring(lua_State* L, int idx) {
  int t = lua_type(L, idx);
  return t == LUA_TSTRING || t == LUA_TNUMBER;
}
int lua_istable(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TTABLE; }
int lua_isfunction(lua_State* L, int idx) { return lua_type(L, idx) == LUA_TFUNCTION; }

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

size_t lua_rawlen(lua_State* L, int idx) {
  auto* v = L->st->at(idx);
  if (v->is_string())
    return v->as_string()->len;
  if (v->is_table())
    return static_cast<size_t>(table_length(v->as_table()));
  return 0;
}

int lua_type(lua_State* L, int idx) {
  if (idx == LUA_REGISTRYINDEX)
    return LUA_TTABLE;
  if (idx == LUA_GLOBALSINDEX)
    return LUA_TTABLE;
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

int lua_error(lua_State* L) {
  const char* msg = lua_tostring(L, -1);
  panic(msg ? msg : "lua error");
}
