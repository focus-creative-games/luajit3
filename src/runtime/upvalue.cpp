#include "runtime/upvalue.hpp"

#include "vm/state.hpp"

namespace lj3 {

TValue UpVal::get() const {
  if (!open)
    return closed;
  return thread->stack[static_cast<size_t>(stack_index)];
}

void UpVal::set(State* L, const TValue& v) {
  if (open)
    thread->stack[static_cast<size_t>(stack_index)] = v;
  else
    closed = v;
  L->gc.barrier(this, v);
}

void UpVal::close(State* L) {
  if (!open)
    return;
  closed = thread->stack[static_cast<size_t>(stack_index)];
  open = false;
  thread = nullptr;
  stack_index = -1;
  L->gc.barrier(this, closed);
}

} // namespace lj3
