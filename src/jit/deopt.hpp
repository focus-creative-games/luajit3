#pragma once

#include "runtime/closure.hpp"
#include "vm/state.hpp"

namespace luatier {

struct StackMapSlot {
  int reg = -1;
  bool is_ref = false;
};

struct DeoptPoint {
  int bytecode_pc = 0;
  std::vector<StackMapSlot> slots;
};

// Reconstruct interpreter state at bytecode_pc (Phase 2+ full maps).
void deopt_to_interpreter(State* L, Proto* p, int bytecode_pc);

} // namespace luatier
