#include "test_harness.hpp"

using namespace lj3test;

int test_lang53_statements() {
  int f = 0;
  const char* G = "lang53/stmts";

  // Block scope
  f += expect_int(G, "local x=1; do local x=42 end; return x", 1);
  f += expect_int(G, "do local x=42 end; return 1", 1);

  // Assignment adjustment
  f += expect_int(G, "local a,b,c=1,2; return (c==nil and a+b) or 0", 3);
  f += expect_int(G, "local a,b=1,2,3; return a+b", 3);
  f += expect_int(G,
                  "local function f() return 10,20 end; "
                  "local t={}; t.a,t.b=f(); return t.a+t.b",
                  30);

  // if / while / repeat
  f += expect_int(G, "if false then return 1 elseif false then return 2 else return 3 end", 3);
  f += expect_int(G, "if true then return 7 end", 7);
  f += expect_int(G, "local x=0; while x<5 do x=x+1 end; return x", 5);
  f += expect_int(G, "local x=0; repeat x=x+1 until x==4; return x", 4);
  f += expect_int(G,
                  "local x; repeat local y=1; x=y until x; return x",
                  1);

  // Numeric for
  f += expect_int(G, "local s=0; for i=1,10 do s=s+i end; return s", 55);
  f += expect_int(G, "local s=0; for i=10,1,-2 do s=s+i end; return s", 30);
  f += expect_int(G, "local s=0; for i=1,0 do s=s+1 end; return s", 0);
  f += expect_int(G, "local s=0; for i=1,10 do if i==3 then break end; s=s+i end; return s", 3);

  // Generic for
  f += expect_int(G, "local s=0; for i,v in ipairs({10,20,30}) do s=s+v end; return s", 60);
  f += expect_int(G, "local t={a=1,b=2}; local n=0; for k,v in pairs(t) do n=n+v end; return n", 3);
  f += expect_int(G,
                  "local function iter(t,i) i=i+1; local v=t[i]; if v==nil then return nil end; "
                  "return i,v end; "
                  "local s=0; for i,v in iter,{4,5,6},0 do s=s+v end; return s",
                  15);
  f += expect_int(G,
                  "local a,b=0,0; for i,v in ipairs({7,8}) do a=a+i; b=b+v end; return a+b",
                  18);

  // Call as statement discards results
  f += expect_int(G,
                  "local x=0; local function f() x=x+1; return 9,8 end; f(); return x",
                  1);

  // return multret vs truncated
  f += expect_int(G,
                  "local function f() return 1,2,3 end; "
                  "local function g() return f() end; "
                  "local a,b,c=g(); return a+b+c",
                  6);
  f += expect_int(G, "local function f() return 1,2 end; return (f())", 1);
  f += expect_int(G,
                  "local function f() return 1,2,3 end; "
                  "local function g() return (f()) end; return g()",
                  1);

  return f ? 1 : 0;
}
