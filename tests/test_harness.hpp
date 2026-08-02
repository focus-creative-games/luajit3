#pragma once

#include "luajit3/lua.h"

#include <cmath>
#include <iostream>
#include <string>

namespace lj3test {

inline int fail(const char* group, const std::string& msg) {
  std::cerr << group << " FAIL: " << msg << "\n";
  return 1;
}

struct Run {
  lua_State* L = nullptr;
  int status = LUA_OK;
  explicit Run(const char* src) {
    L = luaL_newstate();
    status = luaL_loadstring(L, src);
    if (status == LUA_OK)
      status = lua_pcall(L, 0, LUA_MULTRET, 0);
  }
  ~Run() {
    if (L)
      lua_close(L);
  }
  bool ok() const { return status == LUA_OK; }
  const char* err() const { return lua_tostring(L, -1); }
  long long integer(int idx = -1) const { return lua_tointeger(L, idx); }
  double number(int idx = -1) const { return lua_tonumber(L, idx); }
  std::string str(int idx = -1) const {
    const char* s = lua_tostring(L, idx);
    return s ? s : "";
  }
  bool boolean(int idx = -1) const { return lua_toboolean(L, idx) != 0; }
  int top() const { return lua_gettop(L); }
  int type(int idx = -1) const { return lua_type(L, idx); }
};

inline int expect_int(const char* group, const char* src, long long want) {
  Run r(src);
  if (!r.ok())
    return fail(group, std::string(src) + " :: " + (r.err() ? r.err() : "?"));
  if (r.integer() != want)
    return fail(group, std::string(src) + " expected int " + std::to_string(want) + " got " +
                            std::to_string(r.integer()));
  return 0;
}

inline int expect_num(const char* group, const char* src, double want, double eps = 1e-9) {
  Run r(src);
  if (!r.ok())
    return fail(group, std::string(src) + " :: " + (r.err() ? r.err() : "?"));
  double got = r.number();
  if (!(std::fabs(got - want) <= eps))
    return fail(group, std::string(src) + " expected num " + std::to_string(want) + " got " +
                            std::to_string(got));
  return 0;
}

inline int expect_str(const char* group, const char* src, const char* want) {
  Run r(src);
  if (!r.ok())
    return fail(group, std::string(src) + " :: " + (r.err() ? r.err() : "?"));
  if (r.str() != want)
    return fail(group, std::string(src) + " expected \"" + want + "\" got \"" + r.str() + "\"");
  return 0;
}

inline int expect_bool(const char* group, const char* src, bool want) {
  Run r(src);
  if (!r.ok())
    return fail(group, std::string(src) + " :: " + (r.err() ? r.err() : "?"));
  if (r.boolean() != want)
    return fail(group, std::string(src) + " expected bool " + (want ? "true" : "false"));
  return 0;
}

inline int expect_true(const char* group, const char* src) { return expect_bool(group, src, true); }
inline int expect_false(const char* group, const char* src) { return expect_bool(group, src, false); }

inline int expect_nil(const char* group, const char* src) {
  Run r(src);
  if (!r.ok())
    return fail(group, std::string(src) + " :: " + (r.err() ? r.err() : "?"));
  if (r.top() < 1 || r.type(-1) != LUA_TNIL)
    return fail(group, std::string(src) + " expected nil");
  return 0;
}

inline int expect_error(const char* group, const char* src) {
  Run r(src);
  if (r.ok())
    return fail(group, std::string(src) + " expected error");
  return 0;
}

} // namespace lj3test
