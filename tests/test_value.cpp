#include "runtime/value.hpp"
#include "vm/state.hpp"
#include "test_harness.hpp"

#include <iostream>

using namespace lj3;

int test_value() {
  int f = 0;
  auto L = new_state();
  auto* s = L->intern("hello");
  TValue v = TValue::obj(ValueTag::String, s);
  if (!v.is_string() || v.as_string()->view() != "hello") {
    std::cerr << "value FAIL: string\n";
    f = 1;
  }
  if (!values_equal(TValue::integer(3), TValue::number(3.0))) {
    std::cerr << "value FAIL: int/float equal\n";
    f = 1;
  }
  auto* t = table_new(L.get(), 4, 4);
  t->set_int(L.get(), 1, TValue::integer(42));
  if (t->get_int(1).as_int() != 42) {
    std::cerr << "value FAIL: table\n";
    f = 1;
  }
  if (t->get(TValue::number(1.0)).as_int() != 42) {
    std::cerr << "value FAIL: table float index\n";
    f = 1;
  }
  f += lj3test::expect_num("value", "local t={0,2}; return rawget(t,1.0)", 0);
  f += lj3test::expect_num("value", "local t={0,2}; for i=1,2 do return t[i] end", 0);
  return f;
}
