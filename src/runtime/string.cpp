#include "runtime/string.hpp"

#include "vm/state.hpp"

#include <cstring>
#include <new>

namespace luatier {

uint32_t hash_bytes(const char* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<uint8_t>(p[i]);
    h *= 16777619u;
  }
  return h;
}

LjString* StringTable::intern(State* L, std::string_view s) {
  constexpr size_t kShort = 40;
  std::string key(s);
  if (s.size() <= kShort) {
    auto it = short_intern.find(key);
    if (it != short_intern.end())
      return it->second;
  }
  size_t bytes = sizeof(LjString) + s.size();
  auto* mem = static_cast<LjString*>(std::malloc(bytes));
  if (!mem)
    panic("out of memory");
  new (mem) LjString();
  mem->len = s.size();
  mem->hash = hash_bytes(s.data(), s.size());
  std::memcpy(mem->data, s.data(), s.size());
  mem->data[s.size()] = '\0';
  L->gc.link(mem, GcKind::String, bytes);
  if (s.size() <= kShort)
    short_intern.emplace(std::move(key), mem);
  return mem;
}

} // namespace luatier
