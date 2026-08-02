#include "runtime/userdata.hpp"

#include "runtime/table.hpp"
#include "vm/state.hpp"

namespace lj3 {

Userdata* userdata_new(State* L, size_t size, Table* mt) {
  auto* u = L->gc.create<Userdata>(GcKind::Userdata);
  u->data.resize(size);
  u->metatable = mt;
  if (mt)
    L->gc.barrier(u, TValue::obj(ValueTag::Table, mt));
  return u;
}

void* userdata_data(Userdata* u) { return u->data.data(); }

const void* userdata_data(const Userdata* u) { return u->data.data(); }

} // namespace lj3
