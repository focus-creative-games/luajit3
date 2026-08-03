#pragma once

#include "vm/state.hpp"

namespace lj3 {

// Run while frames.size() >= min_frames. Default -1 means "current size" (for
// call_closure). Resume must pass 1 so inner RETURNs continue outer frames.
int interpret(State* L, int min_frames = -1);
int call_closure(State* L, Closure* cl, int nargs, int nresults);

} // namespace lj3
