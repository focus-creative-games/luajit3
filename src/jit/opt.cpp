#include "jit/opt.hpp"

#include <unordered_map>

namespace lj3 {

namespace {
std::unordered_map<Proto*, MirModule> modules;
}

void opt_request_compile(Proto* p) {
  if (!p)
    return;
  auto& m = modules[p];
  m.proto = p;
  // Placeholder: mark module slot; real SSA build is Phase 3 work.
  m.valid = false;
}

MirModule* opt_lookup(Proto* p) {
  auto it = modules.find(p);
  return it == modules.end() ? nullptr : &it->second;
}

} // namespace lj3
