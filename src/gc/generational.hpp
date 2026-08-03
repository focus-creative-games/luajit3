#pragma once

// Phase 5: generational GC extension (disabled by default).
namespace luatier {
namespace gen_gc {

inline constexpr bool kEnabled = false;

void note_old_to_young(void* /*parent*/, void* /*child*/);

} // namespace gen_gc
} // namespace luatier
