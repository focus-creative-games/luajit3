#pragma once

#include "vm/state.hpp"

namespace lj3 {

int interpret(State* L);
int call_closure(State* L, Closure* cl, int nargs, int nresults);

} // namespace lj3
