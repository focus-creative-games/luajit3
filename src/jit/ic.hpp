#pragma once

#include "runtime/value.hpp"

#include <cstdint>

namespace luatier {

enum class IcState : uint8_t { Uninitialized, Monomorphic, Polymorphic, Megamorphic };

struct IcSlot {
  IcState state = IcState::Uninitialized;
  ValueTag seen_tag = ValueTag::Nil;
  uint32_t shape_version = 0;
  uint32_t hits = 0;
  uint32_t misses = 0;
};

IcState ic_observe_type(IcSlot& slot, ValueTag tag);
void ic_invalidate(IcSlot& slot);

} // namespace luatier
