#pragma once

#include "gc/gc.hpp"
#include "runtime/value.hpp"

#include <vector>

namespace luatier {

// PUC TMS: bit `1u << e` in Table::opt_flags means "no such metamethod" (cached).
enum TmEvent : uint8_t {
  TM_INDEX = 0,
  TM_NEWINDEX,
  TM_GC,
  TM_MODE,
  TM_LEN,
  TM_EQ,
  TM_ADD,
  TM_SUB,
  TM_MUL,
  TM_MOD,
  TM_POW,
  TM_DIV,
  TM_IDIV,
  TM_BAND,
  TM_BOR,
  TM_BXOR,
  TM_SHL,
  TM_SHR,
  TM_UNM,
  TM_BNOT,
  TM_LT,
  TM_LE,
  TM_CONCAT,
  TM_CALL,
  TM_N
};

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
  uint32_t opt_flags = 0; // PUC-style fasttm absence bits
  uint8_t weak_mode = 0; // bit0 weak keys, bit1 weak values

  void update_weak_mode(State* L);
  // mask: bit0 clear weak keys, bit1 clear weak values (same as weak_mode bits).
  // Interned strings stay alive via StringTable strong roots (like former short_intern).
  void clear_weak_entries(uint8_t white, uint8_t mask = 3);

  TValue get(const TValue& key) const;
  void set(State* L, const TValue& key, const TValue& value);
  void set_int(State* L, int64_t i, const TValue& value);
  TValue get_int(int64_t i) const;
  void rehash(State* L, size_t array_hint, size_t hash_hint);

  bool no_tm(TmEvent e) const { return (opt_flags & (1u << e)) != 0; }
  void mark_no_tm(TmEvent e) { opt_flags |= (1u << e); }
  void clear_tm_cache() { opt_flags = 0; }
};

Table* table_new(State* L, size_t array_hint = 0, size_t hash_hint = 0);

} // namespace luatier
