#pragma once

#include <cstdint>

namespace luatier {

struct RuntimeProfile {
  uint64_t opcodes = 0;
  uint64_t calls = 0;
  uint64_t allocations = 0;
  uint64_t gc_steps = 0;
  uint64_t deopts = 0;
};

RuntimeProfile& runtime_profile();
void profile_dump_stderr();

} // namespace luatier
