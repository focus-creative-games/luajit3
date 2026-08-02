#include "lib/libs.hpp"

#include "lib/ldump.hpp"
#include "lib/lib_util.hpp"
#include "runtime/string.hpp"
#include "vm/interpreter.hpp"
#include "vm/meta.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#ifndef LUA_OK
#define LUA_OK 0
#endif
#ifndef LUA_YIELD
#define LUA_YIELD 1
#endif
#ifndef LUA_MULTRET
#define LUA_MULTRET (-1)
#endif
#ifndef LUA_ERRSYNTAX
#define LUA_ERRSYNTAX 3
#endif

namespace lj3 {
using namespace lib;

static Closure* bind_env_closure(State* L, Proto* p) {
  Closure* cl = closure_new_lua(L, p);
  if (!p->upvalues.empty()) {
    auto* uv = L->gc.create<UpVal>(GcKind::UpVal);
    uv->open = false;
    uv->closed = TValue::obj(ValueTag::Table, L->globals);
    uv->thread = nullptr;
    uv->stack_index = -1;
    cl->upvals[0] = uv;
  }
  return cl;
}

static int load_chunk(State* L, std::string_view source, std::string_view chunk_name,
                      Table* env) {
  if (is_proto_dump(source)) {
    Proto* p = undump_proto(L, std::string(source), std::string(chunk_name));
    Closure* cl = bind_env_closure(L, p);
    if (env && !p->upvalues.empty())
      cl->upvals[0]->closed = TValue::obj(ValueTag::Table, env);
    L->push(TValue::obj(ValueTag::Function, cl));
    return LUA_OK;
  }
  return L->load_string(std::string(source), std::string(chunk_name));
}

static int base_print(State* L) {
  int n = L->gettop();
  for (int i = 1; i <= n; ++i) {
    if (i > 1)
      std::cout << '\t';
    std::cout << value_to_string(*L->at(i));
  }
  std::cout << '\n';
  return 0;
}

static int base_type(State* L) {
  const char* name = "nil";
  switch (L->at(1)->tag()) {
  case ValueTag::Nil: name = "nil"; break;
  case ValueTag::Bool: name = "boolean"; break;
  case ValueTag::Int:
  case ValueTag::Float: name = "number"; break;
  case ValueTag::String: name = "string"; break;
  case ValueTag::Table: name = "table"; break;
  case ValueTag::Function: name = "function"; break;
  case ValueTag::Thread: name = "thread"; break;
  case ValueTag::Userdata:
  case ValueTag::LightUserdata: name = "userdata"; break;
  default: break;
  }
  // Push result on top (Lua C API convention); do not wipe the window with settop(0),
  // which can clear live registers that share absolute slots with the C args.
  push_string(L, name);
  return 1;
}

static int base_error(State* L) {
  std::string msg = L->gettop() >= 1 ? value_to_string(*L->at(1)) : "error";
  panic(msg);
}

static int base_assert(State* L) {
  if (!L->at(1)->is_truthy()) {
    static thread_local std::string hold;
    hold = (L->gettop() >= 2) ? value_to_string(*L->at(2)) : "assertion failed!";
    panic(hold);
  }
  return L->gettop();
}

static int base_tonumber(State* L) {
  TValue* v = L->at(1);
  TValue out;
  if (try_to_number(*v, &out)) {
    L->settop(0);
    L->push(out);
    return 1;
  }
  L->settop(0);
  L->push(TValue::nil());
  return 1;
}

static int base_tostring(State* L) {
  std::string s = value_to_string(*L->at(1));
  L->settop(0);
  push_string(L, s);
  return 1;
}

static int base_select(State* L) {
  if (L->gettop() < 1)
    return 0;
  TValue* a = L->at(1);
  if (a->is_string() && a->as_string()->view() == "#") {
    int n = L->gettop() - 1;
    L->settop(0);
    L->push(TValue::integer(n));
    return 1;
  }
  int idx = static_cast<int>(a->is_number() ? a->to_number() : 1);
  if (idx < 0)
    idx = L->gettop() + idx + 1;
  else
    idx += 1;
  if (idx < 2 || idx > L->gettop()) {
    L->settop(0);
    return 0;
  }
  int n = L->gettop() - idx + 1;
  std::vector<TValue> outs(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    outs[static_cast<size_t>(i)] = *L->at(idx + i);
  L->settop(0);
  for (int i = 0; i < n; ++i)
    L->push(outs[static_cast<size_t>(i)]);
  return n;
}

static int base_pcall(State* L) {
  if (L->gettop() < 1)
    panic("bad argument to pcall");
  int nargs = L->gettop() - 1;
  TValue f = *L->at(1);
  std::vector<TValue> args(static_cast<size_t>(nargs));
  for (int i = 0; i < nargs; ++i)
    args[static_cast<size_t>(i)] = *L->at(2 + i);
  const int protect_frames = static_cast<int>(L->current->frames.size());
  try {
    L->settop(0);
    L->push(f);
    for (auto& a : args)
      L->push(a);
    if (!f.is_function())
      panic("attempt to call a non-function value");
    call_closure(L, f.as_closure(), nargs, LUA_MULTRET);
    int nret = L->gettop();
    std::vector<TValue> results(static_cast<size_t>(nret));
    for (int i = 0; i < nret; ++i)
      results[static_cast<size_t>(i)] = *L->at(i + 1);
    L->settop(0);
    L->push(TValue::boolean(true));
    for (auto& r : results)
      L->push(r);
    return 1 + nret;
  } catch (const Lj3Error& e) {
    while (static_cast<int>(L->current->frames.size()) > protect_frames) {
      L->close_upvals(L->current, L->current->frames.back().base);
      L->current->frames.pop_back();
    }
    L->settop(0);
    L->push(TValue::boolean(false));
    push_string(L, e.what());
    return 2;
  }
}

static int base_xpcall(State* L) {
  if (L->gettop() < 2)
    panic("bad argument to xpcall");
  TValue f = *L->at(1);
  TValue msgh = *L->at(2);
  const int protect_frames = static_cast<int>(L->current->frames.size());
  try {
    L->settop(0);
    L->push(f);
    if (!f.is_function())
      panic("attempt to call a non-function value");
    call_closure(L, f.as_closure(), 0, LUA_MULTRET);
    int nret = L->gettop();
    std::vector<TValue> results(static_cast<size_t>(nret));
    for (int i = 0; i < nret; ++i)
      results[static_cast<size_t>(i)] = *L->at(i + 1);
    L->settop(0);
    L->push(TValue::boolean(true));
    for (auto& r : results)
      L->push(r);
    return 1 + nret;
  } catch (const Lj3Error& e) {
    while (static_cast<int>(L->current->frames.size()) > protect_frames) {
      L->close_upvals(L->current, L->current->frames.back().base);
      L->current->frames.pop_back();
    }
    std::string emsg = e.what();
    L->settop(0);
    if (msgh.is_function()) {
      try {
        L->push(msgh);
        push_string(L, emsg);
        call_closure(L, msgh.as_closure(), 1, 1);
        TValue m = L->gettop() >= 1 ? *L->at(1) : TValue::nil();
        L->settop(0);
        L->push(TValue::boolean(false));
        L->push(m);
        return 2;
      } catch (...) {
        while (static_cast<int>(L->current->frames.size()) > protect_frames) {
          L->close_upvals(L->current, L->current->frames.back().base);
          L->current->frames.pop_back();
        }
        L->settop(0);
      }
    }
    L->push(TValue::boolean(false));
    push_string(L, emsg);
    return 2;
  }
}

static int base_getmetatable(State* L) {
  Table* mt = get_metatable(*L->at(1));
  L->settop(0);
  if (!mt)
    L->push(TValue::nil());
  else {
    TValue hide = mt->get(TValue::obj(ValueTag::String, L->intern("__metatable")));
    if (!hide.is_nil())
      L->push(hide);
    else
      L->push(TValue::obj(ValueTag::Table, mt));
  }
  return 1;
}

static int base_setmetatable(State* L) {
  TValue t = *L->at(1);
  if (!t.is_table())
    panic("setmetatable: first argument must be a table");
  Table* mt = nullptr;
  if (L->gettop() >= 2 && !L->at(2)->is_nil()) {
    if (!L->at(2)->is_table())
      panic("setmetatable: second argument must be a table or nil");
    mt = L->at(2)->as_table();
  }
  Table* cur = t.as_table()->metatable;
  if (cur) {
    TValue protect = cur->get(TValue::obj(ValueTag::String, L->intern("__metatable")));
    if (!protect.is_nil())
      panic("cannot change a protected metatable");
  }
  set_metatable(L, t, mt);
  L->settop(0);
  L->push(t);
  return 1;
}

static int base_rawget(State* L) {
  if (!L->at(1)->is_table())
    panic("rawget: table expected");
  TValue v = L->at(1)->as_table()->get(*L->at(2));
  L->settop(0);
  L->push(v);
  return 1;
}

static int base_rawset(State* L) {
  if (!L->at(1)->is_table())
    panic("rawset: table expected");
  L->at(1)->as_table()->set(L, *L->at(2), *L->at(3));
  TValue t = *L->at(1);
  L->settop(0);
  L->push(t);
  return 1;
}

static int base_rawequal(State* L) {
  bool eq = values_equal(*L->at(1), *L->at(2));
  L->settop(0);
  L->push(TValue::boolean(eq));
  return 1;
}

static int base_rawlen(State* L) {
  TValue* v = L->at(1);
  int64_t n = 0;
  if (v->is_string())
    n = static_cast<int64_t>(v->as_string()->len);
  else if (v->is_table())
    n = table_length(v->as_table());
  else
    panic("rawlen: table or string expected");
  L->settop(0);
  L->push(TValue::integer(n));
  return 1;
}

static bool key_as_next_index(const TValue& key, int64_t* out) {
  if (key.is_int()) {
    *out = key.as_int();
    return true;
  }
  if (key.is_float()) {
    double d = key.as_float();
    if (d >= 0.0 && d == std::floor(d) && d <= static_cast<double>(INT64_MAX)) {
      *out = static_cast<int64_t>(d);
      return true;
    }
  }
  return false;
}

static int base_next(State* L) {
  if (!L->at(1)->is_table())
    panic("next: table expected");
  Table* t = L->at(1)->as_table();
  TValue key = L->gettop() >= 2 ? *L->at(2) : TValue::nil();

  const int64_t asize = static_cast<int64_t>(t->array.size());
  int64_t ikey = 0;
  const bool key_is_index = !key.is_nil() && key_as_next_index(key, &ikey);

  // Traverse array part using the array vector only (not get_int, which can
  // fall back into the hash and re-surface the same integer key).
  int64_t i = 0;
  if (key.is_nil())
    i = 1;
  else if (key_is_index && ikey >= 1 && ikey <= asize)
    i = ikey + 1;
  else
    i = -1;

  if (i > 0) {
    for (; i <= asize; ++i) {
      const TValue& v = t->array[static_cast<size_t>(i - 1)];
      if (!v.is_nil()) {
        L->push(TValue::integer(i));
        L->push(v);
        return 2;
      }
    }
  }

  // Start hash from the beginning only after nil or a key that lived in array.
  bool seen = key.is_nil() || (key_is_index && ikey >= 1 && ikey <= asize);
  for (auto& n : t->hash) {
    if (!n.used || n.value.is_nil())
      continue;
    int64_t hk = 0;
    if (key_as_next_index(n.key, &hk) && hk >= 1 && hk <= asize)
      continue;
    if (!seen) {
      if (values_equal(n.key, key))
        seen = true;
      continue;
    }
    L->push(n.key);
    L->push(n.value);
    return 2;
  }
  L->push(TValue::nil());
  return 1;
}

static int pairs_next_wrap(State* L) { return base_next(L); }

static int base_pairs(State* L) {
  TValue mm = get_metamethod(L, *L->at(1), "__pairs");
  if (mm.is_function()) {
    L->push(mm);
    L->push(*L->at(1));
    call_closure(L, mm.as_closure(), 1, 3);
    return 3;
  }
  TValue t = *L->at(1);
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, closure_new_c(L, pairs_next_wrap)));
  L->push(t);
  L->push(TValue::nil());
  return 3;
}

