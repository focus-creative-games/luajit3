#include "test_harness.hpp"

using namespace luatiertest;

int test_lang53_types_coercion() {
  int f = 0;
  const char* G = "lang53/types";

  f += expect_str(G, "return type(nil)", "nil");
  f += expect_str(G, "return type(true)", "boolean");
  f += expect_str(G, "return type(false)", "boolean");
  f += expect_str(G, "return type(1)", "number");
  f += expect_str(G, "return type(1.5)", "number");
  f += expect_str(G, "return type('x')", "string");
  f += expect_str(G, "return type({})", "table");
  f += expect_str(G, "return type(function() end)", "function");
  f += expect_str(G,
                  "local c=coroutine.create(function() end); return type(c)",
                  "thread");

  // int vs float paths still type as number
  f += expect_int(G, "return 1+2", 3);
  f += expect_num(G, "return 1+2.5", 3.5);
  f += expect_num(G, "return 10/4", 2.5);
  f += expect_int(G, "return 10//3", 3);
  f += expect_int(G, "return 10//-3", -4);
  f += expect_int(G, "return -10//3", -4);

  // String→number arithmetic coercion (Lua 5.3)
  f += expect_int(G, "return '10'+2", 12);
  f += expect_error(G, "return 'x'+1");
  f += expect_error(G, "return {}+1");

  // NaN / inf
  f += expect_int(G, "local x=0/0; return (x~=x) and 1 or 0", 1);
  f += expect_int(G, "local x=1/0; return (x>0) and 1 or 0", 1);
  f += expect_int(G, "local x=-1/0; return (x<0) and 1 or 0", 1);

  return f ? 1 : 0;
}
