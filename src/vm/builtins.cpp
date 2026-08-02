#include "vm/builtins.hpp"

#include "runtime/string.hpp"
#include "vm/interpreter.hpp"
#include "vm/meta.hpp"

#include <iostream>

#ifndef LUA_OK
#define LUA_OK 0
#endif
#ifndef LUA_YIELD
#define LUA_YIELD 1
#endif
#ifndef LUA_MULTRET
#define LUA_MULTRET (-1)
#endif

namespace lj3 {

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
  L->settop(0);
  L->push(TValue::obj(ValueTag::String, L->intern(name)));
  return 1;
}

static int base_error(State* L) {
  std::string msg = L->gettop() >= 1 ? value_to_string(*L->at(1)) : "error";
  panic(msg);
}

static int base_assert(State* L) {
  if (!L->at(1)->is_truthy()) {
    const char* msg = "assertion failed!";
    if (L->gettop() >= 2)
      msg = value_to_string(*L->at(2)).c_str();
    // copy to stable storage
    static thread_local std::string hold;
    hold = (L->gettop() >= 2) ? value_to_string(*L->at(2)) : "assertion failed!";
    panic(hold);
  }
  return L->gettop();
}

static int base_tonumber(State* L) {
  TValue* v = L->at(1);
  if (v->is_number()) {
    TValue keep = *v;
    L->settop(0);
    L->push(keep);
    return 1;
  }
  if (v->is_string()) {
    try {
      std::string s(v->as_string()->view());
      size_t idx = 0;
      if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
          s.find('E') == std::string::npos) {
        long long n = std::stoll(s, &idx, 0);
        if (idx == s.size()) {
          L->settop(0);
          L->push(TValue::integer(n));
          return 1;
        }
      }
      double d = std::stod(s, &idx);
      if (idx == s.size()) {
        L->settop(0);
        L->push(TValue::number(d));
        return 1;
      }
    } catch (...) {
    }
  }
  L->settop(0);
  L->push(TValue::nil());
  return 1;
}

