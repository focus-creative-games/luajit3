#pragma once

#include "gc/gc.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace lj3 {

struct LjString : GcObject {
  size_t len = 0;
  uint32_t hash = 0;
  char data[1]{};

  std::string_view view() const { return {data, len}; }
};

struct StringTable {
  std::unordered_map<std::string, LjString*> short_intern;

  LjString* intern(State* L, std::string_view s);
};

uint32_t hash_bytes(const char* p, size_t n);

} // namespace lj3