static int ipairs_iter(State* L) {
  if (L->gettop() < 2 || !L->at(1)->is_table())
    panic("bad argument to ipairs iterator");
  Table* t = L->at(1)->as_table();
  int64_t i = L->at(2)->is_number() ? static_cast<int64_t>(L->at(2)->to_number()) + 1 : 1;
  TValue v = t->get_int(i);
  if (v.is_nil()) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  L->settop(0);
  L->push(TValue::integer(i));
  L->push(v);
  return 2;
}

static int base_ipairs(State* L) {
  TValue t = *L->at(1);
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, closure_new_c(L, ipairs_iter)));
  L->push(t);
  L->push(TValue::integer(0));
  return 3;
}

static int base_load(State* L) {
  std::string_view chunk;
  std::string chunk_name;
  Table* env = nullptr;

  if (L->at(1)->is_function()) {
    // load(reader) not implemented — use string only
    panic("load: function reader not supported");
  }
  if (!L->at(1)->is_string())
    panic("load: string or function expected");
  chunk = L->at(1)->as_string()->view();
  // Lua 5.3: default chunkname is the source string itself (not "=(load)").
  if (L->gettop() >= 2 && !L->at(2)->is_nil())
    chunk_name = std::string(L->at(2)->as_string()->view());
  else
    chunk_name = std::string(chunk);
  if (L->gettop() >= 4 && !L->at(4)->is_nil()) {
    if (!L->at(4)->is_table())
      panic("load: environment must be a table");
    env = L->at(4)->as_table();
  }

  try {
    L->settop(0);
    int st = load_chunk(L, chunk, chunk_name, env);
    if (st != LUA_OK) {
      TValue err = L->pop();
      L->push(TValue::nil());
      L->push(err);
      return 2;
    }
    return 1;
  } catch (const Lj3Error& e) {
    L->settop(0);
    L->push(TValue::nil());
    push_string(L, e.what());
    return 2;
  }
}

