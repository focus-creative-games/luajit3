#include "lib/libs.hpp"

#include "lib/lib_util.hpp"

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

static int math_abs(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::fabs(x)));
  return 1;
}

static int math_floor(State* L) {
  double x = check_number(L, 1);
  bool toint = L->gettop() >= 2 && L->at(2)->is_truthy();
  L->settop(0);
  if (toint)
    L->push(TValue::integer(static_cast<int64_t>(std::floor(x))));
  else
    L->push(TValue::number(std::floor(x)));
  return 1;
}

static int math_ceil(State* L) {
  double x = check_number(L, 1);
  L->settop(0);
  L->push(TValue::number(std::ceil(x)));
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
  double y = check_number(L, 1);
  bool two = L->gettop() >= 2;
  double x = two ? check_number(L, 2) : 0;
  L->settop(0);
  if (two)
    L->push(TValue::number(std::atan2(y, x)));
  else
    L->push(TValue::number(std::atan(y)));
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
  L->settop(0);
  if (L->gettop() >= 2) {
    double base = check_number(L, 2);
    L->push(TValue::number(std::log(x) / std::log(base)));
  } else
    L->push(TValue::number(std::log(x)));
  return 1;
}

static int math_modf(State* L) {
  double x = check_number(L, 1);
  double ip;
  double fp = std::modf(x, &ip);
  L->settop(0);
  L->push(TValue::number(ip));
  L->push(TValue::number(fp));
  return 2;
}

static int math_fmod(State* L) {
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
  if (L->gettop() < 1)
    panic("math.max: value expected");
  double m = L->at(1)->to_number();
  for (int i = 2; i <= L->gettop(); ++i)
    m = std::max(m, L->at(i)->to_number());
  L->settop(0);
  L->push(TValue::number(m));
  return 1;
}

static int math_min(State* L) {
  if (L->gettop() < 1)
    panic("math.min: value expected");
  double m = L->at(1)->to_number();
  for (int i = 2; i <= L->gettop(); ++i)
    m = std::min(m, L->at(i)->to_number());
  L->settop(0);
  L->push(TValue::number(m));
  return 1;
}

static int math_tointeger(State* L) {
  TValue v = L->gettop() >= 1 ? *L->at(1) : TValue::nil();
  L->settop(0);
  if (v.is_int()) {
    L->push(v);
    return 1;
  }
  if (v.is_float()) {
    double x = v.as_float();
    if (x == std::floor(x) && x >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
        x <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
      L->push(TValue::integer(static_cast<int64_t>(x)));
      return 1;
    }
  }
  L->push(TValue::nil());
  return 1;
}

static int math_type(State* L) {
  if (L->gettop() < 1) {
    L->push(TValue::nil());
    return 1;
  }
  TValue v = *L->at(1);
  // Push on top without settop(0): results are taken as the topmost nret values.
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
  if (top == 0) {
    L->settop(0);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    L->push(TValue::number(dist(rng())));
    return 1;
  }
  if (top == 1) {
    int64_t u = check_int(L, 1);
    L->settop(0);
    std::uniform_int_distribution<int64_t> dist(1, u);
    L->push(TValue::integer(dist(rng())));
    return 1;
  }
  int64_t lo = check_int(L, 1);
  int64_t hi = check_int(L, 2);
  if (lo > hi)
    std::swap(lo, hi);
  L->settop(0);
  std::uniform_int_distribution<int64_t> dist(lo, hi);
  L->push(TValue::integer(dist(rng())));
  return 1;
}

static int math_randomseed(State* L) {
  int64_t seed = check_int(L, 1);
  rng().seed(static_cast<uint32_t>(seed));
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
