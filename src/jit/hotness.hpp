#pragma once

#include "runtime/closure.hpp"

namespace lj3 {

void hotness_on_entry(Proto* p);
void hotness_on_loop(Proto* p);
uint64_t hotness_entry_count(Proto* p);
uint64_t hotness_loop_count(Proto* p);

// Phase 2: thresholds for baseline compilation
inline constexpr uint64_t kBaselineEntryThreshold = 100;
inline constexpr uint64_t kBaselineLoopThreshold = 50;

} // namespace lj3
