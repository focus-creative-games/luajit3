#include "tools/profile.hpp"

#include <iostream>

namespace luatier {

RuntimeProfile& runtime_profile() {
  static RuntimeProfile p;
  return p;
}

void profile_dump_stderr() {
  auto& p = runtime_profile();
  std::cerr << "[luatier profile] opcodes=" << p.opcodes << " calls=" << p.calls
            << " allocs=" << p.allocations << " gc_steps=" << p.gc_steps
            << " deopts=" << p.deopts << "\n";
}

} // namespace luatier