static int base_loadfile(State* L) {
  std::string filename = (L->gettop() >= 1 && !L->at(1)->is_nil())
                             ? std::string(L->at(1)->as_string()->view())
                             : "";
  Table* env = nullptr;
  if (L->gettop() >= 3 && !L->at(3)->is_nil()) {
    if (!L->at(3)->is_table())
      panic("loadfile: environment must be a table");
    env = L->at(3)->as_table();
  }

  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    L->settop(0);
    L->push(TValue::nil());
    push_string(L, "cannot open " + filename + ": No such file or directory");
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string source = ss.str();
  std::string chunk_name = "@" + filename;

  try {
    L->settop(0);
    int st = load_chunk(L, source, chunk_name, env);
    if (st != LUA_OK) {
      TValue err = L->pop();
      L->push(TValue::nil());
      L->push(err);
      return 2;
    }
    return 1;
  } catch (const Lj3Error& e) {
    L->settop(0);
    L->push(TValue::nil());
    push_string(L, e.what());
    return 2;
  }
}

static int base_dofile(State* L) {
  base_loadfile(L);
  if (L->gettop() >= 2 && L->at(1)->is_nil()) {
    push_string(L, value_to_string(*L->at(2)));
    panic(value_to_string(*L->at(2)));
  }
  if (L->gettop() < 1 || !L->at(1)->is_function())
    return 0;
  TValue f = *L->at(1);
  L->settop(0);
  L->push(f);
  call_closure(L, f.as_closure(), 0, LUA_MULTRET);
  return L->gettop();
}

