#include "test_harness.hpp"

using namespace lj3test;

int test_lang53() {
  int f = 0;
  const char* G = "lang53";

  // Arithmetic & precedence (manual §3.4.1)
  f += expect_int(G, "return 1+2*3", 7);
  f += expect_int(G, "return (1+2)*3", 9);
  f += expect_int(G, "return 10//3", 3);
  f += expect_int(G, "return 10//-3", -4);
  f += expect_int(G, "return 5%3", 2);
  f += expect_int(G, "return 2^3", 8);
  f += expect_int(G, "return - -3", 3);

  // Bitwise (manual §3.4.2)
  f += expect_int(G, "return 1<<3", 8);
  f += expect_int(G, "return 8>>2", 2);
  f += expect_int(G, "return 5&3", 1);
  f += expect_int(G, "return 5|2", 7);
  f += expect_int(G, "return 5~3", 6);
  f += expect_int(G, "return ~0", -1);

  // Relational / logical
  f += expect_int(G, "return (3>2) and 1 or 0", 1);
  f += expect_int(G, "return (3<2) and 1 or 0", 0);
  f += expect_int(G, "return (1==1) and 1 or 0", 1);
  f += expect_int(G, "return (1~=2) and 1 or 0", 1);
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

  // Strings
  f += expect_str(G, "return \"a\"..\"b\"..\"c\"", "abc");
  f += expect_int(G, "return #\"hello\"", 5);
  f += expect_int(G, "return #(\"xy\"..\"z\")", 3);

  // Tables
  f += expect_int(G, "local t={10,20,30}; return t[2]", 20);
  f += expect_int(G, "local t={a=7,b=8}; return t.a+t.b", 15);
  f += expect_int(G, "local t={}; t[1]=5; t[2]=6; return t[1]+t[2]", 11);
  f += expect_int(G, "local t={1,2,3}; return #t", 3);

  // Control flow
  f += expect_int(G, "if false then return 1 elseif false then return 2 else return 3 end", 3);
  f += expect_int(G, "local x=0; while x<5 do x=x+1 end; return x", 5);
  f += expect_int(G, "local x=0; repeat x=x+1 until x==4; return x", 4);
  f += expect_int(G, "local s=0; for i=1,10 do s=s+i end; return s", 55);
  f += expect_int(G, "local s=0; for i=10,1,-2 do s=s+i end; return s", 30);
  f += expect_int(G, "do local x=42 end; return 1", 1);
  f += expect_int(G, "local s=0; for i=1,10 do if i==3 then break end; s=s+i end; return s", 3);

  // Functions / closures / upvalues
  f += expect_int(G, "local function add(a,b) return a+b end; return add(20,22)", 42);
  f += expect_int(G, "local a=1; local function f() return a end; a=7; return f()", 7);
  f += expect_int(G,
                  "local function outer(x) "
                  "  local function inner(y) return x+y end "
                  "  return inner "
                  "end; return outer(40)(2)",
                  42);
  f += expect_int(G, "local function f(...) return select('#', ...) end; return f(1,2,3)", 3);
  f += expect_int(G, "local function f(a,...) return select(1,...) end; return f(9,10,11)", 10);

  // Multiple returns
  f += expect_int(G, "local function f() return 1,2,3 end; local a,b,c=f(); return a+b+c", 6);
  f += expect_int(G, "local function f() return 1,2 end; return (f())", 1);

  // Metatables
  f += expect_int(G,
                  "local t={}; local mt={__index={x=99}}; setmetatable(t, mt); return t.x",
                  99);
  f += expect_int(G,
                  "local t={}; local mt={__newindex=function(t,k,v) rawset(t,k,v*2) end}; "
                  "setmetatable(t,mt); t.a=21; return t.a",
                  42);
  f += expect_int(G,
                  "local a=setmetatable({},{__add=function(x,y) return 100 end}); "
                  "return (a+1)",
                  100);
  f += expect_int(G,
                  "local a=setmetatable({},{__len=function() return 7 end}); return #a",
                  7);

  // pcall / error
  f += expect_int(G, "local ok,err=pcall(function() error('boom') end); return ok and 1 or 0", 0);
  f += expect_int(G, "local ok,v=pcall(function() return 42 end); return ok and v or 0", 42);
  f += expect_str(G, "local ok,err=pcall(function() error('xyz') end); return err", "xyz");

  // ipairs / pairs / next
  f += expect_int(G,
                  "local s=0; for i,v in ipairs({10,20,30}) do s=s+v end; return s",
                  60);
  f += expect_int(G,
                  "local t={a=1,b=2}; local n=0; for k,v in pairs(t) do n=n+v end; return n",
                  3);

  // goto
  f += expect_int(G, "local x=0; goto skip; x=1; ::skip:: return x+2", 2);

  // coroutine create/resume/yield
  f += expect_int(G,
                  "local co=coroutine.create(function(x) return x+2 end); "
                  "local ok,v=coroutine.resume(co, 40); return (ok and v) or 0",
                  42);
  f += expect_str(G,
                  "local co=coroutine.create(function() end); return coroutine.status(co)",
                  "suspended");
  f += expect_int(G,
                  "local co=coroutine.create(function() "
                  "  coroutine.yield(10); return 20 end); "
                  "local ok,v=coroutine.resume(co); "
                  "local ok2,v2=coroutine.resume(co); "
                  "return (ok and ok2 and v==10 and v2==20) and 1 or 0",
                  1);

  // NaN inequality
  f += expect_int(G, "local x=0/0; return (x~=x) and 1 or 0", 1);

  // Integer/float subtype
  f += expect_str(G, "return type(1)", "number");
  f += expect_str(G, "return type(1.5)", "number");
  f += expect_int(G, "return math and 0 or 1", 1); // math lib not required in language core

  return f ? 1 : 0;
}
