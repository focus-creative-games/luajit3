#pragma once

#include "gc/gc.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace luatier {

struct LjString : GcObject {
  size_t len = 0;
  uint32_t hash = 0;
  LjString* hnext = nullptr; // string-table chain (not GC list)
  char data[1]{};

  std::string_view view() const { return {data, len}; }
};

// PUC-style intern table: open buckets + chaining. Lookup never allocates.
// Entries are not GC roots; dead strings are unlinked in sweep.
struct StringTable {
  std::vector<LjString*> hash;
  size_t nuse = 0;

  LjString* intern(State* L, std::string_view s);
  void remove(LjString* s);
  void resize(size_t new_size);
};

uint32_t hash_bytes(const char* p, size_t n);

} // namespace luatier
