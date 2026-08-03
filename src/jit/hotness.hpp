#pragma once

#include "runtime/closure.hpp"

namespace luatier {

// Phase 2: thresholds for baseline compilation
inline constexpr uint64_t kBaselineEntryThreshold = 100;
inline constexpr uint64_t kBaselineLoopThreshold = 50;

// Hotness feeds baseline JIT. Until native code exists, counting (unordered_map
// per entry/loop) is pure overhead — keep off unless LUATIER_ENABLE_HOTNESS.
#if defined(LUATIER_ENABLE_HOTNESS)
void hotness_on_entry(Proto* p);
void hotness_on_loop(Proto* p);
uint64_t hotness_entry_count(Proto* p);
uint64_t hotness_loop_count(Proto* p);
#else
inline void hotness_on_entry(Proto*) {}
inline void hotness_on_loop(Proto*) {}
inline uint64_t hotness_entry_count(Proto*) { return 0; }
inline uint64_t hotness_loop_count(Proto*) { return 0; }
#endif

} // namespace luatier
