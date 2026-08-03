#pragma once

#include "gc/gc.hpp"
#include "runtime/value.hpp"

#include <vector>

namespace luatier {

struct State;
struct Table;

struct Userdata : GcObject {
  Table* metatable = nullptr;
  std::vector<char> data;
};

Userdata* userdata_new(State* L, size_t size, Table* mt = nullptr);
void* userdata_data(Userdata* u);
const void* userdata_data(const Userdata* u);

} // namespace luatier
