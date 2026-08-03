#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "runtime/value.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

namespace luatier {
using namespace lib;

static std::mt19937& rng() {
  static thread_local std::mt19937 gen{std::random_device{}()};
  return gen;
}

// PUC pushnumint: float that fits exactly in an integer → integer, else float.
static void push_num_int(State* L, double d) {
  int64_t n;
  if (float_to_integer(d, &n))
    L->push(TValue::integer(n));
  else
    L->push(TValue::number(d));
}

static bool number_less(const TValue& a, const TValue& b) {
  return number_lt(a, b);
}

static int math_abs(State* L) {
  check_any(L, 1, "abs");
  TValue v = *L->at(1);
  if (v.is_int()) {
    int64_t n = v.as_int();
    if (n < 0)
      n = static_cast<int64_t>(0u - static_cast<uint64_t>(n));
    L->settop(0);
    L->push(TValue::integer(n));
  } else {
    double x = check_number(L, 1);
    L->settop(0);
    L->push(TValue::number(std::fabs(x)));
  }
  return 1;
}

static int math_floor(State* L) {
  check_any(L, 1, "floor");
  if (L->at(1)->is_int()) {
    L->settop(1);
    return 1;
  }
  double d = std::floor(check_number(L, 1));
  L->settop(0);
  push_num_int(L, d);
  return 1;
}

static int math_ceil(State* L) {
  check_any(L, 1, "ceil");
  if (L->at(1)->is_int()) {
    L->settop(1);
    return 1;
  }
  double d = std::ceil(check_number(L, 1));
  L->settop(0);
  push_num_int(L, d);
  return 1;
}

static int math_sqrt(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::sqrt(x)));
  return 1;
}

static int math_sin(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::sin(x)));
  return 1;
}

static int math_cos(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::cos(x)));
  return 1;
}

static int math_tan(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::tan(x)));
  return 1;
}

static int math_asin(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::asin(x)));
  return 1;
}

static int math_acos(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::acos(x)));
  return 1;
}

static int math_atan(State* L) {
  // PUC: atan(y [, x]) → atan2(y, x or 1)
  double y = check_number(L, 1);
  double x = (L->gettop() >= 2 && !L->at(2)->is_nil()) ? check_number(L, 2) : 1.0;
  L->settop(0);
  L->push(TValue::number(std::atan2(y, x)));
  return 1;
}

static int math_exp(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::exp(x)));
  return 1;
}

static int math_log(State* L) {
  double x = check_number(L, 1);
  const bool has_base = L->gettop() >= 2 && !L->at(2)->is_nil();
  double base = has_base ? check_number(L, 2) : 0.0;
  L->settop(0);
  double res;
  if (!has_base)
    res = std::log(x);
  else if (base == 2.0)
    res = std::log2(x);
  else if (base == 10.0)
    res = std::log10(x);
  else
    res = std::log(x) / std::log(base);
  L->push(TValue::number(res));
  return 1;
}

static int math_modf(State* L) {
  check_any(L, 1, "modf");
  if (L->at(1)->is_int()) {
    L->settop(1); // integer is its own integer part
    L->push(TValue::number(0.0));
    return 2;
  }
  double n = check_number(L, 1);
  double ip = (n < 0) ? std::ceil(n) : std::floor(n);
  L->settop(0);
  push_num_int(L, ip);
  L->push(TValue::number((n == ip) ? 0.0 : (n - ip)));
  return 2;
}

static int math_fmod(State* L) {
  if (L->gettop() >= 2 && L->at(1)->is_int() && L->at(2)->is_int()) {
    int64_t a = L->at(1)->as_int();
    int64_t d = L->at(2)->as_int();
    if (static_cast<uint64_t>(d) + 1u <= 1u) { // d == 0 or d == -1
      if (d == 0)
        panic("bad argument #2 to 'fmod' (zero)");
      L->settop(0);
      L->push(TValue::integer(0)); // avoid overflow with minint % -1
      return 1;
    }
    L->settop(0);
    L->push(TValue::integer(a % d));
    return 1;
  }
  double x = check_number(L, 1);
  double y = check_number(L, 2);
  L->settop(0);
  L->push(TValue::number(std::fmod(x, y)));
  return 1;
}

static int math_pow(State* L) {
  double x = check_number(L, 1);
  double y = check_number(L, 2);
  L->settop(0);
  L->push(TValue::number(std::pow(x, y)));
  return 1;
}

static int math_deg(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(x * (180.0 / 3.14159265358979323846)));
  return 1;
}

static int math_rad(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(x * (3.14159265358979323846 / 180.0)));
  return 1;
}

static int math_max(State* L) {
  int n = L->gettop();
  if (n < 1)
    panic("value expected");
  int imax = 1;
  for (int i = 2; i <= n; ++i) {
    if (number_less(*L->at(imax), *L->at(i)))
      imax = i;
  }
  TValue best = *L->at(imax);
  L->settop(0);
  L->push(best);
  return 1;
}

