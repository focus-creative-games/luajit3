#include "jit/hotness.hpp"

#include "jit/baseline.hpp"

#include <unordered_map>

namespace lj3 {

namespace {
std::unordered_map<Proto*, uint64_t> entries;
std::unordered_map<Proto*, uint64_t> loops;
} // namespace

void hotness_on_entry(Proto* p) {
  if (!p)
    return;
  auto n = ++entries[p];
  if (n == kBaselineEntryThreshold)
    baseline_request_compile(p);
}

void hotness_on_loop(Proto* p) {
  if (!p)
    return;
  auto n = ++loops[p];
  if (n == kBaselineLoopThreshold)
    baseline_request_compile(p);
}

uint64_t hotness_entry_count(Proto* p) { return entries[p]; }
uint64_t hotness_loop_count(Proto* p) { return loops[p]; }

} // namespace lj3
