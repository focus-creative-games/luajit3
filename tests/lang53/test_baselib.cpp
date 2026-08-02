#include "test_harness.hpp"

using namespace lj3test;

int test_lang53_baselib() {
  int f = 0;
  const char* G = "lang53/base";

  f += expect_int(G, "return tonumber('42')", 42);
  f += expect_num(G, "return tonumber('3.5')", 3.5);
  f += expect_nil(G, "return tonumber('x')");
  f += expect_str(G, "return tostring(42)", "42");
  f += expect_str(G, "return tostring(true)", "true");
  f += expect_str(G, "return tostring(nil)", "nil");

  f += expect_int(G, "return (select(-2, 10,20,30))", 20);
  f += expect_int(G, "return select('#', 1,2,3,4)", 4);

  // next / pairs key-set: sum of values for {a=1,b=2,c=3}
  f += expect_int(G,
                  "local t={a=1,b=2,c=3}; local s=0; "
                  "local k,v=next(t); while k~=nil do s=s+v; k,v=next(t,k) end; return s",
                  6);
  f += expect_int(G,
                  "local t={a=1,b=2}; local n=0; for k,v in pairs(t) do n=n+1 end; return n",
                  2);

  // math library is part of full 5.3 stdlib
  f += expect_int(G, "return type(math)=='table' and math.floor(3.7) or 0", 3);

  return f ? 1 : 0;
}
