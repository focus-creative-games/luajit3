#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "vm/interpreter.hpp"

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
using namespace lib;

static int co_create(State* L) {
  if (!L->at(1)->is_function())
    panic("coroutine.create: function expected");
  Thread* th = L->gc.create<Thread>(GcKind::Thread);
  th->status = Thread::Status::Fresh;
  th->stack.resize(64, TValue::nil());
  th->frames.reserve(64);
  th->top = 1;
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
      int dest = co->yield_func_idx;
      int want = co->yield_nresults;
      int nplace = (want == LUA_MULTRET) ? nargs : want;
      L->ensure_stack(dest + nplace + 8);
      if (want == LUA_MULTRET) {
        for (int i = 0; i < nargs; ++i)
          co->stack[static_cast<size_t>(dest + i)] = args[static_cast<size_t>(i)];
      } else {
        for (int i = 0; i < want; ++i)
          co->stack[static_cast<size_t>(dest + i)] =
              (i < nargs) ? args[static_cast<size_t>(i)] : TValue::nil();
      }
      // Do not use set_abs_top here — growing top nil-fills and would wipe the values.
      co->top = dest + nplace;
      co->status = Thread::Status::Running;
      st = interpret(L);
    } else {
      panic("cannot resume non-suspended coroutine");
    }

    if (st == LUA_YIELD)
      return return_yield();

    // Main chunk results sit at absolute slots [0, top).
    int nret = L->abs_top();
    std::vector<TValue> results(static_cast<size_t>(nret));
    for (int i = 0; i < nret; ++i)
      results[static_cast<size_t>(i)] = co->stack[static_cast<size_t>(i)];
    co->status = Thread::Status::Dead;
    return return_ok(std::move(results));
  } catch (const Lj3Error& e) {
    L->current = from;
    L->settop(0);
    L->push(TValue::boolean(false));
    push_string(L, e.what());
    co->status = Thread::Status::Dead;
    return 2;
  }
}

static int coro_yield(State* L) {
  if (L->current == L->main)
    panic("cannot yield from main thread");
  L->yield_pending = true;
  L->current->status = Thread::Status::Suspended;
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
  push_string(L, s);
  return 1;
}

static int co_running(State* L) {
  L->settop(0);
  L->push(TValue::obj(ValueTag::Thread, L->current));
  L->push(TValue::boolean(L->current == L->main));
  return 2;
}

static int co_isyieldable(State* L) {
  L->settop(0);
  L->push(TValue::boolean(L->current != L->main));
  return 1;
}

static int wrap_body(State* L) {
  Closure* self = L->current->frames.back().cl;
  TValue thv = L->registry->get(TValue::obj(ValueTag::Function, self));
  if (!thv.is_thread())
    panic("wrap: bad coroutine");
  int nargs = L->gettop();
  std::vector<TValue> args(static_cast<size_t>(nargs));
  for (int i = 0; i < nargs; ++i)
    args[static_cast<size_t>(i)] = *L->at(i + 1);
  L->settop(0);
  L->push(thv);
  for (auto& a : args)
    L->push(a);
  co_resume(L);
  if (L->gettop() >= 1 && !L->at(1)->is_truthy()) {
    std::string err = value_to_string(*L->at(2));
    panic(err);
  }
  int nret = L->gettop() - 1;
  std::vector<TValue> outs(static_cast<size_t>(nret));
  for (int i = 0; i < nret; ++i)
    outs[static_cast<size_t>(i)] = *L->at(i + 2);
  L->settop(0);
  for (int i = 0; i < nret; ++i)
    L->push(outs[static_cast<size_t>(i)]);
  return nret;
}

static int co_wrap(State* L) {
  if (!L->at(1)->is_function())
    panic("coroutine.wrap: function expected");
  co_create(L);
  Thread* th = L->at(1)->as_thread();
  Closure* cl = closure_new_c(L, wrap_body);
  L->registry->set(L, TValue::obj(ValueTag::Function, cl), TValue::obj(ValueTag::Thread, th));
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, cl));
  return 1;
}

void open_coroutine_lib(State* L) {
  Table* co = new_lib(L, 8);
  set_field(L, co, "create", co_create);
  set_field(L, co, "resume", co_resume);
  set_field(L, co, "yield", coro_yield);
  set_field(L, co, "status", co_status);
  set_field(L, co, "running", co_running);
  set_field(L, co, "isyieldable", co_isyieldable);
  set_field(L, co, "wrap", co_wrap);
  set_global_value(L, "coroutine", TValue::obj(ValueTag::Table, co));
}

} // namespace lj3
