#pragma once

#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/userdata.hpp"
#include "runtime/value.hpp"
#include "vm/bytecode.hpp"
#include "vm/ldebug.hpp"
#include "vm/state.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <string_view>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace luatier {
namespace lib {

inline void push_string(State* L, const char* s) {
  L->push(TValue::obj(ValueTag::String, L->intern(s ? s : "")));
}

inline void push_string(State* L, std::string_view s) {
  L->push(TValue::obj(ValueTag::String, L->intern(s)));
}

inline void set_global(State* L, const char* name, CFunction f) {
  Closure* cl = closure_new_c(L, f);
  cl->cname = name;
  L->globals->set(L, TValue::obj(ValueTag::String, L->intern(name)),
                  TValue::obj(ValueTag::Function, cl));
}

inline void set_global_value(State* L, const char* name, const TValue& v) {
  L->globals->set(L, TValue::obj(ValueTag::String, L->intern(name)), v);
}

inline void set_field(State* L, Table* t, const char* name, CFunction f) {
  Closure* cl = closure_new_c(L, f);
  cl->cname = name;
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)),
         TValue::obj(ValueTag::Function, cl));
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

// String library methods used as obj:method() should say "bad self".
inline LjString* check_string_self(State* L, int idx) {
  if (idx > L->gettop() || !L->at(idx)->is_string())
    panic("bad self");
  return L->at(idx)->as_string();
}

inline Table* check_table(State* L, int idx) {
  check_any(L, idx, "check_table");
  if (!L->at(idx)->is_table())
    panic("table expected");
  return L->at(idx)->as_table();
}

// PUC luaL_execresult: true/"exit"/0 or nil/"exit"| "signal"/code.
inline int exec_result(State* L, int stat) {
  const char* what = "exit";
  if (stat == -1) {
    int en = errno;
    L->settop(0);
    L->push(TValue::nil());
    push_string(L, std::strerror(en));
    L->push(TValue::integer(en));
    return 3;
  }
#if !defined(_WIN32)
  if (WIFEXITED(stat)) {
    stat = WEXITSTATUS(stat);
  } else if (WIFSIGNALED(stat)) {
    stat = WTERMSIG(stat);
    what = "signal";
  }
#endif
  L->settop(0);
  if (what[0] == 'e' && stat == 0)
    L->push(TValue::boolean(true));
  else
    L->push(TValue::nil());
  push_string(L, what);
  L->push(TValue::integer(stat));
  return 3;
}

[[noreturn]] inline void arg_type_error(State* L, int idx, const char* expected) {
  std::string got = obj_type_name(L, *L->at(idx));
  const char* fname = "?";
  bool is_method = false;
  Thread* th = L->current;
  const char* nm = nullptr;
  Closure* target = nullptr;
  if (th && !th->frames.empty() && !th->frames.back().invoked_name.empty())
    nm = th->frames.back().invoked_name.c_str();
  if (th && !th->frames.empty()) {
    CallFrame& fr = th->frames.back();
    if (fr.cl && fr.cl->is_c)
      target = fr.cl;
    if (!nm && th->frames.size() >= 2) {
      CallFrame* caller = &th->frames[th->frames.size() - 2];
      const char* what = debug_funcnamefromcode(caller, &nm);
      if (what && std::strcmp(what, "method") == 0)
        is_method = true;
    } else if (th->frames.size() >= 2) {
      CallFrame* caller = &th->frames[th->frames.size() - 2];
      const char* dummy = nullptr;
      const char* what = debug_funcnamefromcode(caller, &dummy);
      if (what && std::strcmp(what, "method") == 0)
        is_method = true;
    }
  }
  // PUC pushglobalfuncname: search globals for a dotted path to this C function.
  std::string dotted;
  if (target && L->globals) {
    auto find_in = [&](Table* tab, const std::string& prefix) -> bool {
      for (auto& n : tab->hash) {
        if (!n.used || !n.key.is_string() || !n.value.is_function())
          continue;
        if (n.value.as_closure() != target)
          continue;
        dotted = prefix + std::string(n.key.as_string()->view());
        return true;
      }
      return false;
    };
    if (!find_in(L->globals, "")) {
      for (auto& n : L->globals->hash) {
        if (!n.used || !n.key.is_string() || !n.value.is_table())
          continue;
        std::string pref = std::string(n.key.as_string()->view()) + ".";
        if (find_in(n.value.as_table(), pref))
          break;
      }
    }
  }
  if (nm)
    fname = nm;
  else if (!dotted.empty())
    fname = dotted.c_str();
  else if (target && target->cname)
    fname = target->cname;

  int arg = idx;
  if (is_method) {
    arg--;
    if (arg == 0)
      panic(std::string("calling '") + fname + "' on bad self (" + expected +
            " expected, got " + got + ")");
  }
  panic(std::string("bad argument #") + std::to_string(arg) + " to '" + fname + "' (" +
        expected + " expected, got " + got + ")");
}

inline int64_t check_int(State* L, int idx) {
  check_any(L, idx, "check_int");
  TValue* v = L->at(idx);
  auto from_number = [&](const TValue& n) -> int64_t {
    if (n.is_int())
      return n.as_int();
    if (n.is_float()) {
      double d = n.as_float();
      if (std::floor(d) == d && d >= static_cast<double>(INT64_MIN) &&
          d <= static_cast<double>(INT64_MAX))
        return static_cast<int64_t>(d);
      panic("number has no integer representation");
    }
    arg_type_error(L, idx, "integer");
  };
  if (v->is_number())
    return from_number(*v);
  TValue n;
  if (try_to_number(*v, &n))
    return from_number(n);
  arg_type_error(L, idx, "integer");
}

// luaL_optinteger: missing or nil → def.
inline int64_t opt_int(State* L, int idx, int64_t def) {
  if (idx > L->gettop() || L->at(idx)->is_nil())
    return def;
  return check_int(L, idx);
}

inline double check_number(State* L, int idx) {
  check_any(L, idx, "check_number");
  TValue* v = L->at(idx);
  if (v->is_number())
    return v->to_number();
  TValue n;
  if (try_to_number(*v, &n) && n.is_number())
    return n.to_number();
  arg_type_error(L, idx, "number");
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
