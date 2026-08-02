#include "vm/meta.hpp"

#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/userdata.hpp"
#include "vm/interpreter.hpp"
#include "vm/state.hpp"

#include <cmath>

namespace lj3 {

Table* get_metatable(const TValue& v) {
  if (v.is_table())
    return v.as_table()->metatable;
  if (v.is_userdata())
    return v.as_userdata()->metatable;
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
  Table* mt = get_metatable(obj);
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

static int call_mm(State* L, const TValue& mm, const TValue* args, int nargs, TValue* out,
                   int nout) {
  int base = L->gettop();
  L->push(mm);
  for (int i = 0; i < nargs; ++i)
    L->push(args[i]);
  int st = call_closure(L, mm.as_closure(), nargs, nout);
  if (st != 0)
    return st;
  for (int i = 0; i < nout; ++i) {
    if (base + i < L->gettop())
      out[i] = L->current->stack[static_cast<size_t>(base + i)];
    else
      out[i] = TValue::nil();
  }
  L->settop(base);
  return 0;
}

TValue meta_index(State* L, const TValue& table, const TValue& key) {
  if (table.is_table()) {
    TValue v = table.as_table()->get(key);
    if (!v.is_nil())
      return v;
  }
  TValue mm = get_metamethod(L, table, "__index");
  if (mm.is_nil()) {
    if (!table.is_table())
      panic("attempt to index a non-table value");
    return TValue::nil();
  }
  if (mm.is_function()) {
    TValue args[2] = {table, key};
    TValue out;
    call_mm(L, mm, args, 2, &out, 1);
    return out;
  }
  if (mm.is_table())
    return meta_index(L, mm, key);
  panic("invalid __index metamethod");
}

void meta_newindex(State* L, const TValue& table, const TValue& key, const TValue& value) {
  if (table.is_table()) {
    TValue cur = table.as_table()->get(key);
    if (!cur.is_nil() || !table.as_table()->metatable) {
      table.as_table()->set(L, key, value);
      return;
    }
    // key absent -- check __newindex
  }
  TValue mm = get_metamethod(L, table, "__newindex");
  if (mm.is_nil()) {
    if (!table.is_table())
      panic("attempt to index a non-table value");
    table.as_table()->set(L, key, value);
    return;
  }
  if (mm.is_function()) {
    TValue args[3] = {table, key, value};
    TValue out;
    call_mm(L, mm, args, 3, &out, 0);
    return;
  }
  if (mm.is_table()) {
    meta_newindex(L, mm, key, value);
    return;
  }
  panic("invalid __newindex metamethod");
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
    TValue mm = get_metamethod(L, a, "__eq");
    TValue mm2 = get_metamethod(L, b, "__eq");
    if (mm.is_nil() || mm.payload != mm2.payload || !mm.is_function()) {
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
    *out_lt = a.as_string()->view() < b.as_string()->view();
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
    *out_le = a.as_string()->view() <= b.as_string()->view();
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

} // namespace lj3
