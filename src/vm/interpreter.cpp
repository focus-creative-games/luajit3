#include "vm/interpreter.hpp"

#include "common/common.hpp"
#include "jit/hotness.hpp"
#include "runtime/string.hpp"
#include "runtime/value.hpp"
#ifndef NDEBUG
#include "tools/profile.hpp"
#endif
#include "vm/debug_hook.hpp"
#include "vm/ldebug.hpp"
#include "vm/meta.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef LUA_MULTRET
#define LUA_MULTRET (-1)
#endif
#ifndef LUA_OK
#define LUA_OK 0
#endif
#ifndef LUA_YIELD
#define LUA_YIELD 1
#endif

namespace luatier {

namespace {

// invoke_mm: Lua frame pushed; continue outer interpret.
constexpr int MM_PUSHED = 2;

const char* arith_mt(OpCode op) {
  switch (op) {
  case OpCode::ADD: return "__add";
  case OpCode::SUB: return "__sub";
  case OpCode::MUL: return "__mul";
  case OpCode::DIV: return "__div";
  case OpCode::IDIV: return "__idiv";
  case OpCode::MOD: return "__mod";
  case OpCode::POW: return "__pow";
  case OpCode::BAND: return "__band";
  case OpCode::BOR: return "__bor";
  case OpCode::BXOR: return "__bxor";
  case OpCode::SHL: return "__shl";
  case OpCode::SHR: return "__shr";
  default: return nullptr;
  }
}

bool to_integer(const TValue& v, int64_t* out) {
  if (v.is_int()) {
    *out = v.as_int();
    return true;
  }
  if (v.is_float()) {
    // PUC lua_numbertointeger: n >= minint && n < -minint (== 2^63 as float).
    // Do not use INT64_MAX cast to double (not exact; would accept 2^63).
    double d = v.as_float();
    constexpr double kMin = static_cast<double>(INT64_MIN);
    constexpr double kSup = -kMin; // 2^63 exactly
    if (d >= kMin && d < kSup && std::floor(d) == d) {
      *out = static_cast<int64_t>(d);
      return true;
    }
    return false;
  }
  return false;
}

bool is_bitwise_op(OpCode op) {
  return op == OpCode::BAND || op == OpCode::BOR || op == OpCode::BXOR || op == OpCode::SHL ||
         op == OpCode::SHR;
}

TValue coerce_number(State* L, const TValue& v, int reg, OpCode op) {
  TValue n;
  if (try_to_number(v, &n))
    return n;
  if (is_bitwise_op(op))
    opinterror(L, v, reg, "attempt to perform bitwise operation on a ");
  opinterror(L, v, reg, "attempt to perform arithmetic on a ");
}

TValue arith_raw(State* L, OpCode op, const TValue& a_in, const TValue& b_in, int reg_a,
                 int reg_b) {
  TValue a = coerce_number(L, a_in, reg_a, op);
  TValue b = coerce_number(L, b_in, reg_b, op);
  if (is_bitwise_op(op)) {
    int64_t x, y;
    if (!to_integer(a, &x))
      tointerror(L, a, reg_a);
    if (!to_integer(b, &y))
      tointerror(L, b, reg_b);
    int64_t r = 0;
    // PUC luaV_shiftl: |disp| >= 64 → 0; negative disp reverses direction; bit ops via unsigned.
    auto shiftl = [](int64_t v, int64_t disp) -> int64_t {
      constexpr int64_t NBITS = 64;
      if (disp < 0) {
        if (disp <= -NBITS)
          return 0;
        return static_cast<int64_t>(static_cast<uint64_t>(v) >> static_cast<uint64_t>(-disp));
      }
      if (disp >= NBITS)
        return 0;
      return static_cast<int64_t>(static_cast<uint64_t>(v) << static_cast<uint64_t>(disp));
    };
    switch (op) {
    case OpCode::BAND: r = x & y; break;
    case OpCode::BOR: r = x | y; break;
    case OpCode::BXOR: r = x ^ y; break;
    case OpCode::SHL: r = shiftl(x, y); break;
    case OpCode::SHR: r = shiftl(x, -y); break;
    default: break;
    }
    return TValue::integer(r);
  }
  if ((op == OpCode::ADD || op == OpCode::SUB || op == OpCode::MUL) && a.is_int() && b.is_int()) {
    int64_t x = a.as_int(), y = b.as_int();
    switch (op) {
    case OpCode::ADD: return TValue::integer(x + y);
    case OpCode::SUB: return TValue::integer(x - y);
    case OpCode::MUL: return TValue::integer(x * y);
    default: break;
    }
  }
  // PUC luaV_div / luaV_mod: integer floor division and modulo.
  if ((op == OpCode::IDIV || op == OpCode::MOD) && a.is_int() && b.is_int()) {
    int64_t m = a.as_int(), n = b.as_int();
    if (static_cast<uint64_t>(n) + 1u <= 1u) { // n == 0 or n == -1
      if (n == 0) {
        if (op == OpCode::IDIV)
          runerror(L, "attempt to divide by zero");
        runerror(L, "attempt to perform 'n%0'");
      }
      // n == -1: avoid overflow with minint / -1
      if (op == OpCode::IDIV)
        return TValue::integer(static_cast<int64_t>(0u - static_cast<uint64_t>(m)));
      return TValue::integer(0);
    }
    if (op == OpCode::IDIV) {
      int64_t q = m / n;
      if ((m ^ n) < 0 && m % n != 0)
        q -= 1; // C truncates toward 0; floor needs correction
      return TValue::integer(q);
    }
    int64_t r = m % n;
    if (r != 0 && (m ^ n) < 0)
      r += n;
    return TValue::integer(r);
  }
  if (!a.is_number() || !b.is_number())
    opinterror(L, a.is_number() ? b_in : a_in, a.is_number() ? reg_b : reg_a,
               "attempt to perform arithmetic on a ");
  double x = a.to_number(), y = b.to_number(), r = 0;
  switch (op) {
  case OpCode::ADD: r = x + y; break;
  case OpCode::SUB: r = x - y; break;
  case OpCode::MUL: r = x * y; break;
  case OpCode::DIV: r = x / y; break;
  case OpCode::IDIV:
    // Float // : IEEE floor(x/y); 0 divisor → ±inf (no runerror).
    r = std::floor(x / y);
    break;
  case OpCode::MOD:
    // Float % : PUC luai_nummod via fmod; 0 → NaN (no runerror).
    r = std::fmod(x, y);
    if (r * y < 0)
      r += y;
    break;
  case OpCode::POW: r = std::pow(x, y); break;
  default: panic("bad arith");
  }
  return TValue::number(r);
}

TValue do_arith(State* L, OpCode op, const TValue& a, const TValue& b, int reg_a = -1,
                int reg_b = -1) {
  TValue na, nb;
  if (try_to_number(a, &na) && try_to_number(b, &nb))
    return arith_raw(L, op, na, nb, reg_a, reg_b);
  if (!(a.is_number() && b.is_number())) {
    TValue out;
    const char* mt = arith_mt(op);
    if (mt && meta_arith(L, mt, a, b, &out))
      return out;
  }
  return arith_raw(L, op, a, b, reg_a, reg_b);
}

// PUC luaD_call nesting: raise "C stack overflow" before the native stack dies.
void incr_Ccalls(Thread* th) {
  if (++th->nCcalls >= LUAI_MAXCCALLS) {
    if (th->nCcalls == LUAI_MAXCCALLS)
      panic("C stack overflow");
    if (th->nCcalls >= LUAI_MAXCCALLS + (LUAI_MAXCCALLS >> 3))
      panic("error in error handling");
  }
}

struct CCallDepth {
  Thread* th;
  explicit CCallDepth(Thread* t) : th(t) { incr_Ccalls(th); }
  ~CCallDepth() {
    if (th && th->nCcalls > 0)
      --th->nCcalls;
  }
  CCallDepth(const CCallDepth&) = delete;
  CCallDepth& operator=(const CCallDepth&) = delete;
};

void push_lua_frame(State* L, Closure* cl, int func_idx, int nargs, int nresults,
                    bool is_tailcall = false) {
  Thread* th = L->current;
  // Hard cap: a broken upvalue/tailcall must not grow the stack into multi-GB.
  if (th->frames.size() > 100000)
    panic("stack overflow");
  Proto* p = cl->proto;
  hotness_on_entry(p);
  CallFrame fr;
  fr.cl = cl;
  fr.proto = p;
  fr.base = func_idx;
  fr.expected_results = nresults;
  fr.saved_pc = 0;
  fr.kind = FrameKind::InterpLua;
  fr.tailcall = is_tailcall;

  int nfixed = p->numparams;
  int nvar = 0;
  if (p->is_vararg && nargs > nfixed)
    nvar = nargs - nfixed;

  fr.varargs.resize(static_cast<size_t>(nvar));
  for (int i = 0; i < nvar; ++i)
    fr.varargs[static_cast<size_t>(i)] = th->stack[static_cast<size_t>(func_idx + 1 + nfixed + i)];

  for (int i = 0; i < nfixed; ++i) {
    if (i < nargs)
      th->stack[static_cast<size_t>(fr.base + i)] =
          th->stack[static_cast<size_t>(func_idx + 1 + i)];
    else
      th->stack[static_cast<size_t>(fr.base + i)] = TValue::nil();
  }

  int want = std::max(p->maxstack, nfixed);
  L->ensure_stack(fr.base + want + 8);
  for (int i = nfixed; i < want; ++i)
    th->stack[static_cast<size_t>(fr.base + i)] = TValue::nil();

  L->current->top = fr.base + want;
  th->frames.push_back(std::move(fr));
  if (is_tailcall)
    debug_on_tailcall(L, th);
  else
    debug_on_call(L, th);
}

int run_c_call(State* L, Closure* cl, int func_idx, int nargs, int nresults,
               bool is_tailcall = false, const char* invoked_name = nullptr) {
  Thread* th = L->current;
  if (func_idx < 0 || nargs < 0)
    panic("run_c_call: bad func_idx/nargs");
  if (static_cast<int>(th->stack.size()) < func_idx + 1 + nargs)
    panic("run_c_call: stack underflow");

  CCallDepth _cc(th); // PUC luaD_call nCcalls++

  // Push a C frame so debug levels match PUC (level 0 = C, level 1 = Lua caller).
  CallFrame cfr;
  cfr.cl = cl;
  cfr.proto = nullptr;
  cfr.base = func_idx;
  cfr.kind = FrameKind::CApi;
  cfr.tailcall = is_tailcall;
  if (invoked_name && invoked_name[0])
    cfr.invoked_name = invoked_name;
  th->frames.push_back(cfr);
  if (is_tailcall)
    debug_on_tailcall(L, th);
  else
    debug_on_call(L, th);

  // Keep the full Lua stack intact so open upvalues remain valid. Expose a
  // C API window where at(1) is the first argument (slot func_idx+1).
  const int prev_base = th->stack_base;
  const int prev_top = th->top;
  th->stack_base = func_idx + 1;
  th->top = func_idx + 1 + nargs;
  L->yield_pending = false;
  th->nny++;
  int nret = 0;
  try {
    nret = cl->cfunc(L);
  } catch (const LuatierError&) {
    th->nny--;
    th->stack_base = prev_base;
    // Non-main threads keep the C frame so debug.traceback can show
    // `error` / the failing C call on a dead coroutine (db.lua).
    if (th != L->main) {
      th->top = func_idx;
      throw;
    }
    debug_return_hook(L, th);
    th->frames.pop_back();
    debug_on_return(L, th);
    th->top = func_idx;
    // Clear the call frame slot area.
    for (int i = func_idx; i < prev_top; ++i)
      th->stack[static_cast<size_t>(i)] = TValue::nil();
    throw;
  }

  // Lua C API: the `nret` results are the topmost values in the C window.
  if (nret < 0)
    nret = 0;
  int ctop = L->gettop();
  if (nret > ctop)
    nret = ctop;
  std::vector<TValue> results(static_cast<size_t>(nret));
  for (int i = 0; i < nret; ++i)
    results[static_cast<size_t>(i)] = *L->at(ctop - nret + i + 1);

  // Yield: keep the C frame on the stack (PUC CallInfo stays) so traceback can
  // see `coroutine.yield` and resume can finish the call.
  if (L->yield_pending) {
    th->stack_base = prev_base;
    if (L->yield_continue) {
      // Nested yieldable C (pcall/xpcall): mark *this* C frame as a continuation.
      // frames.back() may be the inner yield CApi frame kept below — find ours.
      CallFrame* self = nullptr;
      for (int i = static_cast<int>(th->frames.size()) - 1; i >= 0; --i) {
        auto& fr = th->frames[static_cast<size_t>(i)];
        if (fr.cl == cl && fr.kind == FrameKind::CApi && fr.base == func_idx) {
          self = &fr;
          break;
        }
      }
      if (!self)
        panic("yieldable C: missing frame");
      self->kind = FrameKind::Continue;
      self->cont_kind = L->yield_cont_kind;
      self->cont_ctx = L->yield_cont_ctx;
      self->cont_res_base = L->yield_cont_res_base;
      self->expected_results = nresults;
      L->yield_continue = false;
      L->yield_cont_kind = CallFrame::ContKind::None;
      L->yield_cont_ctx = TValue::nil();
      L->yield_cont_res_base = 0;
      L->yield_pending = false;
      // pcall/xpcall already cancelled this frame's nny++ via nny-- around the
      // protected call; leave nny unchanged on the continue path.
      return LUA_YIELD;
    }
    th->yield_vals = std::move(results);
    th->yield_func_idx = func_idx;
    th->yield_nresults = nresults;
    L->ensure_stack(func_idx + 8);
    th->top = func_idx;
    L->yield_pending = false;
    th->nny--; // drop this C frame's nny; frame stays for resume
    return LUA_YIELD;
  }

  th->nny--;
  debug_return_hook(L, th);
  th->frames.pop_back();
  debug_on_return(L, th);

  th->stack_base = prev_base;
  int want = (nresults == LUA_MULTRET) ? nret : nresults;
  if (want < 0)
    want = 0;
  L->ensure_stack(func_idx + want + 8);
  for (int i = 0; i < want; ++i)
    th->stack[static_cast<size_t>(func_idx + i)] =
        (i < nret) ? results[static_cast<size_t>(i)] : TValue::nil();
  // Clear any leftover C-window slots above the results so stale values cannot
  // be mistaken for live registers on subsequent instructions.
  int clear_to = std::max(prev_top, func_idx + 1 + nargs);
  for (int i = func_idx + want; i < clear_to; ++i)
    th->stack[static_cast<size_t>(i)] = TValue::nil();
  th->top = func_idx + want;
  return LUA_OK;
}

int live_regs_top(Thread* th) {
  int limit = th->top;
  for (auto& fr : th->frames) {
    if (fr.proto)
      limit = std::max(limit, fr.base + fr.proto->maxstack);
    else if (fr.cl && fr.cl->proto)
      limit = std::max(limit, fr.base + fr.cl->proto->maxstack);
    else if (fr.cl && fr.cl->is_c)
      limit = std::max(limit, fr.base + 1);
  }
  return limit;
}

// Flat metamethod call from a Lua opcode (PUC luaT_callTM via luaD_call).
// Sets caller pending_finish_op. Returns LUA_OK (C MM done), LUA_YIELD, or MM_PUSHED.
int invoke_mm(State* L, size_t caller_fi, const TValue& mm, const TValue* args, int nargs,
              int nout) {
  Thread* th = L->current;
  if (!mm.is_function())
    panic("invoke_mm: function expected");
  auto& cfr = th->frames[caller_fi];
  const int call_base = live_regs_top(th);
  L->ensure_stack(call_base + 1 + nargs + nout + 8);
  th->stack[static_cast<size_t>(call_base)] = mm;
  for (int i = 0; i < nargs; ++i)
    th->stack[static_cast<size_t>(call_base + 1 + i)] = args[i];
  th->top = call_base + 1 + nargs;
  cfr.pending_finish_op = true;
  cfr.meta_res_base = call_base;
  Closure* cl = mm.as_closure();
  if (cl->is_c) {
    int st = run_c_call(L, cl, call_base, nargs, nout);
    if (st == LUA_YIELD)
      return LUA_YIELD;
    return LUA_OK;
  }
  push_lua_frame(L, cl, call_base, nargs, nout);
  return MM_PUSHED;
}

int finish_interrupted_op(State* L, size_t fi) {
  Thread* th = L->current;
  auto& fr = th->frames[fi];
  if (!fr.pending_finish_op || !fr.proto)
    return LUA_OK;
  fr.pending_finish_op = false;
  const int opc = fr.saved_pc - 1;
  if (opc < 0 || opc >= static_cast<int>(fr.proto->code.size()))
    panic("finish_interrupted_op: bad pc");
  Instruction ins = fr.proto->code[static_cast<size_t>(opc)];
  OpCode op = static_cast<OpCode>(op_get(ins));
  int a = op_a(ins);
  int b = op_b(ins);
  int c = op_c(ins);
  (void)b;
  (void)c;
  TValue* base = th->stack.data() + fr.base;
  TValue res = th->stack[static_cast<size_t>(fr.meta_res_base)];

  switch (op) {
  case OpCode::EQ:
  case OpCode::LT:
  case OpCode::LE: {
    bool r = res.is_truthy();
    if (fr.le_invert)
      r = !r;
    fr.le_invert = false;
    if (r != (a != 0))
      fr.saved_pc++;
    break;
  }
  case OpCode::ADD:
  case OpCode::SUB:
  case OpCode::MUL:
  case OpCode::DIV:
  case OpCode::IDIV:
  case OpCode::MOD:
  case OpCode::POW:
  case OpCode::BAND:
  case OpCode::BOR:
  case OpCode::BXOR:
  case OpCode::SHL:
  case OpCode::SHR:
  case OpCode::UNM:
  case OpCode::BNOT:
  case OpCode::LEN:
    base[a] = res;
    break;
  case OpCode::GETTABLE:
  case OpCode::GETI:
  case OpCode::GETFIELD:
  case OpCode::GETTABUP:
    base[a] = res;
    break;
  case OpCode::SELF:
    base[a] = res;
    break;
  case OpCode::SETTABLE:
  case OpCode::SETI:
  case OpCode::SETFIELD:
  case OpCode::SETTABUP:
    break;
  case OpCode::CONCAT: {
    int pos = fr.concat_pos;
    int last = fr.concat_last;
    int dest = fr.concat_dest;
    TValue acc = res;
    pos++;
    while (pos <= last) {
      TValue rhs = th->stack[static_cast<size_t>(fr.base + pos)];
      if ((acc.is_string() || acc.is_number()) && (rhs.is_string() || rhs.is_number())) {
        acc = TValue::obj(ValueTag::String,
                          L->intern(value_to_string(acc) + value_to_string(rhs)));
        pos++;
        continue;
      }
      TValue mm = get_metamethod(L, acc, "__concat");
      if (mm.is_nil())
        mm = get_metamethod(L, rhs, "__concat");
      if (mm.is_nil() || !mm.is_function())
        if (!(acc.is_string() || acc.is_number()) || !(rhs.is_string() || rhs.is_number())) {
          th->err_reg = pos;
          if (!(acc.is_string() || acc.is_number()))
            runerror(L, "attempt to concatenate a " + obj_type_name(L, acc) + " value" +
                            varinfo_reg(L, -1));
          runerror(L, "attempt to concatenate a " + obj_type_name(L, rhs) + " value" +
                          varinfo_reg(L, -1));
        }
      fr.concat_pos = pos;
      fr.concat_last = last;
      fr.concat_dest = dest;
      TValue args[2] = {acc, rhs};
      int st = invoke_mm(L, fi, mm, args, 2, 1);
      if (st == LUA_OK) {
        acc = th->stack[static_cast<size_t>(fr.meta_res_base)];
        fr.pending_finish_op = false;
        pos++;
        continue;
      }
      if (st == LUA_YIELD)
        return LUA_YIELD;
      return LUA_OK; // MM_PUSHED
    }
    base[dest] = acc;
    break;
  }
  default:
    panic("finish_interrupted_op: unexpected opcode");
  }
  return LUA_OK;
}

// Resolve __index with possible yield. Writes to *out when DONE.
// Returns LUA_OK (out set), LUA_YIELD, or MM_PUSHED.
int index_yieldable(State* L, size_t fi, const TValue& table, const TValue& key, TValue* out) {
  TValue t = table;
  for (int loop = 0; loop < 2000; ++loop) {
    if (t.is_table()) {
      TValue v = t.as_table()->get(key);
      if (!v.is_nil()) {
        *out = v;
        return LUA_OK;
      }
      TValue mm = get_metamethod(L, t, "__index");
      if (mm.is_nil()) {
        *out = TValue::nil();
        return LUA_OK;
      }
      if (mm.is_function()) {
        TValue args[2] = {t, key};
        return invoke_mm(L, fi, mm, args, 2, 1);
      }
      t = mm;
      continue;
    }
    TValue mm = get_metamethod(L, t, "__index");
    if (mm.is_nil())
      typeerror(L, t, -1, "index");
    if (mm.is_function()) {
      TValue args[2] = {t, key};
      return invoke_mm(L, fi, mm, args, 2, 1);
    }
    t = mm;
  }
  panic("'__index' chain too long; possible loop");
}

int newindex_yieldable(State* L, size_t fi, const TValue& table, const TValue& key,
                       const TValue& value) {
  TValue t = table;
  for (int loop = 0; loop < 2000; ++loop) {
    if (t.is_table()) {
      TValue cur = t.as_table()->get(key);
      if (!cur.is_nil()) {
        t.as_table()->set(L, key, value);
        return LUA_OK;
      }
      TValue mm = get_metamethod(L, t, "__newindex");
      if (mm.is_nil()) {
        t.as_table()->set(L, key, value);
        return LUA_OK;
      }
      if (mm.is_function()) {
        TValue args[3] = {t, key, value};
        return invoke_mm(L, fi, mm, args, 3, 0);
      }
      t = mm;
      continue;
    }
    TValue mm = get_metamethod(L, t, "__newindex");
    if (mm.is_nil())
      typeerror(L, t, -1, "index");
    if (mm.is_function()) {
      TValue args[3] = {t, key, value};
      return invoke_mm(L, fi, mm, args, 3, 0);
    }
    t = mm;
  }
  panic("'__newindex' chain too long; possible loop");
}

} // namespace

int call_closure(State* L, Closure* cl, int nargs, int nresults) {
  int func_idx = L->current->top - nargs - 1;
  if (func_idx < 0)
    panic("call_closure stack underflow");
  if (cl->is_c)
    return run_c_call(L, cl, func_idx, nargs, nresults);
  // Nested interpret from C counts as a C-call level (PUC luaD_call).
  CCallDepth _cc(L->current);
  push_lua_frame(L, cl, func_idx, nargs, nresults);
  return interpret(L);
}

int interpret(State* L, int min_frames) {
  Thread* th = L->current;
  const int entry_depth =
      (min_frames < 0) ? static_cast<int>(th->frames.size()) : min_frames;

  // Use frame index (not CallFrame&) so reentrant call_closure / meta calls that
  // push_back on th->frames cannot leave a dangling reference after reallocation.
  while (static_cast<int>(th->frames.size()) >= entry_depth && !th->frames.empty()) {
    const size_t fi = th->frames.size() - 1;
    auto& fr0 = th->frames[fi];
    // Hit a C/continuation frame: return to the resume driver.
    if (fr0.kind == FrameKind::Continue || fr0.kind == FrameKind::CApi || !fr0.proto)
      return LUA_OK;
    // Metamethod returned: finish the interrupted opcode (PUC luaV_finishOp).
    if (fr0.pending_finish_op) {
      int st = finish_interrupted_op(L, fi);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      continue;
    }
    Closure* cl = fr0.cl;
    Proto* p = fr0.proto;
    if (fr0.saved_pc >= static_cast<int>(p->code.size())) {
      debug_return_hook(L, th);
      th->frames.pop_back();
      debug_on_return(L, th);
      if (static_cast<int>(th->frames.size()) < entry_depth)
        return LUA_OK;
      continue;
    }

    // Match PUC vmfetch: advance saved_pc first, then line/count hooks see
    // currentpc = saved_pc - 1 (the instruction about to run).
    auto& fr_fetch = th->frames[fi];
    const int insn_pc = fr_fetch.saved_pc;
    if (insn_pc >= static_cast<int>(p->code.size()))
      continue;
    Instruction ins = p->code[static_cast<size_t>(insn_pc)];
    fr_fetch.saved_pc = insn_pc + 1;
    debug_trace_exec(L, th, fi, p, insn_pc);
    auto& fr1 = th->frames[fi];
    cl = fr1.cl;
    p = fr1.proto;
    OpCode op = static_cast<OpCode>(op_get(ins));
    int a = op_a(ins);
    int b = op_b(ins);
    int c = op_c(ins);

    auto reload = [&]() -> TValue* {
      auto& fr = th->frames[fi];
      cl = fr.cl;
      p = fr.proto;
      L->ensure_stack(fr.base + p->maxstack + static_cast<int>(fr.varargs.size()) + 8);
      // Do not raise th->top to maxstack here: CALL/RETURN MULTRET rely on top
      // marking the end of the variable result range. GC uses thread_live_top().
      return th->stack.data() + fr.base;
    };
    auto fbase = [&]() -> int { return th->frames[fi].base; };
    auto pc = [&]() -> int& { return th->frames[fi].saved_pc; };

    TValue* base = reload();
    L->gc.safepoint();
    // Finalizers / nested calls during GC may reallocate the stack.
    base = reload();
#ifndef NDEBUG
    runtime_profile().opcodes++;
#endif

    switch (op) {
    case OpCode::MOVE:
      base[a] = base[b];
      break;
    case OpCode::LOADNIL:
      for (int r = a; r <= a + b; ++r)
        base[r] = TValue::nil();
      break;
    case OpCode::LOADBOOL:
      base[a] = TValue::boolean(b != 0);
      if (c)
        pc()++;
      break;
    case OpCode::LOADINT:
      base[a] = TValue::integer(op_sbx(ins));
      break;
    case OpCode::LOADK:
    case OpCode::LOADFLOAT:
      base[a] = p->constants[op_bx(ins)];
      break;
    case OpCode::GETUPVAL:
      if (static_cast<size_t>(b) >= cl->upvals.size() || !cl->upvals[static_cast<size_t>(b)])
        panic("GETUPVAL: bad upvalue");
      base[a] = cl->upvals[static_cast<size_t>(b)]->get();
      break;
    case OpCode::SETUPVAL:
      if (static_cast<size_t>(b) >= cl->upvals.size() || !cl->upvals[static_cast<size_t>(b)])
        panic("SETUPVAL: bad upvalue");
      cl->upvals[static_cast<size_t>(b)]->set(L, base[a]);
      break;
    case OpCode::GETTABUP: {
      if (static_cast<size_t>(b) >= cl->upvals.size() || !cl->upvals[static_cast<size_t>(b)])
        panic("GETTABUP: bad upvalue");
      if (static_cast<size_t>(c) >= p->constants.size())
        panic("GETTABUP: bad constant");
      TValue t = cl->upvals[static_cast<size_t>(b)]->get();
      TValue key = p->constants[static_cast<size_t>(c)];
      TValue v;
      int st = index_yieldable(L, fi, t, key, &v);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      } else {
        base = reload();
        base[a] = v;
      }
      break;
    }
    case OpCode::SETTABUP: {
      TValue t = cl->upvals[static_cast<size_t>(a)]->get();
      TValue key = p->constants[static_cast<size_t>(b)];
      TValue val = base[c];
      int st = newindex_yieldable(L, fi, t, key, val);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      }
      (void)reload();
      break;
    }
    case OpCode::GETTABLE: {
      th->err_reg = b;
      TValue v;
      int st = index_yieldable(L, fi, base[b], base[c], &v);
      th->err_reg = -1;
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      } else {
        base = reload();
        base[a] = v;
      }
      break;
    }
    case OpCode::SETTABLE: {
      th->err_reg = a;
      TValue t = base[a], key = base[b], val = base[c];
      int st = newindex_yieldable(L, fi, t, key, val);
      th->err_reg = -1;
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      }
      (void)reload();
      break;
    }
    case OpCode::GETI: {
      th->err_reg = b;
      TValue v;
      int st = index_yieldable(L, fi, base[b], TValue::integer(c), &v);
      th->err_reg = -1;
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      } else {
        base = reload();
        base[a] = v;
      }
      break;
    }
    case OpCode::SETI: {
      th->err_reg = a;
      TValue t = base[a], val = base[c];
      int st = newindex_yieldable(L, fi, t, TValue::integer(b), val);
      th->err_reg = -1;
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      }
      (void)reload();
      break;
    }
    case OpCode::GETFIELD: {
      th->err_reg = b;
      TValue v;
      int st = index_yieldable(L, fi, base[b], p->constants[static_cast<size_t>(c)], &v);
      th->err_reg = -1;
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      } else {
        base = reload();
        base[a] = v;
      }
      break;
    }
    case OpCode::SETFIELD: {
      th->err_reg = a;
      TValue t = base[a], val = base[c];
      int st = newindex_yieldable(L, fi, t, p->constants[static_cast<size_t>(b)], val);
      th->err_reg = -1;
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      }
      (void)reload();
      break;
    }
    case OpCode::NEWTABLE:
      base[a] = TValue::obj(ValueTag::Table, table_new(L, b ? (1u << b) : 0, c ? (1u << c) : 0));
      break;
    case OpCode::SELF: {
      TValue t = base[b];
      TValue key = base[c];
      base[a + 1] = t;
      th->err_reg = b;
      TValue m;
      int st = index_yieldable(L, fi, t, key, &m);
      th->err_reg = -1;
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      if (th->frames[fi].pending_finish_op) {
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
        base = reload();
        base[a + 1] = t;
      } else {
        base = reload();
        base[a + 1] = t;
        base[a] = m;
      }
      break;
    }
    case OpCode::ADD:
    case OpCode::SUB:
    case OpCode::MUL:
    case OpCode::DIV:
    case OpCode::IDIV:
    case OpCode::MOD:
    case OpCode::POW:
    case OpCode::BAND:
    case OpCode::BOR:
    case OpCode::BXOR:
    case OpCode::SHL:
    case OpCode::SHR: {
      TValue lhs = base[b], rhs = base[c];
      TValue na, nb;
      if (try_to_number(lhs, &na) && try_to_number(rhs, &nb)) {
        base[a] = arith_raw(L, op, na, nb, b, c);
        break;
      }
      const char* mt = arith_mt(op);
      TValue mm = mt ? get_metamethod(L, lhs, mt) : TValue::nil();
      if (mm.is_nil() && mt)
        mm = get_metamethod(L, rhs, mt);
      if (mm.is_nil() || !mm.is_function()) {
        (void)do_arith(L, op, lhs, rhs, b, c); // panic with the usual message
        break;
      }
      TValue args[2] = {lhs, rhs};
      int st = invoke_mm(L, fi, mm, args, 2, 1);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      st = finish_interrupted_op(L, fi);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      break;
    }
    case OpCode::UNM: {
      TValue nb;
      if (try_to_number(base[b], &nb)) {
        if (nb.is_int())
          base[a] = TValue::integer(-nb.as_int());
        else
          base[a] = TValue::number(-nb.as_float());
      } else {
        TValue mm = get_metamethod(L, base[b], "__unm");
        if (mm.is_nil() || !mm.is_function()) {
          th->err_reg = b;
          opinterror(L, base[b], b, "attempt to perform arithmetic on a ");
        }
        TValue args[1] = {base[b]};
        int st = invoke_mm(L, fi, mm, args, 1, 1);
        if (st == LUA_YIELD)
          return LUA_YIELD;
        if (st == MM_PUSHED)
          break;
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      }
      break;
    }
    case OpCode::BNOT: {
      int64_t x;
      if (to_integer(base[b], &x))
        base[a] = TValue::integer(~x);
      else {
        TValue mm = get_metamethod(L, base[b], "__bnot");
        if (mm.is_nil() || !mm.is_function()) {
          if (!base[b].is_number())
            opinterror(L, base[b], b, "attempt to perform bitwise operation on a ");
          tointerror(L, base[b], b);
        }
        TValue args[1] = {base[b]};
        int st = invoke_mm(L, fi, mm, args, 1, 1);
        if (st == LUA_YIELD)
          return LUA_YIELD;
        if (st == MM_PUSHED)
          break;
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      }
      break;
    }
    case OpCode::NOT:
      base[a] = TValue::boolean(!base[b].is_truthy());
      break;
    case OpCode::LEN: {
      TValue vb = base[b];
      if (vb.is_string()) {
        base[a] = TValue::integer(static_cast<int64_t>(vb.as_string()->len));
        break;
      }
      if (vb.is_table()) {
        TValue mm = get_metamethod(L, vb, "__len");
        if (mm.is_nil()) {
          base[a] = TValue::integer(table_length(vb.as_table()));
          break;
        }
        if (mm.is_function()) {
          TValue args[1] = {vb};
          int st = invoke_mm(L, fi, mm, args, 1, 1);
          if (st == LUA_YIELD)
            return LUA_YIELD;
          if (st == MM_PUSHED)
            break;
          st = finish_interrupted_op(L, fi);
          if (st == LUA_YIELD)
            return LUA_YIELD;
          break;
        }
      }
      TValue out;
      th->err_reg = b;
      if (!meta_len(L, vb, &out))
        typeerror(L, vb, b, "get length of");
      th->err_reg = -1;
      base = reload();
      base[a] = out;
      break;
    }
    case OpCode::CONCAT: {
      auto& cfr = th->frames[fi];
      cfr.concat_pos = b;
      cfr.concat_last = c;
      cfr.concat_dest = a;
      TValue acc = base[b];
      int pos = b + 1;
      while (pos <= c) {
        base = reload();
        TValue rhs = base[pos];
        if ((acc.is_string() || acc.is_number()) && (rhs.is_string() || rhs.is_number())) {
          acc = TValue::obj(ValueTag::String,
                            L->intern(value_to_string(acc) + value_to_string(rhs)));
          pos++;
          continue;
        }
        TValue mm = get_metamethod(L, acc, "__concat");
        if (mm.is_nil())
          mm = get_metamethod(L, rhs, "__concat");
        if (mm.is_nil() || !mm.is_function())
          if (!(acc.is_string() || acc.is_number()) || !(rhs.is_string() || rhs.is_number())) {
          th->err_reg = pos;
          if (!(acc.is_string() || acc.is_number()))
            runerror(L, "attempt to concatenate a " + obj_type_name(L, acc) + " value" +
                            varinfo_reg(L, -1));
          runerror(L, "attempt to concatenate a " + obj_type_name(L, rhs) + " value" +
                          varinfo_reg(L, -1));
        }
        cfr.concat_pos = pos;
        TValue args[2] = {acc, rhs};
        int st = invoke_mm(L, fi, mm, args, 2, 1);
        if (st == LUA_YIELD)
          return LUA_YIELD;
        if (st == MM_PUSHED)
          break;
        // C MM done: take result and continue folding
        acc = th->stack[static_cast<size_t>(cfr.meta_res_base)];
        cfr.pending_finish_op = false;
        pos++;
        if (pos > c) {
          base = reload();
          base[a] = acc;
        }
        continue;
      }
      if (pos > c && !cfr.pending_finish_op) {
        base = reload();
        base[a] = acc;
      }
      break;
    }
    case OpCode::EQ: {
      TValue rb = base[b], rc = base[c];
      if (rb.tag() != rc.tag() && !(rb.is_number() && rc.is_number())) {
        if (a != 0)
          pc()++; // eq==false; skip JMP when A wants true
        break;
      }
      bool eq = false;
      if (rb.is_table() || rb.is_function() || rb.tag() == ValueTag::Userdata) {
        if (rb.payload == rc.payload) {
          eq = true;
        } else {
          TValue mm = get_metamethod(L, rb, "__eq");
          if (mm.is_nil())
            mm = get_metamethod(L, rc, "__eq");
          if (!mm.is_nil() && mm.is_function()) {
            TValue args[2] = {rb, rc};
            int st = invoke_mm(L, fi, mm, args, 2, 1);
            if (st == LUA_YIELD)
              return LUA_YIELD;
            if (st == MM_PUSHED)
              break;
            st = finish_interrupted_op(L, fi);
            if (st == LUA_YIELD)
              return LUA_YIELD;
            break;
          }
          eq = false;
        }
      } else {
        eq = values_equal(rb, rc);
      }
      if (eq != (a != 0))
        pc()++;
      break;
    }
    case OpCode::LT: {
      TValue rb = base[b], rc = base[c];
      if (rb.is_number() && rc.is_number()) {
        bool lt = number_lt(rb, rc);
        if (lt != (a != 0))
          pc()++;
        break;
      }
      if (rb.is_string() && rc.is_string()) {
        bool lt = false;
        meta_lt(L, rb, rc, &lt);
        if (lt != (a != 0))
          pc()++;
        break;
      }
      TValue mm = get_metamethod(L, rb, "__lt");
      if (mm.is_nil())
        mm = get_metamethod(L, rc, "__lt");
      if (mm.is_nil() || !mm.is_function())
        compareerror(L, rb, rc);
      th->frames[fi].le_invert = false;
      TValue args[2] = {rb, rc};
      int st = invoke_mm(L, fi, mm, args, 2, 1);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      st = finish_interrupted_op(L, fi);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      break;
    }
    case OpCode::LE: {
      TValue rb = base[b], rc = base[c];
      if (rb.is_number() && rc.is_number()) {
        bool le = number_le(rb, rc);
        if (le != (a != 0))
          pc()++;
        break;
      }
      if (rb.is_string() && rc.is_string()) {
        bool le = false;
        meta_le(L, rb, rc, &le);
        if (le != (a != 0))
          pc()++;
        break;
      }
      TValue mm = get_metamethod(L, rb, "__le");
      if (mm.is_nil())
        mm = get_metamethod(L, rc, "__le");
      if (!mm.is_nil() && mm.is_function()) {
        th->frames[fi].le_invert = false;
        TValue args[2] = {rb, rc};
        int st = invoke_mm(L, fi, mm, args, 2, 1);
        if (st == LUA_YIELD)
          return LUA_YIELD;
        if (st == MM_PUSHED)
          break;
        st = finish_interrupted_op(L, fi);
        if (st == LUA_YIELD)
          return LUA_YIELD;
        break;
      }
      // fallback: not (rc < rb) via __lt
      mm = get_metamethod(L, rc, "__lt");
      if (mm.is_nil())
        mm = get_metamethod(L, rb, "__lt");
      if (mm.is_nil() || !mm.is_function())
        compareerror(L, rb, rc);
      th->frames[fi].le_invert = true;
      TValue args[2] = {rc, rb}; // b < a  ⇒  invert for a <= b
      int st = invoke_mm(L, fi, mm, args, 2, 1);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      if (st == MM_PUSHED)
        break;
      st = finish_interrupted_op(L, fi);
      if (st == LUA_YIELD)
        return LUA_YIELD;
      break;
    }
    case OpCode::TEST:
      if (base[a].is_truthy() != (c != 0))
        pc()++;
      break;
    case OpCode::TESTSET:
      if (base[b].is_truthy() != (c != 0))
        pc()++;
      else
        base[a] = base[b];
      break;
    case OpCode::JMP: {
      if (a != 0) {
        int level = fbase() + (a - 1);
        L->close_upvals(th, level);
      }
      pc() += op_sbx(ins);
      hotness_on_loop(p);
      break;
    }
    case OpCode::FORPREP: {
      // Coerce & pick integer vs float loop (PUC OP_FORPREP / forlimit).
      // Integer +/- must wrap like Lua intop (unsigned), otherwise maxinteger
      // loops are UB / can hang under optimizing compilers.
      auto wrap_add = [](int64_t x, int64_t y) -> int64_t {
        return static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y));
      };
      auto wrap_sub = [](int64_t x, int64_t y) -> int64_t {
        return static_cast<int64_t>(static_cast<uint64_t>(x) - static_cast<uint64_t>(y));
      };
      auto tointeger_mode = [](const TValue& obj, int64_t* p, int mode) -> bool {
        TValue n;
        if (!try_to_number(obj, &n))
          return false;
        if (n.is_int() && mode != 0) {
          *p = n.as_int();
          return true;
        }
        double d = n.to_number();
        if (mode == 1)
          d = std::floor(d);
        else if (mode == 2)
          d = std::ceil(d);
        if (d != d)
          return false;
        if (d < static_cast<double>(INT64_MIN) || d > static_cast<double>(INT64_MAX))
          return false;
        int64_t i = static_cast<int64_t>(d);
        if (mode == 0 && static_cast<double>(i) != d)
          return false;
        *p = i;
        return true;
      };
      auto forlimit = [&](const TValue& obj, int64_t step, int64_t* p, bool* stopnow) -> bool {
        *stopnow = false;
        int mode = (step < 0) ? 2 : 1;
        if (tointeger_mode(obj, p, mode))
          return true;
        TValue nf;
        if (!try_to_number(obj, &nf))
          return false;
        double n = nf.to_number();
        if (n > 0) {
          *p = INT64_MAX;
          if (step < 0)
            *stopnow = true;
        } else {
          *p = INT64_MIN;
          if (step >= 0)
            *stopnow = true;
        }
        return true;
      };

      if (base[a].is_int() && base[a + 2].is_int()) {
        int64_t step = base[a + 2].as_int();
        int64_t ilimit = 0;
        bool stopnow = false;
        if (forlimit(base[a + 1], step, &ilimit, &stopnow)) {
          int64_t initv = stopnow ? 0 : base[a].as_int();
          base[a + 1] = TValue::integer(ilimit);
          base[a] = TValue::integer(wrap_sub(initv, step));
          pc() += op_sbx(ins);
          break;
        }
      }
      TValue nlimit, nstep, ninit;
      if (!try_to_number(base[a + 1], &nlimit))
        runerror(L, "'for' limit must be a number");
      if (!try_to_number(base[a + 2], &nstep))
        runerror(L, "'for' step must be a number");
      if (!try_to_number(base[a], &ninit))
        runerror(L, "'for' initial value must be a number");
      double step = nstep.to_number();
      base[a + 1] = TValue::number(nlimit.to_number());
      base[a + 2] = TValue::number(step);
      base[a] = TValue::number(ninit.to_number() - step);
      pc() += op_sbx(ins);
      break;
    }
    case OpCode::FORLOOP: {
      if (base[a].is_int() && base[a + 1].is_int() && base[a + 2].is_int()) {
        auto wrap_add = [](int64_t x, int64_t y) -> int64_t {
          return static_cast<int64_t>(static_cast<uint64_t>(x) + static_cast<uint64_t>(y));
        };
        int64_t step = base[a + 2].as_int();
        int64_t idx = wrap_add(base[a].as_int(), step);
        int64_t limit = base[a + 1].as_int();
        if ((step > 0) ? (idx <= limit) : (limit <= idx)) {
          pc() += op_sbx(ins);
          base[a] = TValue::integer(idx);
          base[a + 3] = TValue::integer(idx);
          hotness_on_loop(p);
        }
      } else {
        double step = base[a + 2].to_number();
        double idx = base[a].to_number() + step;
        double limit = base[a + 1].to_number();
        if ((step > 0) ? (idx <= limit) : (idx >= limit)) {
          pc() += op_sbx(ins);
          base[a] = TValue::number(idx);
          base[a + 3] = TValue::number(idx);
          hotness_on_loop(p);
        }
      }
      break;
    }
    case OpCode::TFORCALL: {
      // Lua 5.3: call at R[A+3] so R[A..A+2] (generator/state/control) stay intact.
      // R[A+3], ... ,R[A+2+C] := R[A](R[A+1], R[A+2])
      int cb = fbase() + a + 3;
      L->ensure_stack(cb + 3 + c);
      TValue f = base[a];
      th->stack[static_cast<size_t>(cb)] = f;
      th->stack[static_cast<size_t>(cb + 1)] = base[a + 1];
      th->stack[static_cast<size_t>(cb + 2)] = base[a + 2];
      L->current->top = cb + 3;
      if (!f.is_function()) {
        meta_call(L, cb, 2, c);
      } else {
        int st = call_closure(L, f.as_closure(), 2, c);
        if (st == LUA_YIELD)
          return LUA_YIELD;
      }
      // Results already land at cb == base+a+3; reload only for safety.
      (void)reload();
      break;
    }
    case OpCode::TFORLOOP: {
      // Lua 5.3: A is the control-variable register (typically generator_base+2).
      // if R[A+1] ~= nil then { R[A]=R[A+1]; pc += sBx }
      if (!base[a + 1].is_nil()) {
        base[a] = base[a + 1];
        pc() += op_sbx(ins);
        hotness_on_loop(p);
      }
      break;
    }
    case OpCode::SETLIST: {
      int n = b;
      // B==0: values occupy R[A+1]..R[top-1] (same top convention as CALL B==0).
      if (n == 0)
        n = L->abs_top() - (fbase() + a) - 1;
      int offset = (c - 1) * 50;
      if (!base[a].is_table())
        panic("SETLIST on non-table");
      for (int i = 1; i <= n; ++i)
        base[a].as_table()->set_int(L, offset + i, base[a + i]);
      break;
    }
    case OpCode::CLOSURE: {
      unsigned bx = op_bx(ins);
      if (bx >= p->protos.size())
        panic("CLOSURE: bad proto index");
      Proto* np = p->protos[bx];
      // PUC getcached: reuse last closure when every upvalue location matches.
      Closure* cached = np->cache;
      if (cached && cached->upvals.size() == np->upvalues.size()) {
        bool ok = true;
        for (size_t uv = 0; uv < np->upvalues.size(); ++uv) {
          auto& d = np->upvalues[uv];
          UpVal* have = cached->upvals[uv];
          if (!have) {
            ok = false;
            break;
          }
          if (d.instack) {
            if (!have->open || have->thread != th || have->stack_index != fbase() + d.idx) {
              ok = false;
              break;
            }
          } else {
            if (d.idx >= cl->upvals.size() || have != cl->upvals[d.idx]) {
              ok = false;
              break;
            }
          }
        }
        if (ok) {
          base = reload();
          base[a] = TValue::obj(ValueTag::Function, cached);
          break;
        }
      }
      Closure* ncl = closure_new_lua(L, np);
      for (size_t uv = 0; uv < np->upvalues.size(); ++uv) {
        auto& d = np->upvalues[uv];
        if (d.instack)
          ncl->upvals[uv] = L->find_upval(th, fbase() + d.idx);
        else {
          if (d.idx >= cl->upvals.size() || !cl->upvals[d.idx])
            panic("CLOSURE: bad parent upvalue");
          ncl->upvals[uv] = cl->upvals[d.idx];
        }
      }
      np->cache = ncl;
      base = reload();
      base[a] = TValue::obj(ValueTag::Function, ncl);
      break;
    }
    case OpCode::VARARG: {
      auto& fr = th->frames[fi];
      const int nvar = static_cast<int>(fr.varargs.size());
      int nwant = (b == 0) ? nvar : (b - 1);
      L->ensure_stack(fr.base + a + nwant + 8);
      base = reload();
      for (int i = 0; i < nwant; ++i) {
        if (i < nvar)
          base[a + i] = fr.varargs[static_cast<size_t>(i)];
        else
          base[a + i] = TValue::nil();
      }
      if (b == 0)
        th->top = fr.base + a + nwant; // no nil-fill; slots already written
      break;
    }
    case OpCode::CALL: {
      int nargs = (b == 0) ? (L->abs_top() - (fbase() + a) - 1) : (b - 1);
      if (nargs < 0)
        nargs = 0;
      int nret = (c == 0) ? LUA_MULTRET : (c - 1);
      int call_base = fbase() + a;
      // Raise top without nil-filling: args are already in R[A+1..] (nil-fill would wipe them).
      L->ensure_stack(call_base + 1 + nargs + 8);
      th->top = call_base + 1 + nargs;
      base = reload();
      TValue f = base[a];
      if (f.is_function()) {
        if (f.as_closure()->is_c) {
          int st = run_c_call(L, f.as_closure(), call_base, nargs, nret);
          if (st == LUA_YIELD)
            return LUA_YIELD;
        } else {
          push_lua_frame(L, f.as_closure(), call_base, nargs, nret);
        }
      } else {
        meta_call(L, call_base, nargs, nret == LUA_MULTRET ? 1 : nret);
      }
      break;
    }
    case OpCode::TAILCALL: {
      int nargs = (b == 0) ? (L->abs_top() - (fbase() + a) - 1) : (b - 1);
      if (nargs < 0)
        nargs = 0;
      int nresults = th->frames[fi].expected_results;
      int dest = fbase();
      int call_base = dest + a;
      // Snapshot func+args first so close/slide cannot clobber them, and so open
      // upvalues still see the caller's original locals when closed (PUC order:
      // close before sliding the new frame into the caller's slots).
      std::vector<TValue> call_vals(static_cast<size_t>(nargs + 1));
      for (int i = 0; i <= nargs; ++i)
        call_vals[static_cast<size_t>(i)] = th->stack[static_cast<size_t>(call_base + i)];
      TValue callee = call_vals[0];
      const char* tail_name_ptr = nullptr;
      debug_funcnamefromcode(&th->frames[fi], &tail_name_ptr);
      std::string tail_name = tail_name_ptr ? tail_name_ptr : "";
      L->close_upvals(th, dest);
      L->ensure_stack(dest + 1 + nargs + 8);
      for (int i = 0; i <= nargs; ++i)
        th->stack[static_cast<size_t>(dest + i)] = call_vals[static_cast<size_t>(i)];
      th->top = dest + 1 + nargs;
      // Proper tail call: replace the frame; no return hook for the caller.
      th->frames.pop_back();
      debug_on_return(L, th);
      if (callee.is_function()) {
        if (callee.as_closure()->is_c) {
          int st = run_c_call(L, callee.as_closure(), dest, nargs, nresults, true,
                              tail_name.empty() ? nullptr : tail_name.c_str());
          if (st == LUA_YIELD)
            return LUA_YIELD;
          if (static_cast<int>(th->frames.size()) < entry_depth)
            return LUA_OK;
        } else {
          push_lua_frame(L, callee.as_closure(), dest, nargs, nresults, true);
        }
      } else {
        meta_call(L, dest, nargs, nresults == LUA_MULTRET ? 1 : nresults);
        if (static_cast<int>(th->frames.size()) < entry_depth)
          return LUA_OK;
      }
      break;
    }
    case OpCode::RETURN: {
      auto& fr = th->frames[fi];
      int nret = (b == 0) ? (L->abs_top() - (fr.base + a)) : (b - 1);
      if (nret < 0)
        nret = 0;
      L->close_upvals(th, fr.base);
      int dest = fr.base;
      int want = fr.expected_results;
      std::vector<TValue> results(static_cast<size_t>(nret));
      // Use absolute indices: `base` can be stale after stack reallocation.
      for (int i = 0; i < nret; ++i)
        results[static_cast<size_t>(i)] =
            th->stack[static_cast<size_t>(fr.base + a + i)];
      debug_return_hook(L, th);
      th->frames.pop_back();
      debug_on_return(L, th);
      // Do not use set_abs_top to grow: when top sits low after a 1-result CALL,
      // growing would nil-fill over the results we are about to (or just) write.
      int placed = (want == LUA_MULTRET) ? nret : want;
      if (placed < 0)
        placed = 0;
      L->ensure_stack(dest + placed + 8);
      for (int i = 0; i < placed; ++i)
        th->stack[static_cast<size_t>(dest + i)] =
            (i < nret) ? results[static_cast<size_t>(i)] : TValue::nil();
      th->top = dest + placed;
      if (static_cast<int>(th->frames.size()) < entry_depth)
        return LUA_OK;
      break;
    }
    case OpCode::CHECKGC:
    case OpCode::SAFEPOINT:
      L->gc.safepoint();
      break;
    default:
      panic(std::string("unimplemented opcode ") + opcode_name(op));
    }
  }
  return LUA_OK;
}