static int gc_pause = 200;
static int gc_stepmul = 200;

static int base_collectgarbage(State* L) {
  const char* opt = "collect";
  if (L->gettop() >= 1 && !L->at(1)->is_nil())
    opt = L->at(1)->as_string()->view().data();
  int64_t arg2 = 0;
  bool has_arg2 = L->gettop() >= 2;
  if (has_arg2)
    arg2 = static_cast<int64_t>(L->at(2)->to_number());

  L->settop(0);
  if (std::strcmp(opt, "collect") == 0) {
    // Full collection runs even when automatic GC is stopped (PUC Lua 5.3).
    L->gc.full_gc();
    L->push(TValue::boolean(true));
    return 1;
  }
  if (std::strcmp(opt, "count") == 0) {
    double kb = static_cast<double>(L->gc.debt_) / 1024.0;
    L->push(TValue::number(kb));
    return 1;
  }
  if (std::strcmp(opt, "step") == 0) {
    // Incremental step also runs while stopped (manual control).
    bool finished = false;
    int work = static_cast<int>(arg2);
    if (work <= 0)
      work = 1;
    if (work >= 10000) {
      L->gc.full_gc();
      finished = true;
    } else {
      auto start = L->gc.phase();
      for (int i = 0; i < work; ++i) {
        L->gc.step();
        if (start != GC::Phase::Pause && L->gc.phase() == GC::Phase::Pause) {
          finished = true;
          break;
        }
        start = L->gc.phase();
      }
    }
    L->push(TValue::boolean(finished));
    return 1;
  }
  if (std::strcmp(opt, "stop") == 0) {
    bool was = L->gc.is_running();
    L->gc.set_running(false);
    L->push(TValue::boolean(was));
    return 1;
  }
  if (std::strcmp(opt, "restart") == 0) {
    L->gc.set_running(true);
    L->push(TValue::boolean(true));
    return 1;
  }
  if (std::strcmp(opt, "isrunning") == 0) {
    L->push(TValue::boolean(L->gc.is_running()));
    return 1;
  }
  if (std::strcmp(opt, "setpause") == 0) {
    int old = gc_pause;
    if (has_arg2)
      gc_pause = static_cast<int>(arg2);
    L->push(TValue::integer(old));
    return 1;
  }
  if (std::strcmp(opt, "setstepmul") == 0) {
    int old = gc_stepmul;
    if (has_arg2)
      gc_stepmul = static_cast<int>(arg2);
    L->push(TValue::integer(old));
    return 1;
  }
  panic("collectgarbage: invalid option");
}

void open_base_lib(State* L) {
  set_global(L, "print", base_print);
  set_global(L, "type", base_type);
  set_global(L, "assert", base_assert);
  set_global(L, "error", base_error);
  set_global(L, "tonumber", base_tonumber);
  set_global(L, "tostring", base_tostring);
  set_global(L, "select", base_select);
  set_global(L, "pcall", base_pcall);
  set_global(L, "xpcall", base_xpcall);
  set_global(L, "getmetatable", base_getmetatable);
  set_global(L, "setmetatable", base_setmetatable);
  set_global(L, "rawget", base_rawget);
  set_global(L, "rawset", base_rawset);
  set_global(L, "rawequal", base_rawequal);
  set_global(L, "rawlen", base_rawlen);
  set_global(L, "next", base_next);
  set_global(L, "pairs", base_pairs);
  set_global(L, "ipairs", base_ipairs);
  set_global(L, "load", base_load);
  set_global(L, "loadfile", base_loadfile);
  set_global(L, "dofile", base_dofile);
  set_global(L, "collectgarbage", base_collectgarbage);
  set_global_value(L, "_VERSION", TValue::obj(ValueTag::String, L->intern("Lua 5.3")));
}

} // namespace lj3
