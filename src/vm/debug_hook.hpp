#pragma once

#include "vm/state.hpp"

namespace luatier {

enum DebugHookMask : int {
  DEBUG_HOOK_CALL = 1,
  DEBUG_HOOK_RET = 2,
  DEBUG_HOOK_LINE = 4,
  DEBUG_HOOK_COUNT = 8,
};

void debug_sethook_thread(Thread* th, Closure* f, int mask, int count);
void debug_call_hook(State* L, Thread* th, const char* event, int line);
void debug_trace_exec(State* L, Thread* th, size_t frame_idx, Proto* p, int pc);
// After a frame is pushed: fire call hook (if any).
void debug_on_call(State* L, Thread* th);
// After a frame is pushed via OP_TAILCALL: fire "tail call" (uses CALL mask).
void debug_on_tailcall(State* L, Thread* th);
// Before popping a frame: fire return hook (if any).
void debug_return_hook(State* L, Thread* th);
// After a frame is popped: sync oldpc to the caller.
void debug_on_return(State* L, Thread* th);

} // namespace luatier
