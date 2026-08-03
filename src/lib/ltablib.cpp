#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "vm/interpreter.hpp"
#include "vm/meta.hpp"

#include <algorithm>
#include <vector>

namespace luatier {
using namespace lib;

static int64_t aux_getn(State* L, const TValue& t) {
  TValue len;
  if (!meta_len(L, t, &len))
    panic("attempt to get length of a " + std::string(value_to_string(t)) + " value");
  if (len.is_int())
    return len.as_int();
  if (len.is_float()) {
    double d = len.as_float();
    if (d == std::floor(d) && d >= static_cast<double>(INT64_MIN) &&
        d <= static_cast<double>(INT64_MAX))
      return static_cast<int64_t>(d);
  }
  panic("object length is not an integer");
}

static TValue geti(State* L, const TValue& t, int64_t i) {
  return meta_index(L, t, TValue::integer(i));
}

static void seti(State* L, const TValue& t, int64_t i, const TValue& v) {
  meta_newindex(L, t, TValue::integer(i), v);
}

static void concat_addfield(State* L, const TValue& t, int64_t i, std::string& out) {
  TValue v = geti(L, t, i);
  // PUC lua_isstring: strings and numbers are accepted.
  if (v.is_string()) {
    out.append(v.as_string()->view());
    return;
  }
  if (v.is_number()) {
    out += value_to_string(v);
    return;
  }
  const char* tn = "nil";
  if (v.is_bool())
    tn = "boolean";
  else if (v.is_table())
    tn = "table";
  else if (v.is_function())
    tn = "function";
  else if (v.is_userdata())
    tn = "userdata";
  else if (v.is_thread())
    tn = "thread";
  panic(std::string("invalid value (") + tn + ") at index " + std::to_string(i) +
        " in table for 'concat'");
}

static int tab_concat(State* L) {
  TValue t = *L->at(1);
  if (!t.is_table())
    panic("table expected");
  std::string sep = (L->gettop() >= 2 && !L->at(2)->is_nil())
                        ? std::string(check_string(L, 2)->view())
                        : "";
  int64_t i = opt_int(L, 3, 1);
  int64_t last = opt_int(L, 4, aux_getn(L, t));
  std::string out;
  for (; i < last; ++i) {
    concat_addfield(L, t, i, out);
    out += sep;
  }
  if (i == last)
    concat_addfield(L, t, i, out);
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int tab_insert(State* L) {
  TValue t = *L->at(1);
  if (!t.is_table())
    panic("table expected");
  if (L->gettop() == 2) {
    int64_t pos = aux_getn(L, t) + 1;
    seti(L, t, pos, *L->at(2));
  } else if (L->gettop() == 3) {
    int64_t pos = check_int(L, 2);
    int64_t e = aux_getn(L, t) + 1;
    if (pos < 1 || pos > e)
      panic("position out of bounds");
    for (int64_t k = e; k > pos; --k)
      seti(L, t, k, geti(L, t, k - 1));
    seti(L, t, pos, *L->at(3));
  } else {
    panic("wrong number of arguments to 'insert'");
  }
  return 0;
}

static int tab_remove(State* L) {
  TValue t = *L->at(1);
  if (!t.is_table())
    panic("table expected");
  int64_t size = aux_getn(L, t);
  int64_t pos = L->gettop() >= 2 ? check_int(L, 2) : size;
  if (pos != size) {
    if (pos < 1 || pos > size + 1)
      panic("position out of bounds");
  }
  TValue v = geti(L, t, pos);
  for (int64_t k = pos; k < size; ++k)
    seti(L, t, k, geti(L, t, k + 1));
  if (pos >= 1 && pos <= size)
    seti(L, t, size, TValue::nil());
  else if (pos == 0)
    seti(L, t, 0, TValue::nil());
  L->settop(0);
  L->push(v);
  return 1;
}

static int tab_move(State* L) {
  TValue a = *L->at(1);
  if (!a.is_table())
    panic("table expected");
  int64_t f = check_int(L, 2);
  int64_t e = check_int(L, 3);
  int64_t tpos = check_int(L, 4);
  TValue dest = L->gettop() >= 5 ? *L->at(5) : a;
  if (L->gettop() >= 5 && !dest.is_table())
    panic("table expected");
  if (e >= f) {
    if (values_equal(dest, a) && tpos > e) {
      for (int64_t i = e; i >= f; --i)
        seti(L, dest, tpos + (i - f), geti(L, a, i));
    } else {
      for (int64_t i = f; i <= e; ++i)
        seti(L, dest, tpos + (i - f), geti(L, a, i));
    }
  }
  L->settop(0);
  L->push(dest);
  return 1;
}

static int tab_pack(State* L) {
  int n = L->gettop();
  Table* t = table_new(L, static_cast<size_t>(n), 0);
  for (int i = 1; i <= n; ++i)
    t->set_int(L, i, *L->at(i));
  t->set(L, TValue::obj(ValueTag::String, L->intern("n")), TValue::integer(n));
  L->settop(0);
  L->push(TValue::obj(ValueTag::Table, t));
  return 1;
}

static int tab_unpack(State* L) {
  TValue t = *L->at(1);
  if (!t.is_table())
    panic("table expected");
  int64_t i = opt_int(L, 2, 1);
  int64_t j = opt_int(L, 3, aux_getn(L, t));
  L->settop(0);
  if (j < i)
    return 0;
  for (int64_t k = i; k <= j; ++k)
    L->push(geti(L, t, k));
  return static_cast<int>(j - i + 1);
}

struct SortCtx {
  State* L = nullptr;
  TValue cmp;
};

static int sort_compare(const TValue& a, const TValue& b, SortCtx* ctx) {
  if (ctx->cmp.is_nil()) {
    if (!a.is_number() || !b.is_number())
      panic("attempt to compare two non-number values");
    return a.to_number() < b.to_number() ? -1 : (a.to_number() > b.to_number() ? 1 : 0);
  }
  ctx->L->push(ctx->cmp);
  ctx->L->push(a);
  ctx->L->push(b);
  call_closure(ctx->L, ctx->cmp.as_closure(), 2, 1);
  bool lt = ctx->L->at(1)->is_truthy();
  ctx->L->settop(0);
  return lt ? -1 : 1;
}

static int tab_sort(State* L) {
  TValue t = *L->at(1);
  if (!t.is_table())
    panic("table expected");
  TValue cmp = L->gettop() >= 2 ? *L->at(2) : TValue::nil();
  int64_t n = aux_getn(L, t);
  std::vector<TValue> arr(static_cast<size_t>(n));
  for (int64_t i = 1; i <= n; ++i)
    arr[static_cast<size_t>(i - 1)] = geti(L, t, i);
  SortCtx ctx{L, cmp};
  std::stable_sort(arr.begin(), arr.end(), [&](const TValue& a, const TValue& b) {
    return sort_compare(a, b, &ctx) < 0;
  });
  for (int64_t i = 1; i <= n; ++i)
    seti(L, t, i, arr[static_cast<size_t>(i - 1)]);
  return 0;
}

void open_table_lib(State* L) {
  Table* tab = new_lib(L, 8);
  set_field(L, tab, "concat", tab_concat);
  set_field(L, tab, "insert", tab_insert);
  set_field(L, tab, "remove", tab_remove);
  set_field(L, tab, "move", tab_move);
  set_field(L, tab, "pack", tab_pack);
  set_field(L, tab, "unpack", tab_unpack);
  set_field(L, tab, "sort", tab_sort);
  set_global_value(L, "table", TValue::obj(ValueTag::Table, tab));
}

} // namespace luatier