namespace {

void place_abs_results(State* L, int dest, int want, const std::vector<TValue>& results) {
  Thread* th = L->current;
  int nret = static_cast<int>(results.size());
  int placed = (want == LUA_MULTRET) ? nret : want;
  if (placed < 0)
    placed = 0;
  L->ensure_stack(dest + placed + 8);
  for (int i = 0; i < placed; ++i)
    th->stack[static_cast<size_t>(dest + i)] =
        (i < nret) ? results[static_cast<size_t>(i)] : TValue::nil();
  th->top = dest + placed;
}

int finish_continue_frame(State* L, bool ok, const char* err_msg) {
  Thread* th = L->current;
  if (th->frames.empty())
    return LUA_OK;
  CallFrame cfr = th->frames.back();
  if (cfr.kind != FrameKind::Continue || cfr.cont_kind == CallFrame::ContKind::None)
    return LUA_OK;
  th->frames.pop_back();

  std::vector<TValue> outs;
  if (ok) {
    int base = cfr.cont_res_base;
    int nret = th->top - base;
    if (nret < 0)
      nret = 0;
    if (cfr.cont_kind != CallFrame::ContKind::DoFile)
      outs.push_back(TValue::boolean(true));
    for (int i = 0; i < nret; ++i)
      outs.push_back(th->stack[static_cast<size_t>(base + i)]);
  } else {
    outs.push_back(TValue::boolean(false));
    TValue emsg;
    if (th->err_obj_set) {
      emsg = th->err_obj;
      th->err_obj_set = false;
    } else {
      emsg = TValue::obj(ValueTag::String, L->intern(err_msg ? err_msg : ""));
    }
    if (cfr.cont_kind == CallFrame::ContKind::XPCall && cfr.cont_ctx.is_function()) {
      try {
        int slot = th->top;
        L->ensure_stack(slot + 4);
        th->stack[static_cast<size_t>(slot)] = cfr.cont_ctx;
        th->stack[static_cast<size_t>(slot + 1)] = emsg;
        th->top = slot + 2;
        th->stack_base = 0;
        int st = call_closure(L, cfr.cont_ctx.as_closure(), 1, 1);
        if (st == LUA_YIELD)
          return LUA_YIELD;
        emsg = (th->top > slot) ? th->stack[static_cast<size_t>(slot)] : TValue::nil();
      } catch (const LuatierError& e) {
        if (th->err_obj_set) {
          emsg = th->err_obj;
          th->err_obj_set = false;
        } else {
          emsg = TValue::obj(ValueTag::String, L->intern(e.what()));
        }
      }
    }
    outs.push_back(emsg);
  }
  place_abs_results(L, cfr.base, cfr.expected_results, outs);
  return LUA_OK;
}

} // namespace

