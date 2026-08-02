#include "test_harness.hpp"

using namespace lj3test;

int test_lang53_visibility_goto() {
  int f = 0;
  const char* G = "lang53/goto";

  f += expect_int(G, "local x=0; goto skip; x=1; ::skip:: return x+2", 2);
  f += expect_int(G,
                  "local x=0; ::loop:: x=x+1; if x<3 then goto loop end; return x",
                  3);
  // Jump into scope of a local: Lua 5.3 forbids this; if sema is lax, still must not hang.
  f += expect_int(G, "goto L; ::L:: return 1", 1);
  f += expect_error(G, "::L:: ::L:: return 1");

  // Closure visibility: capture after assignment visible
  f += expect_int(G,
                  "local f; local function g() return f() end; "
                  "f=function() return 9 end; return g()",
                  9);
  f += expect_int(G,
                  "local a=1; local function f() return a end; "
                  "do local a=2 end; return f()",
                  1);

  return f ? 1 : 0;
}
