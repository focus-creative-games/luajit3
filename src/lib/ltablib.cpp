#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "vm/interpreter.hpp"
#include "vm/meta.hpp"

#include <climits>
#include <ctime>

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
    // PUC: f > 0 || e < LUA_MAXINTEGER + f  (reject overflow of e - f + 1)
    if (f <= 0 &&
        static_cast<uint64_t>(e) - static_cast<uint64_t>(f) >=
            static_cast<uint64_t>(INT64_MAX))
      panic("too many elements to move");
    int64_t n = e - f + 1;
    if (tpos > INT64_MAX - n + 1)
      panic("destination wrap around");
    bool same = values_equal(dest, a);
    if (tpos > e || tpos <= f || !same) {
      for (int64_t i = 0; i < n; ++i)
        seti(L, dest, tpos + i, geti(L, a, f + i));
    } else {
      for (int64_t i = n - 1; i >= 0; --i)
        seti(L, dest, tpos + i, geti(L, a, f + i));
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
  if (i > j)
    return 0;
  // PUC: n = e - i (count - 1); reject if n >= INT_MAX or !checkstack(++n)
  uint64_t n = static_cast<uint64_t>(j) - static_cast<uint64_t>(i);
  if (n >= static_cast<uint64_t>(INT_MAX) - 1)
    panic("too many results to unpack");
  int nres = static_cast<int>(n + 1);
  L->settop(0);
  for (int64_t k = i; k < j; ++k)
    L->push(geti(L, t, k));
  L->push(geti(L, t, j));
  return nres;
}

// --- PUC-style quicksort (ltablib.c), in-place via geti/seti ---
using IdxT = unsigned int;

static bool sort_comp_idx(State* L, IdxT ai, IdxT bi) {
  TValue t = *L->at(1);
  TValue a = geti(L, t, static_cast<int64_t>(ai));
  TValue b = geti(L, t, static_cast<int64_t>(bi));
  TValue cmp = *L->at(2);
  if (cmp.is_nil()) {
    bool lt = false;
    meta_lt(L, a, b, &lt);
    return lt;
  }
  if (!cmp.is_function())
    panic("bad argument #2 to 'sort' (function expected)");
  L->push(cmp);
  L->push(a);
  L->push(b);
  call_closure(L, cmp.as_closure(), 2, 1);
  bool lt = L->at(L->gettop())->is_truthy();
  L->settop(2); // keep table + cmp
  return lt;
}

static void sort_swap(State* L, IdxT i, IdxT j) {
  TValue t = *L->at(1);
  TValue ai = geti(L, t, static_cast<int64_t>(i));
  TValue aj = geti(L, t, static_cast<int64_t>(j));
  seti(L, t, static_cast<int64_t>(i), aj);
  seti(L, t, static_cast<int64_t>(j), ai);
}

static IdxT sort_partition(State* L, IdxT lo, IdxT up) {
  IdxT i = lo;
  IdxT j = up - 1;
  for (;;) {
    while (sort_comp_idx(L, ++i, up - 1)) {
      if (i == up - 1)
        panic("invalid order function for sorting");
    }
    while (sort_comp_idx(L, up - 1, --j)) {
      if (j < i)
        panic("invalid order function for sorting");
    }
    if (j < i) {
      sort_swap(L, up - 1, i);
      return i;
    }
    sort_swap(L, i, j);
  }
}

static IdxT choose_pivot(IdxT lo, IdxT up, unsigned int rnd) {
  IdxT r4 = (up - lo) / 4;
  return rnd % (r4 * 2) + (lo + r4);
}

static unsigned int randomize_pivot() {
  clock_t c = clock();
  time_t t = time(nullptr);
  return static_cast<unsigned int>(c) + static_cast<unsigned int>(t);
}

static void auxsort(State* L, IdxT lo, IdxT up, unsigned int rnd) {
  while (lo < up) {
    if (sort_comp_idx(L, up, lo))
      sort_swap(L, lo, up);
    if (up - lo == 1)
      return;
    IdxT p;
    constexpr unsigned RANLIMIT = 100u;
    if (up - lo < RANLIMIT || rnd == 0)
      p = (lo + up) / 2;
    else
      p = choose_pivot(lo, up, rnd);
    if (sort_comp_idx(L, p, lo))
      sort_swap(L, p, lo);
    else if (sort_comp_idx(L, up, p))
      sort_swap(L, p, up);
    if (up - lo == 2)
      return;
    sort_swap(L, p, up - 1);
    p = sort_partition(L, lo, up);
    IdxT n;
    if (p - lo < up - p) {
      auxsort(L, lo, p - 1, rnd);
      n = p - lo;
      lo = p + 1;
    } else {
      auxsort(L, p + 1, up, rnd);
      n = up - p;
      up = p - 1;
    }
    if ((up - lo) / 128 > n)
      rnd = randomize_pivot();
  }
}

static int tab_sort(State* L) {
  TValue t = *L->at(1);
  if (!t.is_table())
    panic("bad argument #1 to 'table.sort' (table expected, got " + obj_type_name(L, t) + ")");
  int64_t n = aux_getn(L, t);
  if (n > 1) {
    if (n >= INT_MAX)
      panic("array too big");
    if (L->gettop() >= 2 && !L->at(2)->is_nil() && !L->at(2)->is_function())
      panic("bad argument #2 to 'sort' (function expected)");
    while (L->gettop() < 2)
      L->push(TValue::nil());
    L->settop(2);
    auxsort(L, 1, static_cast<IdxT>(n), 0);
  }
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
