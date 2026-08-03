#include "jit/baseline.hpp"

#include "jit/deopt.hpp"

#include <iostream>
#include <unordered_map>

namespace luatier {

namespace {
std::unordered_map<Proto*, JitCode> codes;
}

void baseline_request_compile(Proto* p) {
  if (!p)
    return;
  auto& jc = codes[p];
  if (jc.blacklisted || jc.tier >= Tier::Baseline)
    return;
  // Stub "compilation": mark as baseline-ready with null entry (always deopt).
  jc.tier = Tier::Baseline;
  jc.entry = nullptr;
  if (std::getenv("LUATIER_JIT_LOG"))
    std::cerr << "[luatier] baseline compile stub for proto@" << p << " maxstack=" << p->maxstack
              << "\n";
}

JitCode* baseline_lookup(Proto* p) {
  auto it = codes.find(p);
  return it == codes.end() ? nullptr : &it->second;
}

bool baseline_try_enter(State* L, Proto* p) {
  auto* jc = baseline_lookup(p);
  if (!jc || jc->tier < Tier::Baseline || jc->blacklisted)
    return false;
  // No native code yet — deopt immediately (keeps pipeline wired).
  deopt_to_interpreter(L, p, 0);
  return false;
}

} // namespace luatier
