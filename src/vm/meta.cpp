#include "vm/meta.hpp"

#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/userdata.hpp"
#include "vm/interpreter.hpp"
#include "vm/state.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace luatier {

// PUC l_strcmp: locale-aware via strcoll, segment-wise across embedded NULs.
static int string_cmp(const LjString* ls, const LjString* rs) {
  const char* l = ls->data;
  size_t ll = ls->len;
  const char* r = rs->data;
  size_t lr = rs->len;
  for (;;) {
    int temp = std::strcoll(l, r);
    if (temp != 0)
      return temp;
    size_t len = std::strlen(l);
    if (len == lr)
      return (len == ll) ? 0 : 1;
    if (len == ll)
      return -1;
    ++len;
    l += len;
    ll -= len;
    r += len;
    lr -= len;
  }
}

Table* get_metatable(State* L, const TValue& v) {
  if (v.is_table())
    return v.as_table()->metatable;
  if (v.is_userdata())
    return v.as_userdata()->metatable;
  ValueTag t = v.tag();
  // Int/Float share one "number" metatable (stored in the Float slot).
  if (t == ValueTag::Int)
    t = ValueTag::Float;
  size_t i = static_cast<size_t>(t);
  if (L && i < L->type_mt.size())
    return L->type_mt[i];
  return nullptr;
}

void set_metatable(State* L, TValue& v, Table* mt) {
  if (!v.is_table())
    panic("setmetatable: value is not a table");
  v.as_table()->metatable = mt;
  v.as_table()->update_weak_mode(L);
  if (mt)
    L->gc.barrier(v.as_gc(), TValue::obj(ValueTag::Table, mt));
}

TValue get_metamethod(State* L, const TValue& obj, const char* name) {
  Table* mt = get_metatable(L, obj);
  if (!mt)
    return TValue::nil();
  return mt->get(TValue::obj(ValueTag::String, L->intern(name)));
}

int64_t table_length(Table* t) {
  // Lua 5.3 border: any integer index such that t[n] ~= nil and t[n+1] == nil.
  // Prefer array part; then extend into hash.
  int64_t n = static_cast<int64_t>(t->array.size());
  while (n > 0 && t->get_int(n).is_nil())
    --n;
  if (n < static_cast<int64_t>(t->array.size()))
    return n;
  // grow while consecutive integers exist in hash
  for (;;) {
    if (t->get_int(n + 1).is_nil())
      return n;
    ++n;
    if (n > 10000000)
      return n; // safety
  }
}

