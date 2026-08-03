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

void StringTable::resize(size_t new_size) {
  if (new_size < 32)
    new_size = 32;
  size_t n = 1;
  while (n < new_size)
    n <<= 1;
  std::vector<LjString*> nh(n, nullptr);
  for (LjString* head : hash) {
    while (head) {
      LjString* next = head->hnext;
      size_t i = head->hash & (n - 1);
      head->hnext = nh[i];
      nh[i] = head;
      head = next;
    }
  }
  hash = std::move(nh);
}

void StringTable::remove(LjString* s) {
  if (!s || hash.empty())
    return;
  size_t i = s->hash & (hash.size() - 1);
  LjString** p = &hash[i];
  while (*p) {
    if (*p == s) {
      *p = s->hnext;
      s->hnext = nullptr;
      if (nuse > 0)
        --nuse;
      return;
    }
    p = &(*p)->hnext;
  }
}

LjString* StringTable::intern(State* L, std::string_view s) {
  uint32_t h = hash_bytes(s.data(), s.size());
  if (hash.empty())
    resize(128);
  size_t mask = hash.size() - 1;
  size_t i = h & mask;
  for (LjString* p = hash[i]; p; p = p->hnext) {
    if (p->hash == h && p->len == s.size() &&
        (s.empty() || std::memcmp(p->data, s.data(), s.size()) == 0))
      return p;
  }
  if (nuse >= hash.size()) {
    resize(hash.size() * 2);
    mask = hash.size() - 1;
    i = h & mask;
  }

  size_t bytes = sizeof(LjString) + s.size();
  auto* mem = static_cast<LjString*>(std::malloc(bytes));
  if (!mem)
    panic("out of memory");
  new (mem) LjString();
  mem->len = s.size();
  mem->hash = h;
  mem->hnext = nullptr;
  std::memcpy(mem->data, s.data(), s.size());
  mem->data[s.size()] = '\0';
  L->gc.link(mem, GcKind::String, bytes);

  mem->hnext = hash[i];
  hash[i] = mem;
  ++nuse;
  return mem;
}

} // namespace luatier
