#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "vm/interpreter.hpp"
#include "vm/meta.hpp"

#include <algorithm>
#include <vector>

namespace lj3 {
using namespace lib;

static void concat_addfield(Table* t, int64_t i, std::string& out) {
  TValue v = t->get_int(i);
  if (!v.is_string()) {
    const char* tn = "nil";
    if (v.is_bool())
      tn = "boolean";
    else if (v.is_number())
      tn = "number";
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
  out.append(v.as_string()->view());
}

static int tab_concat(State* L) {
  Table* t = check_table(L, 1);
  std::string sep = L->gettop() >= 2 ? std::string(check_string(L, 2)->view()) : "";
  int64_t i = L->gettop() >= 3 ? check_int(L, 3) : 1;
  int64_t last = L->gettop() >= 4 ? check_int(L, 4) : table_length(t);
  std::string out;
  // PUC: i < last then optional last element — avoids ++ overflowing maxinteger.
  for (; i < last; ++i) {
    concat_addfield(t, i, out);
    out += sep;
  }
  if (i == last)
    concat_addfield(t, i, out);
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int tab_insert(State* L) {
  Table* t = check_table(L, 1);
  if (L->gettop() == 2) {
    int64_t n = table_length(t) + 1;
    t->set_int(L, n, *L->at(2));
  } else {
    int64_t pos = check_int(L, 2);
    int64_t n = table_length(t);
    for (int64_t k = n; k >= pos; --k)
      t->set_int(L, k + 1, t->get_int(k));
    t->set_int(L, pos, *L->at(3));
  }
  return 0;
}

static int tab_remove(State* L) {
  Table* t = check_table(L, 1);
  int64_t pos = L->gettop() >= 2 ? check_int(L, 2) : table_length(t);
  TValue v = t->get_int(pos);
  int64_t n = table_length(t);
  for (int64_t k = pos; k < n; ++k)
    t->set_int(L, k, t->get_int(k + 1));
  t->set_int(L, n, TValue::nil());
  L->settop(0);
  L->push(v);
  return 1;
}

static int tab_move(State* L) {
  Table* a = check_table(L, 1);
  int64_t f = check_int(L, 2);
  int64_t e = check_int(L, 3);
  int64_t t = check_int(L, 4);
  Table* dest = L->gettop() >= 5 ? check_table(L, 5) : a;
  if (e >= f) {
    if (dest == a && t > e) {
      for (int64_t i = e; i >= f; --i)
        dest->set_int(L, t + (i - f), a->get_int(i));
    } else {
      for (int64_t i = f; i <= e; ++i)
        dest->set_int(L, t + (i - f), a->get_int(i));
    }
  }
  L->settop(0);
  L->push(TValue::obj(ValueTag::Table, dest));
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
  Table* t = check_table(L, 1);
  int64_t i = L->gettop() >= 2 ? check_int(L, 2) : 1;
  int64_t j = L->gettop() >= 3 ? check_int(L, 3) : table_length(t);
  L->settop(0);
  for (int64_t k = i; k <= j; ++k)
    L->push(t->get_int(k));
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
  Table* t = check_table(L, 1);
  TValue cmp = L->gettop() >= 2 ? *L->at(2) : TValue::nil();
  int64_t n = table_length(t);
  std::vector<TValue> arr(static_cast<size_t>(n));
  for (int64_t i = 1; i <= n; ++i)
    arr[static_cast<size_t>(i - 1)] = t->get_int(i);
  SortCtx ctx{L, cmp};
  std::stable_sort(arr.begin(), arr.end(), [&](const TValue& a, const TValue& b) {
    return sort_compare(a, b, &ctx) < 0;
  });
  for (int64_t i = 1; i <= n; ++i)
    t->set_int(L, i, arr[static_cast<size_t>(i - 1)]);
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

} // namespace lj3