// Like debug_call_hook: `th->top` may sit inside a Lua frame's maxstack after a
// CALL, so metamethod calls must start above all live register windows.
static int live_stack_top(Thread* th) {
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

static int call_mm(State* L, const TValue& mm, const TValue* args, int nargs, TValue* out,
                   int nout) {
  Thread* th = L->current;
  const int saved_top = th->top;
  const int call_base = live_stack_top(th);
  L->ensure_stack(call_base + 1 + nargs + nout + 8);
  th->stack[static_cast<size_t>(call_base)] = mm;
  for (int i = 0; i < nargs; ++i)
    th->stack[static_cast<size_t>(call_base + 1 + i)] = args[i];
  th->top = call_base + 1 + nargs;
  int st = call_closure(L, mm.as_closure(), nargs, nout);
  if (st != 0) {
    th->top = saved_top;
    return st;
  }
  for (int i = 0; i < nout; ++i) {
    if (call_base + i < th->top)
      out[i] = th->stack[static_cast<size_t>(call_base + i)];
    else
      out[i] = TValue::nil();
  }
  th->top = saved_top;
  return 0;
}

// PUC MAXTAGLOOP — avoid infinite __index / __newindex table chains.
constexpr int kMaxTagLoop = 2000;

TValue meta_index(State* L, const TValue& table, const TValue& key) {
  TValue t = table;
  for (int loop = 0; loop < kMaxTagLoop; ++loop) {
    if (t.is_table()) {
      TValue v = t.as_table()->get(key);
      if (!v.is_nil())
        return v;
      TValue mm = get_metamethod(L, t, "__index");
      if (mm.is_nil())
        return TValue::nil();
      if (mm.is_function()) {
        TValue args[2] = {t, key};
        TValue out;
        call_mm(L, mm, args, 2, &out, 1);
        return out;
      }
      t = mm; // table (or other) metamethod — continue chain
      continue;
    }
    TValue mm = get_metamethod(L, t, "__index");
    if (mm.is_nil())
      panic("attempt to index a non-table value");
    if (mm.is_function()) {
      TValue args[2] = {t, key};
      TValue out;
      call_mm(L, mm, args, 2, &out, 1);
      return out;
    }
    t = mm;
  }
  panic("'__index' chain too long; possible loop");
}

void meta_newindex(State* L, const TValue& table, const TValue& key, const TValue& value) {
  TValue t = table;
  for (int loop = 0; loop < kMaxTagLoop; ++loop) {
    if (t.is_table()) {
      TValue cur = t.as_table()->get(key);
      if (!cur.is_nil()) {
        t.as_table()->set(L, key, value);
        return;
      }
      TValue mm = get_metamethod(L, t, "__newindex");
      if (mm.is_nil()) {
        t.as_table()->set(L, key, value);
        return;
      }
      if (mm.is_function()) {
        TValue args[3] = {t, key, value};
        TValue out;
        call_mm(L, mm, args, 3, &out, 0);
        return;
      }
      t = mm;
      continue;
    }
    TValue mm = get_metamethod(L, t, "__newindex");
    if (mm.is_nil())
      panic("attempt to index a non-table value");
    if (mm.is_function()) {
      TValue args[3] = {t, key, value};
      TValue out;
      call_mm(L, mm, args, 3, &out, 0);
      return;
    }
    t = mm;
  }
  panic("'__newindex' chain too long; possible loop");
}

bool meta_arith(State* L, const char* mt_name, const TValue& a, const TValue& b, TValue* out) {
  TValue mm = get_metamethod(L, a, mt_name);
  if (mm.is_nil())
    mm = get_metamethod(L, b, mt_name);
  if (mm.is_nil() || !mm.is_function())
    return false;
  TValue args[2] = {a, b};
  call_mm(L, mm, args, 2, out, 1);
  return true;
}

bool meta_unary(State* L, const char* mt_name, const TValue& a, TValue* out) {
  TValue mm = get_metamethod(L, a, mt_name);
  if (mm.is_nil() || !mm.is_function())
    return false;
  TValue args[1] = {a};
  call_mm(L, mm, args, 1, out, 1);
  return true;
}

bool meta_eq(State* L, const TValue& a, const TValue& b, bool* out_eq) {
  if (a.tag() != b.tag() && !(a.is_number() && b.is_number())) {
    *out_eq = false;
    return true;
  }
  if (a.is_table() || a.is_function() || a.tag() == ValueTag::Userdata) {
    if (a.payload == b.payload) {
      *out_eq = true;
      return true;
    }
    // PUC 5.3: use __eq from a, else from b (either side is enough).
    TValue mm = get_metamethod(L, a, "__eq");
    if (mm.is_nil())
      mm = get_metamethod(L, b, "__eq");
    if (mm.is_nil() || !mm.is_function()) {
      *out_eq = false;
      return true;
    }
    TValue args[2] = {a, b};
    TValue out;
    call_mm(L, mm, args, 2, &out, 1);
    *out_eq = out.is_truthy();
    return true;
  }
  *out_eq = values_equal(a, b);
  return true;
}

bool meta_lt(State* L, const TValue& a, const TValue& b, bool* out_lt) {
  TValue na, nb;
  if (try_to_number(a, &na) && try_to_number(b, &nb)) {
    *out_lt = na.to_number() < nb.to_number();
    return true;
  }
  if (a.is_string() && b.is_string()) {
    *out_lt = string_cmp(a.as_string(), b.as_string()) < 0;
    return true;
  }
  TValue mm = get_metamethod(L, a, "__lt");
  if (mm.is_nil())
    mm = get_metamethod(L, b, "__lt");
  if (mm.is_nil() || !mm.is_function())
    panic("attempt to compare incompatible types");
  TValue args[2] = {a, b};
  TValue out;
  call_mm(L, mm, args, 2, &out, 1);
  *out_lt = out.is_truthy();
  return true;
}

bool meta_le(State* L, const TValue& a, const TValue& b, bool* out_le) {
  TValue na, nb;
  if (try_to_number(a, &na) && try_to_number(b, &nb)) {
    *out_le = na.to_number() <= nb.to_number();
    return true;
  }
  if (a.is_string() && b.is_string()) {
    *out_le = string_cmp(a.as_string(), b.as_string()) <= 0;
    return true;
  }
  TValue mm = get_metamethod(L, a, "__le");
  if (mm.is_nil())
    mm = get_metamethod(L, b, "__le");
  if (!mm.is_nil() && mm.is_function()) {
    TValue args[2] = {a, b};
    TValue out;
    call_mm(L, mm, args, 2, &out, 1);
    *out_le = out.is_truthy();
    return true;
  }
  // fallback: not (b < a)
  bool lt = false;
  meta_lt(L, b, a, &lt);
  *out_le = !lt;
  return true;
}

bool meta_concat(State* L, const TValue& a, const TValue& b, TValue* out) {
  if ((a.is_string() || a.is_number()) && (b.is_string() || b.is_number())) {
    *out = TValue::obj(ValueTag::String, L->intern(value_to_string(a) + value_to_string(b)));
    return true;
  }
  TValue mm = get_metamethod(L, a, "__concat");
  if (mm.is_nil())
    mm = get_metamethod(L, b, "__concat");
  if (mm.is_nil() || !mm.is_function())
    panic("attempt to concatenate incompatible types");
  TValue args[2] = {a, b};
  call_mm(L, mm, args, 2, out, 1);
  return true;
}

bool meta_len(State* L, const TValue& a, TValue* out) {
  if (a.is_string()) {
    *out = TValue::integer(static_cast<int64_t>(a.as_string()->len));
    return true;
  }
  if (a.is_table()) {
    TValue mm = get_metamethod(L, a, "__len");
    if (!mm.is_nil() && mm.is_function()) {
      TValue args[1] = {a};
      call_mm(L, mm, args, 1, out, 1);
      return true;
    }
    *out = TValue::integer(table_length(a.as_table()));
    return true;
  }
  TValue mm = get_metamethod(L, a, "__len");
  if (mm.is_nil() || !mm.is_function())
    panic("attempt to get length of incompatible type");
  TValue args[1] = {a};
  call_mm(L, mm, args, 1, out, 1);
  return true;
}

int meta_call(State* L, int func_idx, int nargs, int nresults) {
  TValue f = L->current->stack[static_cast<size_t>(func_idx)];
  if (f.is_function())
    return call_closure(L, f.as_closure(), nargs, nresults);
  TValue mm = get_metamethod(L, f, "__call");
  if (!mm.is_function())
    panic("attempt to call a non-function value");
  // Insert mm before func: shift args
  L->ensure_stack(func_idx + 2 + nargs);
  for (int i = nargs; i >= 0; --i)
    L->current->stack[static_cast<size_t>(func_idx + 1 + i)] =
        L->current->stack[static_cast<size_t>(func_idx + i)];
  L->current->stack[static_cast<size_t>(func_idx)] = mm;
  L->current->top = func_idx + 2 + nargs;
  return call_closure(L, mm.as_closure(), nargs + 1, nresults);
}

} // namespace luatier
