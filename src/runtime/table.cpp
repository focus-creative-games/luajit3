#include "runtime/table.hpp"

#include "vm/state.hpp"

namespace lj3 {

static size_t hash_key(const TValue& k, size_t mod) {
  if (mod == 0)
    return 0;
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

TValue Table::get(const TValue& key) const {
  if (key.is_int()) {
    int64_t i = key.as_int();
    if (i >= 1 && static_cast<size_t>(i) <= array.size())
      return array[static_cast<size_t>(i - 1)];
  }
  if (hash.empty())
    return TValue::nil();
  size_t idx = hash_key(key, hash.size());
  for (size_t probe = 0; probe < hash.size(); ++probe) {
    size_t i = (idx + probe) % hash.size();
    if (!hash[i].used)
      return TValue::nil();
    if (values_equal(hash[i].key, key))
      return hash[i].value;
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
  if (key.is_int()) {
    int64_t i = key.as_int();
    if (i >= 1 && static_cast<size_t>(i) <= array.size()) {
      array[static_cast<size_t>(i - 1)] = value;
      L->gc.barrier(this, value);
      return;
    }
  }
  if (hash.empty())
    hash.resize(8);

  size_t idx = hash_key(key, hash.size());
  size_t first_free = static_cast<size_t>(-1);
  for (size_t probe = 0; probe < hash.size(); ++probe) {
    size_t i = (idx + probe) % hash.size();
    if (!hash[i].used) {
      if (value.is_nil())
        return;
      first_free = i;
      break;
    }
    if (values_equal(hash[i].key, key)) {
      hash[i].value = value;
      if (value.is_nil())
        hash[i].used = false;
      L->gc.barrier(this, value);
      return;
    }
  }
  if (value.is_nil())
    return;
  if (first_free == static_cast<size_t>(-1)) {
    rehash(L, array.size(), hash.size() * 2);
    set(L, key, value);
    return;
  }
  hash[first_free].used = true;
  hash[first_free].key = key;
  hash[first_free].value = value;
  hash[first_free].next = nullptr;
  structure_version++;
  L->gc.barrier(this, key);
  L->gc.barrier(this, value);
}

} // namespace lj3
