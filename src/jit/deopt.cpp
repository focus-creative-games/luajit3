#include "jit/deopt.hpp"

#include <iostream>

namespace lj3 {

void deopt_to_interpreter(State* L, Proto* p, int bytecode_pc) {
  (void)L;
  (void)p;
  (void)bytecode_pc;
  if (std::getenv("LJ3_JIT_LOG"))
    std::cerr << "[lj3] deopt to interpreter pc=" << bytecode_pc << "\n";
  // Full frame reconstruction lands with real baseline codegen.
}

} // namespace lj3
