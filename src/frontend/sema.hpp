#pragma once

#include "frontend/ast.hpp"

namespace lj3 {

// Lightweight semantic pass: validate break/goto nesting basics.
// Full upvalue capture is resolved during lowering with a scope stack.
void sema_analyze(Chunk& chunk);

} // namespace lj3
