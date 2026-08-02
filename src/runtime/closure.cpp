#include "runtime/closure.hpp"

#include "vm/state.hpp"

namespace lj3 {

Closure* closure_new_lua(State* L, Proto* p) {
  auto* cl = L->gc.create<Closure>(GcKind::Closure);
  cl->is_c = false;
  cl->proto = p;
  cl->upvals.resize(p->upvalues.size(), nullptr);
  return cl;
}

Closure* closure_new_c(State* L, CFunction f) {
  auto* cl = L->gc.create<Closure>(GcKind::Closure);
  cl->is_c = true;
  cl->cfunc = f;
  return cl;
}

} // namespace lj3