int resume_after_yield(State* L, bool ok, const char* err_msg) {
  Thread* th = L->current;
  if (!ok) {
    while (!th->frames.empty()) {
      auto& fr = th->frames.back();
      if (fr.kind == FrameKind::Continue)
        break;
      if (fr.kind == FrameKind::CApi && !fr.proto) {
        th->frames.pop_back();
        continue;
      }
      L->close_upvals(th, fr.base);
      th->frames.pop_back();
    }
    if (!th->frames.empty() && th->frames.back().kind == FrameKind::Continue) {
      int st = finish_continue_frame(L, false, err_msg);
      if (st == LUA_YIELD)
        return LUA_YIELD;
    }
    // Outer Continue frames (e.g. xpcall around pcall) see the false+msg
    // tuple as a normal protected-call result.
  }

  for (;;) {
    while (!th->frames.empty() && th->frames.back().kind == FrameKind::Continue) {
      int st = finish_continue_frame(L, true, nullptr);
      if (st == LUA_YIELD)
        return LUA_YIELD;
    }
    if (th->frames.empty())
      return LUA_OK;
    if (th->frames.back().kind == FrameKind::CApi || !th->frames.back().proto)
      return LUA_OK;
    int st = interpret(L, 1);
    if (st == LUA_YIELD)
      return LUA_YIELD;
    if (th->frames.empty() || th->frames.back().kind != FrameKind::Continue)
      return LUA_OK;
  }
}

} // namespace luatier
