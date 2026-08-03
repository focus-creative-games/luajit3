#include "runtime/table.hpp"

#include "runtime/string.hpp"
#include "vm/state.hpp"

#include <cmath>
#include <cstdint>

namespace luatier {

static bool is_gc_key(const TValue& k) {
  switch (k.tag()) {
  case ValueTag::String:
  case ValueTag::Table:
  case ValueTag::Function:
  case ValueTag::Userdata:
  case ValueTag::Thread:
    return true;
  default:
    return false;
  }
}

void Table::update_weak_mode(State* L) {
  weak_mode = 0;
  if (!metatable)
    return;
  TValue mode = metatable->get(TValue::obj(ValueTag::String, L->intern("__mode")));
  if (!mode.is_string())
    return;
  std::string_view s = mode.as_string()->view();
  if (s.find('k') != std::string_view::npos)
    weak_mode |= 1;
  if (s.find('v') != std::string_view::npos)
    weak_mode |= 2;
}

void Table::clear_weak_entries(uint8_t white, uint8_t mask) {
  uint8_t mode = static_cast<uint8_t>(weak_mode & mask);
  if (!mode)
    return;
  auto dead = [&](const TValue& v) {
    if (!is_gc_key(v))
      return false;
    GcObject* o = v.as_gc();
    // Objects already separated for finalization keep their mark as the
    // "other" color; treat them as alive for weak-key retention.
    return o && o->mark == white;
  };
  if (mode & 2) {
    for (auto& v : array) {
      if (dead(v))
        v = TValue::nil();
    }
  }
  for (auto& n : hash) {
    if (!n.used)
      continue;
    if ((mode & 1) && dead(n.key)) {
      // Tombstone: keep `used` so open-addressing probe chains stay intact.
      // (Hard-clearing used broke s[obj] lookups in gc.lua after a prior
      // cycle left finalized keys in a weak-key table.)
      n.key = TValue::nil();
      n.value = TValue::nil();
      continue;
    }
    if ((mode & 2) && dead(n.value))
      n.value = TValue::nil();
  }
}

static size_t hash_key(const TValue& k, size_t mod) {
  if (mod == 0)
    return 0;
  // Strings hash by cached content hash (all strings are pointer-interned).
  if (k.is_string()) {
    LjString* s = k.as_string();
    return static_cast<size_t>(s->hash % mod);
  }
  uint64_t h = k.payload ^ (static_cast<uint64_t>(k.type) << 32);
  return static_cast<size_t>(h % mod);
}

Table* table_new(State* L, size_t array_hint, size_t hash_hint) {
  auto* t = L->gc.create<Table>(GcKind::Table);
  if (array_hint)
    t->array.resize(array_hint, TValue::nil());
  size_t n = 8;
  if (hash_hint) {
    n = 1;
    while (n < hash_hint)
      n <<= 1;
  }
  t->hash.resize(n);
  return t;
}

TValue Table::get_int(int64_t i) const {
  if (i >= 1 && static_cast<size_t>(i) <= array.size())
    return array[static_cast<size_t>(i - 1)];
  return get(TValue::integer(i));
}

// PUC: integer-valued floats (incl. negatives / 0) are stored as integers.
static bool key_as_integer(const TValue& key, int64_t* out) {
  if (key.is_int()) {
    *out = key.as_int();
    return true;
  }
  if (key.is_float()) {
    double d = key.as_float();
    if (d != d) // NaN
      return false;
    if (d >= static_cast<double>(INT64_MIN) && d <= static_cast<double>(INT64_MAX) &&
        d == std::floor(d)) {
      *out = static_cast<int64_t>(d);
      // Reject floats that cannot round-trip (e.g. > 2^53).
      return static_cast<double>(*out) == d;
    }
  }
  return false;
}

TValue Table::get(const TValue& key) const {
  TValue lookup = key;
  int64_t i = 0;
  if (key_as_integer(key, &i)) {
    lookup = TValue::integer(i);
    if (i >= 1 && static_cast<size_t>(i) <= array.size())
      return array[static_cast<size_t>(i - 1)];
  }
  if (hash.empty())
    return TValue::nil();
  size_t idx = hash_key(lookup, hash.size());
  for (size_t probe = 0; probe < hash.size(); ++probe) {
    size_t slot = (idx + probe) % hash.size();
    if (!hash[slot].used)
      return TValue::nil();
    if (values_equal(hash[slot].key, lookup))
      return hash[slot].value; // may be nil (tombstone / deleted)
  }
  return TValue::nil();
}

void Table::rehash(State* L, size_t array_hint, size_t hash_hint) {
  std::vector<std::pair<TValue, TValue>> items;
  for (size_t i = 0; i < array.size(); ++i) {
    if (!array[i].is_nil())
      items.emplace_back(TValue::integer(static_cast<int64_t>(i + 1)), array[i]);
  }
  for (auto& n : hash) {
    if (n.used && !n.value.is_nil())
      items.emplace_back(n.key, n.value);
  }
  array.assign(array_hint, TValue::nil());
  size_t hs = 8;
  while (hs < std::max<size_t>(hash_hint, items.size() + 1))
    hs <<= 1;
  hash.clear();
  hash.resize(hs);
  structure_version++;
  for (auto& kv : items)
    set(L, kv.first, kv.second);
}

void Table::set_int(State* L, int64_t i, const TValue& value) {
  if (i >= 1 && static_cast<size_t>(i) <= array.size()) {
    array[static_cast<size_t>(i - 1)] = value;
    L->gc.barrier(this, value);
    return;
  }
  if (i >= 1 && i <= 64) {
    if (array.size() < static_cast<size_t>(i))
      array.resize(static_cast<size_t>(i), TValue::nil());
    array[static_cast<size_t>(i - 1)] = value;
    structure_version++;
    L->gc.barrier(this, value);
    return;
  }
  set(L, TValue::integer(i), value);
}

void Table::set(State* L, const TValue& key, const TValue& value) {
  if (key.is_nil())
    panic("table index is nil");
  if (key.is_float() && key.as_float() != key.as_float())
    panic("table index is NaN");
  TValue lookup = key;
  int64_t i = 0;
  if (key_as_integer(key, &i)) {
    // Prefer the array part for positive integer keys (incl. integer-valued
    // floats from numeric for), growing it via set_int when needed.
    lookup = TValue::integer(i);
    if (i >= 1 && i <= 64) {
      set_int(L, i, value);
      return;
    }
    if (i >= 1 && static_cast<size_t>(i) <= array.size()) {
      array[static_cast<size_t>(i - 1)] = value;
      L->gc.barrier(this, value);
      return;
    }
  }
  if (hash.empty())
    hash.resize(8);

  size_t idx = hash_key(lookup, hash.size());
  size_t first_tomb = static_cast<size_t>(-1);
  for (size_t probe = 0; probe < hash.size(); ++probe) {
    size_t slot = (idx + probe) % hash.size();
    if (!hash[slot].used) {
      if (value.is_nil())
        return;
      size_t dest = (first_tomb != static_cast<size_t>(-1)) ? first_tomb : slot;
      hash[dest].used = true;
      hash[dest].key = lookup;
      hash[dest].value = value;
      hash[dest].next = nullptr;
      structure_version++;
      L->gc.barrier(this, lookup);
      L->gc.barrier(this, value);
      return;
    }
    if (values_equal(hash[slot].key, lookup)) {
      // Keep the slot occupied when assigning nil so open-addressing probe
      // chains remain intact (tombstone). next()/get ignore nil values.
      hash[slot].value = value;
      if (!value.is_nil())
        L->gc.barrier(this, value);
      return;
    }
    // Reuse deleted (nil-valued) slots for new keys after the probe finishes.
    if (first_tomb == static_cast<size_t>(-1) && hash[slot].value.is_nil())
      first_tomb = slot;
  }
  if (value.is_nil())
    return;
  if (first_tomb != static_cast<size_t>(-1)) {
    hash[first_tomb].used = true;
    hash[first_tomb].key = lookup;
    hash[first_tomb].value = value;
    hash[first_tomb].next = nullptr;
    structure_version++;
    L->gc.barrier(this, lookup);
    L->gc.barrier(this, value);
    return;
  }
  rehash(L, array.size(), hash.size() * 2);
  set(L, key, value);
}

} // namespace luatier
