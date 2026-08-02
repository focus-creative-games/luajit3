#pragma once

#include "gc/gc.hpp"
#include "runtime/value.hpp"

#include <vector>

namespace lj3 {

struct TableNode {
  TValue key;
  TValue value;
  TableNode* next = nullptr;
  bool used = false;
};

struct Table : GcObject {
  std::vector<TValue> array;
  std::vector<TableNode> hash;
  Table* metatable = nullptr;
  uint32_t structure_version = 1;
  uint32_t opt_flags = 0;
  uint8_t weak_mode = 0; // bit0 weak keys, bit1 weak values

  void update_weak_mode(State* L);
  // mask: bit0 clear weak keys, bit1 clear weak values (same as weak_mode bits).
  void clear_weak_entries(uint8_t white, uint8_t mask = 3);

  TValue get(const TValue& key) const;
  void set(State* L, const TValue& key, const TValue& value);
  void set_int(State* L, int64_t i, const TValue& value);
  TValue get_int(int64_t i) const;
  void rehash(State* L, size_t array_hint, size_t hash_hint);
};

Table* table_new(State* L, size_t array_hint = 0, size_t hash_hint = 0);

} // namespace lj3
