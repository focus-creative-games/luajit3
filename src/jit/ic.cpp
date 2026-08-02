#include "jit/ic.hpp"

namespace lj3 {

IcState ic_observe_type(IcSlot& slot, ValueTag tag) {
  slot.hits++;
  switch (slot.state) {
  case IcState::Uninitialized:
    slot.seen_tag = tag;
    slot.state = IcState::Monomorphic;
    break;
  case IcState::Monomorphic:
    if (slot.seen_tag != tag) {
      slot.state = IcState::Polymorphic;
      slot.misses++;
    }
    break;
  case IcState::Polymorphic:
    slot.misses++;
    if (slot.misses > 32)
      slot.state = IcState::Megamorphic;
    break;
  case IcState::Megamorphic:
    break;
  }
  return slot.state;
}

void ic_invalidate(IcSlot& slot) {
  slot = {};
}

} // namespace lj3