static int base_tostring(State* L) {
  TValue mm = get_metamethod(L, *L->at(1), "__tostring");
  if (mm.is_function()) {
    L->push(mm);
    L->push(*L->at(1));
    // stack messy ??simplify
  }
  std::string s = value_to_string(*L->at(1));
  L->settop(0);
  L->push(TValue::obj(ValueTag::String, L->intern(s)));
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
  for (int i = 0; i < n; ++i)
    L->current->stack[static_cast<size_t>(i)] = *L->at(idx + i);
  L->settop(n);
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
    // stack: results...
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
    L->push(TValue::obj(ValueTag::String, L->intern(e.what())));
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
        L->push(TValue::obj(ValueTag::String, L->intern(emsg)));
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
    L->push(TValue::obj(ValueTag::String, L->intern(emsg)));
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

static int base_next(State* L) {
  if (!L->at(1)->is_table())
    panic("next: table expected");
  Table* t = L->at(1)->as_table();
  TValue key = L->gettop() >= 2 ? *L->at(2) : TValue::nil();

  // Iterate array then hash
  int64_t start = 0;
  if (key.is_nil())
    start = 1;
  else if (key.is_int() && key.as_int() >= 0)
    start = key.as_int() + 1;
  else
    start = -1; // go to hash

  if (start > 0) {
    for (int64_t i = start; i <= static_cast<int64_t>(t->array.size()); ++i) {
      TValue v = t->get_int(i);
      if (!v.is_nil()) {
        L->settop(0);
        L->push(TValue::integer(i));
        L->push(v);
        return 2;
      }
    }
    // continue into hash from beginning
    start = -1;
  }

  bool seen = key.is_nil() || (key.is_int() && key.as_int() > 0);
  for (auto& n : t->hash) {
    if (!n.used || n.value.is_nil())
      continue;
    if (n.key.is_int() && n.key.as_int() >= 1 &&
        n.key.as_int() <= static_cast<int64_t>(t->array.size()))
      continue; // already covered by array
    if (!seen) {
      if (values_equal(n.key, key))
        seen = true;
      continue;
    }
    L->settop(0);
    L->push(n.key);
    L->push(n.value);
    return 2;
  }
  L->settop(0);
  L->push(TValue::nil());
  return 1;
}

static int pairs_next_wrap(State* L) {
  // same as next but for pairs generator: next(t, k)
  return base_next(L);
}

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

// --- coroutines ---

static int co_create(State* L) {
  if (!L->at(1)->is_function())
    panic("coroutine.create: function expected");
  Thread* th = L->gc.create<Thread>(GcKind::Thread);
  th->status = Thread::Status::Fresh;
  th->stack.resize(64, TValue::nil());
  th->frames.reserve(64);
  th->top = 1;
  // store function at stack[0]
  th->stack[0] = *L->at(1);
  L->settop(0);
  L->push(TValue::obj(ValueTag::Thread, th));
  return 1;
}

static int co_resume(State* L) {
  if (L->at(1)->tag() != ValueTag::Thread)
    panic("coroutine.resume: thread expected");
  Thread* co = L->at(1)->as_thread();
  int nargs = L->gettop() - 1;
  std::vector<TValue> args(static_cast<size_t>(nargs));
  for (int i = 0; i < nargs; ++i)
    args[static_cast<size_t>(i)] = *L->at(2 + i);

  Thread* from = L->current;
  auto return_ok = [&](std::vector<TValue> results) -> int {
    L->current = from;
    L->settop(0);
    L->push(TValue::boolean(true));
    for (auto& r : results)
      L->push(r);
    return 1 + static_cast<int>(results.size());
  };
  auto return_yield = [&]() -> int {
    co->status = Thread::Status::Suspended;
    std::vector<TValue> results = std::move(co->yield_vals);
    co->yield_vals.clear();
    return return_ok(std::move(results));
  };

  try {
    if (co->status == Thread::Status::Dead)
      panic("cannot resume dead coroutine");
    L->current = co;
    int st = LUA_OK;
    if (co->status == Thread::Status::Fresh) {
      if (co->stack.empty())
        panic("coroutine.create: empty stack");
      TValue f = co->stack[0];
      co->stack.clear();
      co->stack.resize(64, TValue::nil());
      co->frames.clear();
      co->frames.reserve(64);
      co->top = 0;
      L->push(f);
      for (auto& a : args)
        L->push(a);
      co->status = Thread::Status::Running;
      if (!f.is_function())
        panic("coroutine main is not a function");
      st = call_closure(L, f.as_closure(), nargs, LUA_MULTRET);
    } else if (co->status == Thread::Status::Suspended) {
      // Install resume args as results of the yielding CALL.
      int dest = co->yield_func_idx;
      int want = co->yield_nresults;
      L->ensure_stack(dest + (want == LUA_MULTRET ? nargs : want) + 8);
      if (want == LUA_MULTRET) {
        for (int i = 0; i < nargs; ++i)
          co->stack[static_cast<size_t>(dest + i)] = args[static_cast<size_t>(i)];
        L->settop(dest + nargs);
      } else {
        for (int i = 0; i < want; ++i)
          co->stack[static_cast<size_t>(dest + i)] =
              (i < nargs) ? args[static_cast<size_t>(i)] : TValue::nil();
        L->settop(dest + want);
      }
      co->status = Thread::Status::Running;
      st = interpret(L);
    } else {
      panic("cannot resume non-suspended coroutine");
    }

    if (st == LUA_YIELD)
      return return_yield();

    int nret = L->gettop();
    std::vector<TValue> results(static_cast<size_t>(nret));
    for (int i = 0; i < nret; ++i)
      results[static_cast<size_t>(i)] = *L->at(i + 1);
    co->status = Thread::Status::Dead;
    return return_ok(std::move(results));
  } catch (const Lj3Error& e) {
    L->current = from;
    L->settop(0);
    L->push(TValue::boolean(false));
    L->push(TValue::obj(ValueTag::String, L->intern(e.what())));
    co->status = Thread::Status::Dead;
    return 2;
  }
}

static int coro_yield(State* L) {
  if (L->current == L->main)
    panic("cannot yield from main thread");
  L->yield_pending = true;
  L->current->status = Thread::Status::Suspended;
  // Leave yield values on the coroutine stack for resume to harvest.
  return L->gettop();
}

static int co_status(State* L) {
  if (L->at(1)->tag() != ValueTag::Thread)
    panic("coroutine.status: thread expected");
  Thread* co = L->at(1)->as_thread();
  const char* s = "dead";
  switch (co->status) {
  case Thread::Status::Fresh:
  case Thread::Status::Suspended: s = "suspended"; break;
  case Thread::Status::Running: s = (co == L->current) ? "running" : "normal"; break;
  case Thread::Status::Dead:
  case Thread::Status::Error: s = "dead"; break;
  }
  L->settop(0);
  L->push(TValue::obj(ValueTag::String, L->intern(s)));
  return 1;
}

static int co_running(State* L) {
  L->settop(0);
  L->push(TValue::obj(ValueTag::Thread, L->current));
  L->push(TValue::boolean(L->current == L->main));
  return 2;
}

static void set_global(State* L, const char* name, CFunction f) {
  L->globals->set(L, TValue::obj(ValueTag::String, L->intern(name)),
                  TValue::obj(ValueTag::Function, closure_new_c(L, f)));
}

static void set_field(State* L, Table* t, const char* name, CFunction f) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)),
         TValue::obj(ValueTag::Function, closure_new_c(L, f)));
}

void open_base(State* L) {
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

  Table* co = table_new(L, 0, 8);
  set_field(L, co, "create", co_create);
  set_field(L, co, "resume", co_resume);
  set_field(L, co, "yield", coro_yield);
  set_field(L, co, "status", co_status);
  set_field(L, co, "running", co_running);
  L->globals->set(L, TValue::obj(ValueTag::String, L->intern("coroutine")),
                  TValue::obj(ValueTag::Table, co));
}

} // namespace lj3
