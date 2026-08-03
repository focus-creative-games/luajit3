#pragma once

#include "vm/state.hpp"

namespace luatier {

// Run while frames.size() >= min_frames. Default -1 means "current size" (for
// call_closure). Resume must pass 1 so inner RETURNs continue outer frames.
int interpret(State* L, int min_frames = -1);
int call_closure(State* L, Closure* cl, int nargs, int nresults);

// After a yield resume (or protected-call error), drain Continue frames and
// run Lua until the next yield or the coroutine body returns.
// Returns LUA_OK or LUA_YIELD.
int resume_after_yield(State* L, bool ok, const char* err_msg);

} // namespace luatier
