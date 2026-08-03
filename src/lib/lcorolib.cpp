#include "lib/libs.hpp"

#include "common/common.hpp"
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

namespace luatier {
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

  auto return_err = [&](const char* msg) -> int {
    L->current = from;
    L->settop(0);
    L->push(TValue::boolean(false));
    push_string(L, msg);
    return 2;
  };

  // PUC: resume of dead / non-suspended returns false+msg and does not
  // change the coroutine status (running stays "running").
  if (co->status == Thread::Status::Dead || co->status == Thread::Status::Error)
    return return_err("cannot resume dead coroutine");
  if (co->status != Thread::Status::Fresh && co->status != Thread::Status::Suspended)
    return return_err("cannot resume non-suspended coroutine");

  // PUC lua_resume: inherit C-call depth from the caller; refuse before native SO.
  {
    unsigned short ncc = static_cast<unsigned short>(from->nCcalls + 1);
    if (ncc >= LUAI_MAXCCALLS)
      return return_err("C stack overflow");
    co->nCcalls = ncc;
  }
  struct ResumeCcalls {
    Thread* co;
    ~ResumeCcalls() {
      if (co && co->nCcalls > 0)
        --co->nCcalls;
    }
  } _rc{co};

  try {
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
    } else {
      // Finish the yielded C call: drop its frame, then write resume values into
      // the caller's result slots (same as a normal C return).
      if (!co->frames.empty() && co->frames.back().kind == FrameKind::CApi &&
          co->frames.back().proto == nullptr)
        co->frames.pop_back();
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
      // Drain Continue frames (yieldable pcall/xpcall) and run Lua until the
      // next yield or the coroutine body returns.
      st = resume_after_yield(L, true, nullptr);
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
  } catch (const LuatierError& e) {
    bool had_continue = false;
    for (auto& fr : co->frames) {
      if (fr.kind == FrameKind::Continue) {
        had_continue = true;
        break;
      }
    }
    if (had_continue) {
      try {
        int st = resume_after_yield(L, false, e.what());
        if (st == LUA_YIELD)
          return return_yield();
        int nret = L->abs_top();
        std::vector<TValue> results(static_cast<size_t>(nret));
        for (int i = 0; i < nret; ++i)
          results[static_cast<size_t>(i)] = co->stack[static_cast<size_t>(i)];
        co->status = Thread::Status::Dead;
        return return_ok(std::move(results));
      } catch (const LuatierError& e2) {
        L->current = from;
        TValue emsg = co->err_obj_set ? co->err_obj
                                       : TValue::obj(ValueTag::String, L->intern(e2.what()));
        co->err_obj_set = false;
        L->settop(0);
        L->push(TValue::boolean(false));
        L->push(emsg);
        co->status = Thread::Status::Dead;
        return 2;
      }
    }
    L->current = from;
    TValue emsg = co->err_obj_set ? co->err_obj
                                   : TValue::obj(ValueTag::String, L->intern(e.what()));
    co->err_obj_set = false;
    L->settop(0);
    L->push(TValue::boolean(false));
    L->push(emsg);
    co->status = Thread::Status::Dead;
    return 2;
  }
}

static int coro_yield(State* L) {
  if (L->current == L->main)
    panic("attempt to yield from outside a coroutine");
  // nny includes this yield C frame (always +1); >1 means an outer non-yieldable C.
  if (L->current->nny > 1)
    panic("attempt to yield across a C-call boundary");
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
  // nny includes this C frame (+1). Yieldable when no outer non-yieldable C.
  L->settop(0);
  L->push(TValue::boolean(L->current != L->main && L->current->nny <= 1));
  return 1;
}

static int wrap_body(State* L) {
  Closure* self = L->current->frames.back().cl;
  if (self->upvals.empty() || !self->upvals[0])
    panic("wrap: bad coroutine");
  TValue thv = self->upvals[0]->get();
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
    // Propagate the original error object (PUC luaB_auxwrap).
    TValue err = L->gettop() >= 2 ? *L->at(2) : TValue::obj(ValueTag::String, L->intern("error"));
    L->current->err_obj = err;
    L->current->err_obj_set = true;
    panic(value_to_string(err));
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
  // Thread lives in an upvalue so wrap can be collected (gc.lua / coroutine.lua).
  auto* uv = L->gc.create<UpVal>(GcKind::UpVal);
  uv->open = false;
  uv->closed = TValue::obj(ValueTag::Thread, th);
  uv->thread = nullptr;
  uv->stack_index = -1;
  cl->upvals.push_back(uv);
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

} // namespace luatier
