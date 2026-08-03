#include "vm/interpreter.hpp"

#include "jit/hotness.hpp"
#include "runtime/string.hpp"
#include "tools/profile.hpp"
#include "vm/debug_hook.hpp"
#include "vm/meta.hpp"

#include <cmath>
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

namespace lj3 {

namespace {

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
    double d = v.as_float();
    if (std::floor(d) == d && d >= static_cast<double>(INT64_MIN) &&
        d <= static_cast<double>(INT64_MAX)) {
      *out = static_cast<int64_t>(d);
      return true;
    }
    return false;
  }
  return false;
}

TValue coerce_number(const TValue& v) {
  TValue n;
  if (try_to_number(v, &n))
    return n;
  if (v.is_string())
    panic("attempt to perform arithmetic on a string value");
  panic("attempt to perform arithmetic on non-number");
}

TValue arith_raw(OpCode op, const TValue& a_in, const TValue& b_in) {
  TValue a = coerce_number(a_in);
  TValue b = coerce_number(b_in);
  if (op == OpCode::BAND || op == OpCode::BOR || op == OpCode::BXOR || op == OpCode::SHL ||
      op == OpCode::SHR) {
    int64_t x, y;
    if (!to_integer(a, &x) || !to_integer(b, &y))
      panic("number has no integer representation");
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
  if (!a.is_number() || !b.is_number())
    panic("attempt to perform arithmetic on non-number");
  double x = a.to_number(), y = b.to_number(), r = 0;
  switch (op) {
  case OpCode::ADD: r = x + y; break;
  case OpCode::SUB: r = x - y; break;
  case OpCode::MUL: r = x * y; break;
  case OpCode::DIV: r = x / y; break;
  case OpCode::IDIV: r = std::floor(x / y); break;
  case OpCode::MOD: r = x - std::floor(x / y) * y; break;
  case OpCode::POW: r = std::pow(x, y); break;
  default: panic("bad arith");
  }
  if (op == OpCode::IDIV && a.is_int() && b.is_int())
    return TValue::integer(static_cast<int64_t>(r));
  return TValue::number(r);
}

TValue do_arith(State* L, OpCode op, const TValue& a, const TValue& b) {
  TValue na, nb;
  if (try_to_number(a, &na) && try_to_number(b, &nb))
    return arith_raw(op, na, nb);
  if (!(a.is_number() && b.is_number())) {
    TValue out;
    const char* mt = arith_mt(op);
    if (mt && meta_arith(L, mt, a, b, &out))
      return out;
  }
  return arith_raw(op, a, b);
}

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
               bool is_tailcall = false) {
  Thread* th = L->current;
  if (func_idx < 0 || nargs < 0)
    panic("run_c_call: bad func_idx/nargs");
  if (static_cast<int>(th->stack.size()) < func_idx + 1 + nargs)
    panic("run_c_call: stack underflow");

  // Push a C frame so debug levels match PUC (level 0 = C, level 1 = Lua caller).
  CallFrame cfr;
  cfr.cl = cl;
  cfr.proto = nullptr;
  cfr.base = func_idx;
  cfr.kind = FrameKind::CApi;
  cfr.tailcall = is_tailcall;
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
  int nret = 0;
  try {
    nret = cl->cfunc(L);
  } catch (const Lj3Error&) {
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
    th->yield_vals = std::move(results);
    th->yield_func_idx = func_idx;
    th->yield_nresults = nresults;
    th->stack_base = prev_base;
    L->ensure_stack(func_idx + 8);
    th->top = func_idx;
    L->yield_pending = false;
    return LUA_YIELD;
  }

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

} // namespace

