#pragma once

#include "gc/gc.hpp"
#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/upvalue.hpp"
#include "runtime/value.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace luatier {

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
  // Extra args beyond numparams (stored off-stack so VARARG multret cannot clobber them).
  std::vector<TValue> varargs;
  FrameKind kind = FrameKind::InterpLua;
  bool protected_call = false;
  bool hooked = false; // true while a debug hook is running for this frame
  bool tailcall = false; // entered via OP_TAILCALL (debug.getinfo istailcall)
  bool finalizer = false; // __gc finalizer call (namewhat "metamethod")
  int protect_top = 0;
  int protect_frames = 0;
  // Yieldable C call (pcall/xpcall): continuation after the protected body
  // returns or errors across a yield. cont_ctx holds xpcall's message handler.
  enum class ContKind : uint8_t { None, PCall, XPCall } cont_kind = ContKind::None;
  TValue cont_ctx{};
  int cont_res_base = 0; // absolute stack index of protected-call results

  // Yieldable metamethod (PUC luaV_finishOp): set on the Lua caller frame while
  // a flat-pushed metamethod runs; cleared when finish_interrupted_op runs.
  bool pending_finish_op = false;
  bool le_invert = false; // OP_LE fell back to not (b < a)
  int meta_res_base = 0;  // absolute slot of metamethod result (call_base)
  // OP_CONCAT progress across yields: next register index to fold, and last.
  int concat_pos = 0;
  int concat_last = 0;
  int concat_dest = 0;
};

struct DebugHookState {
  Closure* func = nullptr;
  int mask = 0;
  int count = 0;
  int hookcount = 0;
  int allowhook = 1;
  int oldpc = 0;
};

struct Thread : GcObject {
  std::vector<TValue> stack;
  std::vector<CallFrame> frames;
  DebugHookState hook;
  UpVal* open_upvals = nullptr;
  int top = 0; // absolute index one past last used slot
  // C-call window: Lua API indices are relative to stack_base (at(1) == stack[stack_base]).
  int stack_base = 0;
  enum class Status { Fresh, Running, Suspended, Dead, Error } status = Status::Fresh;
  std::string error;
  // Non-yieldable C-call nesting (PUC L->nny). Yield only when nny==0.
  int nny = 0;
  // Nested C-call / resume depth (PUC L->nCcalls). Limit: LUAI_MAXCCALLS.
  unsigned short nCcalls = 0;
  // Original object from error(obj); used by pcall/xpcall instead of only the string.
  TValue err_obj{};
  bool err_obj_set = false;
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
  // Per-type metatables (PUC G(L)->mt[]): String, number (Float slot; Int shares it), etc.
  std::array<Table*, static_cast<size_t>(ValueTag::Internal) + 1> type_mt{};
  StringTable strings;
  GC gc;
  bool panic_on_error = false;

  // JIT / stress hooks
  bool force_deopt = false;
  bool disable_ic = false;

  // Coroutine yield: set by coroutine.yield C function; honored after C call returns.
  bool yield_pending = false;
  // When set, run_c_call keeps the C frame as a continuation and does not
  // rebind yield_func_idx (nested yieldable C: pcall/xpcall).
  bool yield_continue = false;
  CallFrame::ContKind yield_cont_kind = CallFrame::ContKind::None;
  TValue yield_cont_ctx{};
  int yield_cont_res_base = 0;

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
  // Absolute stack top (ignores C-call stack_base). For the interpreter / VM.
  int abs_top() const { return current->top; }
  void set_abs_top(int abs_idx);

  LjString* intern(std::string_view s);
  void close_upvals(Thread* th, int level);
  UpVal* find_upval(Thread* th, int level);

  int load_string(const std::string& source, const std::string& chunk_name);
  int pcall(int nargs, int nresults);
  int resume_call(Closure* cl, int nargs, int nresults);
};

std::unique_ptr<State> new_state();

} // namespace luatier
