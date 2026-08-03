#include "vm/debug_hook.hpp"

#include "vm/interpreter.hpp"

#include <algorithm>

namespace luatier {

void debug_sethook_thread(Thread* th, Closure* f, int mask, int count) {
  th->hook.func = f;
  th->hook.mask = mask;
  th->hook.count = count;
  th->hook.hookcount = count;
  // Keep the caller's current PC as oldpc so the line that invoked sethook
  // (often sharing a line with load(...)) does not immediately re-fire.
  if (f && !th->frames.empty())
    th->hook.oldpc = th->frames.back().saved_pc;
  else
    th->hook.oldpc = 0;
}

void debug_call_hook(State* L, Thread* th, const char* event, int line) {
  auto& hook = th->hook;
  if (!hook.func || hook.allowhook <= 0)
    return;

  // Place the hook call above all live Lua register windows. `th->top` alone is
  // not safe: after a CALL it may sit inside the caller's maxstack range, and
  // push_lua_frame would nil-fill / clobber live temporaries (e.g. CONCAT ops).
  const int saved_top = th->top;
  int hook_base = saved_top;
  for (auto& fr : th->frames) {
    if (fr.proto)
      hook_base = std::max(hook_base, fr.base + fr.proto->maxstack);
    else if (fr.cl && fr.cl->proto)
      hook_base = std::max(hook_base, fr.base + fr.cl->proto->maxstack);
    else if (fr.cl && fr.cl->is_c)
      hook_base = std::max(hook_base, fr.base + 1);
  }
  L->ensure_stack(hook_base + 3);
  th->stack[static_cast<size_t>(hook_base)] =
      TValue::obj(ValueTag::Function, hook.func);
  th->stack[static_cast<size_t>(hook_base + 1)] =
      TValue::obj(ValueTag::String, L->intern(event));
  th->stack[static_cast<size_t>(hook_base + 2)] =
      line >= 0 ? TValue::integer(line) : TValue::nil();
  th->top = hook_base + 3;

  // Index, not pointer: call_closure may reallocate frames.
  const int interrupted_idx =
      th->frames.empty() ? -1 : static_cast<int>(th->frames.size()) - 1;
  if (interrupted_idx >= 0)
    th->frames[static_cast<size_t>(interrupted_idx)].hooked = true;

  hook.allowhook--;
  try {
    call_closure(L, hook.func, 2, 0);
  } catch (...) {
    if (interrupted_idx >= 0 &&
        interrupted_idx < static_cast<int>(th->frames.size()))
      th->frames[static_cast<size_t>(interrupted_idx)].hooked = false;
    hook.allowhook++;
    th->top = saved_top;
    throw;
  }
  if (interrupted_idx >= 0 && interrupted_idx < static_cast<int>(th->frames.size()))
    th->frames[static_cast<size_t>(interrupted_idx)].hooked = false;
  hook.allowhook++;
  th->top = saved_top;
}

void debug_trace_exec(State* L, Thread* th, size_t /*frame_idx*/, Proto* p, int npc) {
  auto& hook = th->hook;
  if (!hook.func || hook.allowhook <= 0)
    return;

  if (hook.mask & DEBUG_HOOK_COUNT) {
    if (hook.count > 0 && --hook.hookcount == 0) {
      hook.hookcount = hook.count;
      debug_call_hook(L, th, "count", -1);
      if (hook.allowhook <= 0)
        return;
    }
  }

  if (!(hook.mask & DEBUG_HOOK_LINE))
    return;

  if (npc < 0 || npc >= static_cast<int>(p->lineinfo.size()))
    return;

  const int saved_pc = npc + 1;
  const int newline = p->lineinfo[static_cast<size_t>(npc)];
  int oldline = -1;
  if (hook.oldpc > 0) {
    const int oldnpc = hook.oldpc - 1;
    if (oldnpc >= 0 && oldnpc < static_cast<int>(p->lineinfo.size()))
      oldline = p->lineinfo[static_cast<size_t>(oldnpc)];
  }

  if (npc == 0 || saved_pc <= hook.oldpc || newline != oldline)
    debug_call_hook(L, th, "line", newline);

  hook.oldpc = saved_pc;
}

void debug_on_call(State* L, Thread* th) {
  if (th->hook.func && (th->hook.mask & DEBUG_HOOK_CALL) && th->hook.allowhook > 0)
    debug_call_hook(L, th, "call", -1);
}

void debug_on_tailcall(State* L, Thread* th) {
  if (th->hook.func && (th->hook.mask & DEBUG_HOOK_CALL) && th->hook.allowhook > 0)
    debug_call_hook(L, th, "tail call", -1);
}

// Call while the returning frame is still on the stack (PUC luaD_poscall).
void debug_return_hook(State* L, Thread* th) {
  if (th->hook.func && (th->hook.mask & DEBUG_HOOK_RET) && th->hook.allowhook > 0)
    debug_call_hook(L, th, "return", -1);
}

void debug_on_return(State* L, Thread* th) {
  (void)L;
  // After pop: sync oldpc to the caller's saved_pc.
  if (!th->frames.empty() && th->frames.back().proto)
    th->hook.oldpc = th->frames.back().saved_pc;
  else
    th->hook.oldpc = 0;
}

} // namespace luatier