int call_closure(State* L, Closure* cl, int nargs, int nresults) {
  int func_idx = L->current->top - nargs - 1;
  if (func_idx < 0)
    panic("call_closure stack underflow");
  if (cl->is_c)
    return run_c_call(L, cl, func_idx, nargs, nresults);
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
    runtime_profile().opcodes++;

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
      TValue v = meta_index(L, t, key);
      base = reload();
      base[a] = v;
      break;
    }
    case OpCode::SETTABUP: {
      TValue t = cl->upvals[static_cast<size_t>(a)]->get();
      TValue key = p->constants[static_cast<size_t>(b)];
      TValue val = base[c];
      meta_newindex(L, t, key, val);
      (void)reload();
      break;
    }
    case OpCode::GETTABLE: {
      TValue v = meta_index(L, base[b], base[c]);
      base = reload();
      base[a] = v;
      break;
    }
    case OpCode::SETTABLE: {
      TValue t = base[a], key = base[b], val = base[c];
      meta_newindex(L, t, key, val);
      (void)reload();
      break;
    }
    case OpCode::GETI: {
      TValue v = meta_index(L, base[b], TValue::integer(c));
      base = reload();
      base[a] = v;
      break;
    }
    case OpCode::SETI: {
      TValue t = base[a], val = base[c];
      meta_newindex(L, t, TValue::integer(b), val);
      (void)reload();
      break;
    }
    case OpCode::GETFIELD: {
      TValue v = meta_index(L, base[b], p->constants[static_cast<size_t>(c)]);
      base = reload();
      base[a] = v;
      break;
    }
    case OpCode::SETFIELD: {
      TValue t = base[a], val = base[c];
      meta_newindex(L, t, p->constants[static_cast<size_t>(b)], val);
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
      TValue m = meta_index(L, t, key);
      base = reload();
      base[a + 1] = t;
      base[a] = m;
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
      TValue r = do_arith(L, op, lhs, rhs);
      base = reload();
      base[a] = r;
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
        TValue out;
        if (!meta_unary(L, "__unm", base[b], &out))
          panic("attempt to perform arithmetic on non-number");
        base = reload();
        base[a] = out;
      }
      break;
    }
    case OpCode::BNOT: {
      int64_t x;
      if (to_integer(base[b], &x))
        base[a] = TValue::integer(~x);
      else {
        TValue out;
        if (!meta_unary(L, "__bnot", base[b], &out))
          panic("number has no integer representation");
        base = reload();
        base[a] = out;
      }
      break;
    }
    case OpCode::NOT:
      base[a] = TValue::boolean(!base[b].is_truthy());
      break;
    case OpCode::LEN: {
      TValue out;
      meta_len(L, base[b], &out);
      base = reload();
      base[a] = out;
      break;
    }
    case OpCode::CONCAT: {
      TValue acc = base[b];
      for (int r = b + 1; r <= c; ++r) {
        base = reload();
        TValue out;
        meta_concat(L, acc, base[r], &out);
        acc = out;
      }
      base = reload();
      base[a] = acc;
      break;
    }
    case OpCode::EQ: {
      bool eq = false;
      meta_eq(L, base[b], base[c], &eq);
      (void)reload();
      if (eq != (a != 0))
        pc()++;
      break;
    }
    case OpCode::LT: {
      bool lt = false;
      meta_lt(L, base[b], base[c], &lt);
      (void)reload();
      if (lt != (a != 0))
        pc()++;
      break;
    }
    case OpCode::LE: {
      bool le = false;
      meta_le(L, base[b], base[c], &le);
      (void)reload();
      if (le != (a != 0))
        pc()++;
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
      if (a != 0)
        L->close_upvals(th, fbase() + (a - 1));
      pc() += op_sbx(ins);
      hotness_on_loop(p);
      break;
    }
    case OpCode::FORPREP: {
      if (!base[a].is_number() || !base[a + 1].is_number() || !base[a + 2].is_number())
        panic("'for' initial/limit/step must be a number");
      // Lua 5.3: if all of init/limit/step are integers, the loop uses integers.
      if (base[a].is_int() && base[a + 1].is_int() && base[a + 2].is_int()) {
        int64_t idx = base[a].as_int();
        int64_t step = base[a + 2].as_int();
        base[a] = TValue::integer(idx - step);
      } else {
        double idx = base[a].to_number();
        double step = base[a + 2].to_number();
        base[a] = TValue::number(idx - step);
      }
      pc() += op_sbx(ins);
      break;
    }
    case OpCode::FORLOOP: {
      if (base[a].is_int() && base[a + 1].is_int() && base[a + 2].is_int()) {
        int64_t step = base[a + 2].as_int();
        int64_t idx = base[a].as_int() + step;
        int64_t limit = base[a + 1].as_int();
        if ((step > 0) ? (idx <= limit) : (idx >= limit)) {
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
      if (!f.is_function())
        meta_call(L, cb, 2, c);
      else
        call_closure(L, f.as_closure(), 2, c);
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
          int st = run_c_call(L, callee.as_closure(), dest, nargs, nresults, true);
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

} // namespace lj3
