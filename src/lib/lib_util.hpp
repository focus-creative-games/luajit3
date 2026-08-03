#pragma once

#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/userdata.hpp"
#include "runtime/value.hpp"
#include "vm/state.hpp"

#include <cmath>
#include <string_view>

namespace luatier {
namespace lib {

inline void push_string(State* L, const char* s) {
  L->push(TValue::obj(ValueTag::String, L->intern(s ? s : "")));
}

inline void push_string(State* L, std::string_view s) {
  L->push(TValue::obj(ValueTag::String, L->intern(s)));
}

inline void set_global(State* L, const char* name, CFunction f) {
  L->globals->set(L, TValue::obj(ValueTag::String, L->intern(name)),
                  TValue::obj(ValueTag::Function, closure_new_c(L, f)));
}

inline void set_global_value(State* L, const char* name, const TValue& v) {
  L->globals->set(L, TValue::obj(ValueTag::String, L->intern(name)), v);
}

inline void set_field(State* L, Table* t, const char* name, CFunction f) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)),
         TValue::obj(ValueTag::Function, closure_new_c(L, f)));
}

inline void set_field_value(State* L, Table* t, const char* name, const TValue& v) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)), v);
}

inline void check_type(State* L, int idx, ValueTag tag, const char* what) {
  if (L->at(idx)->tag() != tag)
    panic(std::string(what) + ": " + std::string(value_to_string(*L->at(idx))) + " expected");
}

inline void check_any(State* L, int idx, const char* what) {
  if (idx < 1 || idx > L->gettop())
    panic(std::string(what) + ": bad argument #" + std::to_string(idx));
}

inline LjString* check_string(State* L, int idx) {
  check_any(L, idx, "check_string");
  if (!L->at(idx)->is_string())
    panic("string expected");
  return L->at(idx)->as_string();
}

inline Table* check_table(State* L, int idx) {
  check_any(L, idx, "check_table");
  if (!L->at(idx)->is_table())
    panic("table expected");
  return L->at(idx)->as_table();
}

inline int64_t check_int(State* L, int idx) {
  check_any(L, idx, "check_int");
  TValue* v = L->at(idx);
  if (v->is_int())
    return v->as_int();
  // Lua 5.3: integer-valued floats are accepted by luaL_checkinteger.
  if (v->is_float()) {
    double d = v->as_float();
    if (std::floor(d) == d && d >= static_cast<double>(INT64_MIN) &&
        d <= static_cast<double>(INT64_MAX))
      return static_cast<int64_t>(d);
  }
  panic("integer expected");
}

// luaL_optinteger: missing or nil → def.
inline int64_t opt_int(State* L, int idx, int64_t def) {
  if (idx > L->gettop() || L->at(idx)->is_nil())
    return def;
  return check_int(L, idx);
}

inline double check_number(State* L, int idx) {
  check_any(L, idx, "check_number");
  if (!L->at(idx)->is_number())
    panic("number expected");
  return L->at(idx)->to_number();
}

inline bool opt_bool(State* L, int idx, bool def) {
  if (idx > L->gettop() || L->at(idx)->is_nil())
    return def;
  return L->at(idx)->is_truthy();
}

inline std::string_view opt_string(State* L, int idx, std::string_view def) {
  if (idx > L->gettop() || L->at(idx)->is_nil())
    return def;
  if (!L->at(idx)->is_string())
    panic("string expected");
  return L->at(idx)->as_string()->view();
}

inline int64_t to_int(TValue* v) {
  if (v->is_int())
    return v->as_int();
  if (v->is_float())
    return static_cast<int64_t>(v->as_float());
  panic("integer expected");
}

inline Table* new_lib(State* L, int nrec) {
  return table_new(L, 0, static_cast<size_t>(nrec));
}

} // namespace lib
} // namespace luatier
