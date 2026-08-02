#include "test_harness.hpp"

using namespace lj3test;

int test_lang53_functions() {
  int f = 0;
  const char* G = "lang53/funcs";

  f += expect_int(G, "local function add(a,b) return a+b end; return add(20,22)", 42);
  f += expect_nil(G, "local function f(a) return a end; return f()");
  f += expect_int(G, "local function f(a,b) return (a or 0)+(b or 0) end; return f(3)", 3);

  // vararg / select
  f += expect_int(G, "local function f(...) return select('#', ...) end; return f(1,2,3)", 3);
  f += expect_int(G, "local function f(a,...) return (select(1,...)) end; return f(9,10,11)", 10);
  f += expect_int(G, "local function f(...) return select(-1,...) end; return f(1,2,9)", 9);
  f += expect_int(G,
                  "local function f(...) local a,b=...; return a+b end; return f(3,4,5)",
                  7);

  // Closures
  f += expect_int(G, "local a=1; local function f() return a end; a=7; return f()", 7);
  f += expect_int(G,
                  "local function outer(x) "
                  "  local function inner(y) return x+y end "
                  "  return inner "
                  "end; return outer(40)(2)",
                  42);
  f += expect_int(G,
                  "local function mk() local n=0; return function() n=n+1; return n end end; "
                  "local a=mk(); return a()+a()+a()",
                  6);

  // Method call
  f += expect_int(G,
                  "local o={x=10; get=function(self) return self.x end}; return o:get()",
                  10);
  f += expect_int(G,
                  "local o={n=0}; function o:inc(k) self.n=self.n+k end; o:inc(3); return o.n",
                  3);

  // Multret contexts
  f += expect_int(G, "local function f() return 1,2,3 end; local a,b,c=f(); return a+b+c", 6);
  f += expect_int(G, "local function f() return 1,2 end; return (f())", 1);
  f += expect_int(G,
                  "local function f() return 2,3 end; "
                  "local function g(a,b,c) return (a or 0)+(b or 0)+(c or 0) end; "
                  "return g(1,f())",
                  6);
  f += expect_int(G,
                  "local function f() return 2,3 end; "
                  "local function g(a,b,c) return (a or 0)+(b or 0)+(c or 0) end; "
                  "return g(f(),1)",
                  3); // only first of f() kept when not last

  return f ? 1 : 0;
}
