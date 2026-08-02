#pragma once

#include "gc/gc.hpp"
#include "runtime/value.hpp"
#include "vm/bytecode.hpp"

#include <functional>
#include <vector>

namespace lj3 {

struct UpVal;

using CFunction = int (*)(State* L);

struct Closure : GcObject {
  bool is_c = false;
  Proto* proto = nullptr;
  CFunction cfunc = nullptr;
  std::vector<UpVal*> upvals;
};

struct LocVar {
  std::string name;
  int startpc = 0;
  int endpc = 0;
};

struct Proto : GcObject {
  std::vector<Instruction> code;
  std::vector<TValue> constants;
  std::vector<Proto*> protos;
  std::vector<UpvalDesc> upvalues;
  std::vector<int> lineinfo;
  std::vector<LocVar> locvars;
  std::string source;
  int linedefined = 0;
  int lastlinedefined = 0;
  int maxstack = 0;
  int numparams = 0;
  bool is_vararg = false;
};

Closure* closure_new_lua(State* L, Proto* p);
Closure* closure_new_c(State* L, CFunction f);

} // namespace lj3
