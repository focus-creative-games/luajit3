#include "jit/deopt.hpp"

#include <iostream>

namespace luatier {

void deopt_to_interpreter(State* L, Proto* p, int bytecode_pc) {
  (void)L;
  (void)p;
  (void)bytecode_pc;
  if (std::getenv("LUATIER_JIT_LOG"))
    std::cerr << "[luatier] deopt to interpreter pc=" << bytecode_pc << "\n";
  // Full frame reconstruction lands with real baseline codegen.
}

} // namespace luatier
