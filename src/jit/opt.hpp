#pragma once

#include "jit/baseline.hpp"
#include "runtime/closure.hpp"

namespace lj3 {

// Phase 3 optimizing JIT scaffolding: SSA MIR passes will attach here.
struct MirModule {
  Proto* proto = nullptr;
  bool valid = false;
};

void opt_request_compile(Proto* p);
MirModule* opt_lookup(Proto* p);

} // namespace lj3
