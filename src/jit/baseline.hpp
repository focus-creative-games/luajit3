#pragma once

#include "runtime/closure.hpp"

namespace lj3 {

// Phase 2 baseline JIT: records compilation requests and provides a stub
// native entry that immediately deopts to the interpreter until real codegen lands.
enum class Tier : uint8_t { Interpreter = 0, Baseline = 1, Optimizing = 2 };

struct JitCode {
  Tier tier = Tier::Interpreter;
  void* entry = nullptr; // future: native code pointer
  bool blacklisted = false;
};

void baseline_request_compile(Proto* p);
JitCode* baseline_lookup(Proto* p);
bool baseline_try_enter(State* L, Proto* p);

} // namespace lj3
