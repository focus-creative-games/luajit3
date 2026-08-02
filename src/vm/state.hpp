#pragma once

#include "gc/gc.hpp"
#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/upvalue.hpp"
#include "runtime/value.hpp"

#include <memory>
#include <string>
#include <vector>

namespace lj3 {

enum class FrameKind : uint8_t {
  InterpLua,
  BaselineJit,
  OptJit,
  CApi,
  Continue,
  Protected,
};

struct CallFrame {
  Closure* cl = nullptr;
  Proto* proto = nullptr;
  int base = 0;
  int saved_pc = 0;
  int expected_results = 0;
  int nvarargs = 0;      // extra args beyond numparams
  int vararg_base = 0;   // absolute stack index of first vararg (or -1)
  FrameKind kind = FrameKind::InterpLua;
  bool protected_call = false;
  int protect_top = 0;
  int protect_frames = 0;
};

struct Thread : GcObject {
  std::vector<TValue> stack;
  std::vector<CallFrame> frames;
  UpVal* open_upvals = nullptr;
  int top = 0; // stack top (absolute count of used slots) — per-thread
  enum class Status { Fresh, Running, Suspended, Dead, Error } status = Status::Fresh;
  std::string error;
  // After yield: values returned to resume; resume args fill CALL at yield_func_idx.
  std::vector<TValue> yield_vals;
  int yield_func_idx = 0;
  int yield_nresults = 0;
};

struct State {
  Thread* main = nullptr;
  Thread* current = nullptr;
  Table* globals = nullptr;
  Table* registry = nullptr;
  StringTable strings;
  GC gc;
  bool panic_on_error = false;

  // JIT / stress hooks
  bool force_deopt = false;
  bool disable_ic = false;

  // Coroutine yield: set by coroutine.yield C function; honored after C call returns.
  bool yield_pending = false;

  State();
  ~State();

  Thread* thread() { return current; }
  TValue* at(int abs_index);
  int absindex(int idx) const;
  void ensure_stack(int n);
  void push(const TValue& v);
  TValue pop();
  TValue& top_ref();
  int gettop() const;
  void settop(int idx);

  LjString* intern(std::string_view s);
  void close_upvals(Thread* th, int level);
  UpVal* find_upval(Thread* th, int level);

  int load_string(const std::string& source, const std::string& chunk_name);
  int pcall(int nargs, int nresults);
  int resume_call(Closure* cl, int nargs, int nresults);
};

std::unique_ptr<State> new_state();

} // namespace lj3
