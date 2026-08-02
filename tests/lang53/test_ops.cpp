#include "test_harness.hpp"

using namespace lj3test;

int test_lang53_ops() {
  int f = 0;
  const char* G = "lang53/ops";

  // Arithmetic
  f += expect_int(G, "return 1+2*3", 7);
  f += expect_int(G, "return (1+2)*3", 9);
  f += expect_int(G, "return 5%3", 2);
  f += expect_int(G, "return 2^3", 8);
  f += expect_int(G, "return 2^3^2", 512); // right-assoc: 2^(3^2)
  f += expect_int(G, "return - -3", 3);
  f += expect_int(G, "return 10-3-2", 5);

  // Bitwise
  f += expect_int(G, "return 1<<3", 8);
  f += expect_int(G, "return 8>>2", 2);
  f += expect_int(G, "return 5&3", 1);
  f += expect_int(G, "return 5|2", 7);
  f += expect_int(G, "return 5~3", 6);
  f += expect_int(G, "return ~0", -1);
  f += expect_error(G, "return 1.5&1");

  // Relational
  f += expect_true(G, "return 3>2");
  f += expect_false(G, "return 3<2");
  f += expect_true(G, "return 1==1");
  f += expect_true(G, "return 1~=2");
  f += expect_true(G, "return 1<=1");
  f += expect_true(G, "return 2>=2");
  f += expect_false(G, "return 1=='1'"); // number ~= string without coercion for ==
  f += expect_false(G, "return {}=={}");
  f += expect_true(G, "local t={}; return t==t");

  // Logical values + short-circuit
  f += expect_int(G, "return (3>2) and 1 or 0", 1);
  f += expect_int(G, "return false and 5 or 9", 9);
  f += expect_int(G, "return nil or 4", 4);
  f += expect_int(G, "return 2 and 3", 3);
  f += expect_int(G, "return not false and 1 or 0", 1);
  f += expect_int(G,
                  "local x=0; local function t() x=x+1; return true end; "
                  "local function f() x=x+10; return false end; "
                  "local _ = f() and t(); return x",
                  10);
  f += expect_int(G,
                  "local x=0; local function t() x=x+1; return true end; "
                  "local function f() x=x+10; return false end; "
                  "local _ = t() or f(); return x",
                  1);

  // Concat / length
  f += expect_str(G, "return \"a\"..\"b\"..\"c\"", "abc");
  f += expect_str(G, "return 'a'..(1+2)", "a3");
  f += expect_int(G, "return #\"hello\"", 5);
  f += expect_int(G, "return #(\"xy\"..\"z\")", 3);
  f += expect_int(G, "local t={1,2,3}; return #t", 3);
  // Hole border is implementation-defined among valid borders; only require finite length.
  f += expect_int(G, "local t={1,nil,3}; local n=#t; return (n>=0 and n<=3) and 1 or 0", 1);

  // Precedence
  f += expect_int(G, "return 1+2<<1", 6);   // (1+2)<<1
  f += expect_int(G, "return 1<<2+1", 8);   // 1<<(2+1)
  f += expect_int(G, "return not not true and 1 or 0", 1);

  return f ? 1 : 0;
}
