#pragma once

#include "vm/state.hpp"

namespace luatier {

void open_libs(State* L);
void open_base(State* L);

} // namespace luatier
