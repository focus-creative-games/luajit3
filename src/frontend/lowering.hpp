#pragma once

#include "frontend/ast.hpp"
#include "runtime/closure.hpp"
#include "vm/state.hpp"

namespace lj3 {

Proto* lower_chunk(State* L, Chunk& chunk, const std::string& source_name);

} // namespace lj3