static int math_min(State* L) {
  int n = L->gettop();
  if (n < 1)
    panic("value expected");
  int imin = 1;
  for (int i = 2; i <= n; ++i) {
    if (number_less(*L->at(i), *L->at(imin)))
      imin = i;
  }
  TValue best = *L->at(imin);
  L->settop(0);
  L->push(best);
  return 1;
}

static int math_tointeger(State* L) {
  if (L->gettop() < 1) {
    L->push(TValue::nil());
    return 1;
  }
  TValue v = *L->at(1);
  L->settop(0);
  if (v.is_int()) {
    L->push(v);
    return 1;
  }
  TValue n;
  if (try_to_number(v, &n)) {
    if (n.is_int()) {
      L->push(n);
      return 1;
    }
    if (n.is_float()) {
      int64_t i;
      if (float_to_integer(n.as_float(), &i)) {
        L->push(TValue::integer(i));
        return 1;
      }
    }
  }
  L->push(TValue::nil());
  return 1;
}

static int math_type(State* L) {
  if (L->gettop() < 1)
    panic("value expected");
  TValue v = *L->at(1);
  if (v.is_int())
    push_string(L, "integer");
  else if (v.is_float())
    push_string(L, "float");
  else
    L->push(TValue::nil());
  return 1;
}

static int math_ult(State* L) {
  uint64_t a = static_cast<uint64_t>(check_int(L, 1));
  uint64_t b = static_cast<uint64_t>(check_int(L, 2));
  L->settop(0);
  L->push(TValue::boolean(a < b));
  return 1;
}

static int math_random(State* L) {
  int top = L->gettop();
  if (top > 2)
    panic("wrong number of arguments");
  // Match PUC: r in [0,1), then scale.
  double r = std::generate_canonical<double, 53>(rng());
  if (top == 0) {
    L->settop(0);
    L->push(TValue::number(r));
    return 1;
  }
  int64_t low, up;
  if (top == 1) {
    low = 1;
    up = check_int(L, 1);
  } else {
    low = check_int(L, 1);
    up = check_int(L, 2);
  }
  if (low > up)
    panic("interval is empty");
  // PUC: low >= 0 || up <= LUA_MAXINTEGER + low
  if (low < 0) {
    int64_t lim = static_cast<int64_t>(
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + static_cast<uint64_t>(low));
    if (up > lim)
      panic("interval too large");
  }
  L->settop(0);
  r *= static_cast<double>(up - low) + 1.0;
  L->push(TValue::integer(static_cast<int64_t>(r) + low));
  return 1;
}

static int math_randomseed(State* L) {
  // PUC: seed from number (may be float), discard first rand.
  double seed = check_number(L, 1);
  rng().seed(static_cast<uint32_t>(static_cast<int64_t>(seed)));
  (void)rng()();
  return 0;
}

static int math_frexp(State* L) {
  double x = check_number(L, 1);
  int exp = 0;
  double m = std::frexp(x, &exp);
  L->settop(0);
  L->push(TValue::number(m));
  L->push(TValue::integer(exp));
  return 2;
}

static int math_ldexp(State* L) {
  double m = check_number(L, 1);
  int exp = static_cast<int>(check_int(L, 2));
  L->settop(0);
  L->push(TValue::number(std::ldexp(m, exp)));
  return 1;
}

void open_math_lib(State* L) {
  Table* m = new_lib(L, 32);
  set_field(L, m, "abs", math_abs);
  set_field(L, m, "floor", math_floor);
  set_field(L, m, "ceil", math_ceil);
  set_field(L, m, "sqrt", math_sqrt);
  set_field(L, m, "sin", math_sin);
  set_field(L, m, "cos", math_cos);
  set_field(L, m, "tan", math_tan);
  set_field(L, m, "asin", math_asin);
  set_field(L, m, "acos", math_acos);
  set_field(L, m, "atan", math_atan);
  set_field(L, m, "exp", math_exp);
  set_field(L, m, "log", math_log);
  set_field(L, m, "modf", math_modf);
  set_field(L, m, "fmod", math_fmod);
  set_field(L, m, "pow", math_pow);
  set_field(L, m, "deg", math_deg);
  set_field(L, m, "rad", math_rad);
  set_field(L, m, "max", math_max);
  set_field(L, m, "min", math_min);
  set_field(L, m, "tointeger", math_tointeger);
  set_field(L, m, "type", math_type);
  set_field(L, m, "ult", math_ult);
  set_field(L, m, "random", math_random);
  set_field(L, m, "randomseed", math_randomseed);
  set_field(L, m, "frexp", math_frexp);
  set_field(L, m, "ldexp", math_ldexp);
  set_field_value(L, m, "pi", TValue::number(3.14159265358979323846));
  set_field_value(L, m, "huge", TValue::number(HUGE_VAL));
  set_field_value(L, m, "maxinteger", TValue::integer(std::numeric_limits<int64_t>::max()));
  set_field_value(L, m, "mininteger", TValue::integer(std::numeric_limits<int64_t>::min()));
  set_global_value(L, "math", TValue::obj(ValueTag::Table, m));
}

} // namespace luatier
