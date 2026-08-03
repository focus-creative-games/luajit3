#include "vm/state.hpp"

#include "frontend/lowering.hpp"
#include "frontend/parser.hpp"
#include "frontend/sema.hpp"
#include "lib/libs.hpp"
#include "vm/interpreter.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef LUA_OK
#define LUA_OK 0
#endif
#ifndef LUA_ERRSYNTAX
#define LUA_ERRSYNTAX 3
#endif
#ifndef LUA_ERRRUN
#define LUA_ERRRUN 2
#endif

namespace luatier {

#ifndef LUA_MULTRET
#define LUA_MULTRET (-1)
#endif

State::State() : gc(this) {
  main = gc.create<Thread>(GcKind::Thread);
  current = main;
  main->status = Thread::Status::Running;
  main->stack.resize(256, TValue::nil());
  main->frames.reserve(64);
  globals = table_new(this, 0, 32);
  registry = table_new(this, 0, 8);
  // _G
  globals->set(this, TValue::obj(ValueTag::String, intern("_G")),
               TValue::obj(ValueTag::Table, globals));
  open_libs(this);
  const char* deopt = std::getenv("LUATIER_STRESS_FORCE_DEOPT");
  force_deopt = deopt && deopt[0] == '1';
  const char* ic = std::getenv("LUATIER_STRESS_DISABLE_IC");
  disable_ic = ic && ic[0] == '1';
}

State::~State() = default;

std::unique_ptr<State> new_state() { return std::make_unique<State>(); }

LjString* State::intern(std::string_view s) { return strings.intern(this, s); }

void State::ensure_stack(int n) {
  if (static_cast<int>(current->stack.size()) < n)
    current->stack.resize(static_cast<size_t>(n) + 64, TValue::nil());
}

int State::gettop() const { return current->top - current->stack_base; }

void State::set_abs_top(int abs_idx) {
  if (abs_idx < 0)
    abs_idx = 0;
  ensure_stack(abs_idx);
  // Growing top must not wipe slots: callers may have already written return
  // values / temps above a temporarily low top (after a fixed-result CALL).
  if (abs_idx < current->top) {
    for (int i = abs_idx; i < current->top; ++i)
      current->stack[static_cast<size_t>(i)] = TValue::nil();
  }
  current->top = abs_idx;
}

void State::settop(int idx) {
  if (idx < 0)
    idx = gettop() + idx + 1;
  if (idx < 0)
    idx = 0;
  set_abs_top(current->stack_base + idx);
}

void State::push(const TValue& v) {
  ensure_stack(current->top + 1);
  current->stack[static_cast<size_t>(current->top++)] = v;
}

TValue State::pop() {
  if (current->top <= current->stack_base)
    return TValue::nil();
  return current->stack[static_cast<size_t>(--current->top)];
}

TValue& State::top_ref() {
  ensure_stack(current->stack_base + 1);
  if (current->top <= current->stack_base)
    panic("top_ref on empty stack");
  return current->stack[static_cast<size_t>(current->top - 1)];
}

int State::absindex(int idx) const {
  if (idx > 0)
    return idx;
  if (idx == 0)
    return 0;
  return gettop() + idx + 1;
}

TValue* State::at(int idx) {
  idx = absindex(idx);
  if (idx <= 0 || idx > gettop())
    panic("invalid stack index");
  return &current->stack[static_cast<size_t>(current->stack_base + idx - 1)];
}

void State::close_upvals(Thread* th, int level) {
  UpVal** p = &th->open_upvals;
  while (*p) {
    UpVal* uv = *p;
    if (uv->open && uv->thread == th && uv->stack_index >= level) {
      *p = uv->next_open;
      uv->close(this);
    } else {
      p = &uv->next_open;
    }
  }
}

UpVal* State::find_upval(Thread* th, int level) {
  for (UpVal* uv = th->open_upvals; uv; uv = uv->next_open) {
    if (uv->open && uv->thread == th && uv->stack_index == level)
      return uv;
  }
  auto* uv = gc.create<UpVal>(GcKind::UpVal);
  uv->open = true;
  uv->thread = th;
  uv->stack_index = level;
  uv->next_open = th->open_upvals;
  th->open_upvals = uv;
  return uv;
}

int State::load_string(const std::string& source, const std::string& chunk_name) {
  try {
    auto chunk = parse(source, chunk_name);
    sema_analyze(*chunk);
    Proto* p = lower_chunk(this, *chunk, chunk_name);
    Closure* cl = closure_new_lua(this, p);
    // bind _ENV upvalue to globals
    if (!p->upvalues.empty()) {
      auto* uv = gc.create<UpVal>(GcKind::UpVal);
      uv->open = false;
      uv->closed = TValue::obj(ValueTag::Table, globals);
      uv->thread = nullptr;
      uv->stack_index = -1;
      cl->upvals[0] = uv;
    }
    push(TValue::obj(ValueTag::Function, cl));
    return LUA_OK;
  } catch (const LuatierError& e) {
    push(TValue::obj(ValueTag::String, intern(e.what())));
    return LUA_ERRSYNTAX;
  }
}

int State::pcall(int nargs, int nresults) {
  int func_idx = gettop() - nargs; // 1-based index of function roughly
  (void)func_idx;
  try {
    return resume_call(at(gettop() - nargs)->as_closure(), nargs, nresults);
  } catch (const LuatierError& e) {
    settop(gettop() - nargs - 1);
    push(TValue::obj(ValueTag::String, intern(e.what())));
    return LUA_ERRRUN;
  }
}

int State::resume_call(Closure* cl, int nargs, int nresults) {
  return call_closure(this, cl, nargs, nresults);
}

} // namespace luatier
