#include "tools/profile.hpp"

#include <iostream>

namespace lj3 {

RuntimeProfile& runtime_profile() {
  static RuntimeProfile p;
  return p;
}

void profile_dump_stderr() {
  auto& p = runtime_profile();
  std::cerr << "[lj3 profile] opcodes=" << p.opcodes << " calls=" << p.calls
            << " allocs=" << p.allocations << " gc_steps=" << p.gc_steps
            << " deopts=" << p.deopts << "\n";
}

} // namespace lj3
