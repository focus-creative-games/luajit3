#pragma once

#include "gc/gc.hpp"
#include "runtime/value.hpp"

namespace luatier {

struct Thread;

// Open upvalues store a stack index (not a raw pointer) so stack reallocation is safe.
struct UpVal : GcObject {
  Thread* thread = nullptr;
  int stack_index = -1;
  TValue closed = TValue::nil();
  bool open = true;
  UpVal* next_open = nullptr;

  TValue get() const;
  void set(State* L, const TValue& v);
  void close(State* L);
};

} // namespace luatier
