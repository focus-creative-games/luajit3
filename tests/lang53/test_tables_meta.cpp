#include "test_harness.hpp"

using namespace luatiertest;

int test_lang53_tables_meta() {
  int f = 0;
  const char* G = "lang53/tables";

  // Constructors
  f += expect_int(G, "local t={10,20,30}; return t[2]", 20);
  f += expect_int(G, "local t={a=7,b=8}; return t.a+t.b", 15);
  f += expect_int(G, "local t={}; t[1]=5; t[2]=6; return t[1]+t[2]", 11);
  f += expect_int(G, "local k='z'; local t={[k]=9, [1+1]=4}; return t.z+t[2]", 13);
  f += expect_int(G, "local t={1,2,a=3}; return t[1]+t[2]+t.a", 6);

  // Trailing multret in constructors
  f += expect_int(G,
                  "local function f() return 4,5,6 end; "
                  "local t={f()}; return t[1]+t[2]+t[3]",
                  15);
  f += expect_int(G,
                  "local function f() return 4,5,6 end; "
                  "local t={1,f()}; return t[1]+t[2]+t[3]+(t[4] or 0)",
                  16);

  f += expect_int(G, "local t={1,2,3}; return #t", 3);
  f += expect_int(G, "return rawget({a=1},'a')", 1);
  f += expect_int(G, "local t={}; rawset(t,'a',2); return t.a", 2);
  f += expect_int(G, "return rawlen({1,2,3,4})", 4);
  f += expect_true(G, "return rawequal(1,1)");
  f += expect_false(G, "return rawequal({},{})");

  // Metatables
  f += expect_int(G,
                  "local t={}; local mt={__index={x=99}}; setmetatable(t, mt); return t.x",
                  99);
  f += expect_int(G,
                  "local t={}; local mt={__index=function(t,k) return 11 end}; "
                  "setmetatable(t,mt); return t.z",
                  11);
  f += expect_int(G,
                  "local t={}; local mt={__newindex=function(t,k,v) rawset(t,k,v*2) end}; "
                  "setmetatable(t,mt); t.a=21; return t.a",
                  42);
  f += expect_int(G,
                  "local a=setmetatable({},{__add=function(x,y) return 100 end}); return (a+1)",
                  100);
  f += expect_int(G,
                  "local a=setmetatable({},{__unm=function() return 5 end}); return -a",
                  5);
  f += expect_int(G,
                  "local a=setmetatable({},{__len=function() return 7 end}); return #a",
                  7);
  f += expect_str(G,
                  "local a=setmetatable({},{__concat=function(x,y) return 'ok' end}); "
                  "return a..'x'",
                  "ok");
  f += expect_true(G,
                   "local eq=function() return true end; "
                   "local a=setmetatable({},{__eq=eq}); "
                   "local b=setmetatable({},{__eq=eq}); "
                   "return a==b");
  f += expect_true(G,
                   "local lt=function() return true end; "
                   "local a=setmetatable({},{__lt=lt}); "
                   "local b=setmetatable({},{__lt=lt}); "
                   "return a<b");
  f += expect_int(G,
                  "local a=setmetatable({},{__call=function(t,x) return x*2 end}); "
                  "return a(21)",
                  42);
  f += expect_str(G,
                  "local t={}; local mt={__metatable='hid'}; setmetatable(t,mt); "
                  "return getmetatable(t)",
                  "hid");
  f += expect_error(G,
                    "local t={}; local mt={__metatable='hid'}; setmetatable(t,mt); "
                    "setmetatable(t,{})");
  f += expect_int(G,
                  "local t=setmetatable({},{__index=function() return 1 end}); "
                  "return rawget(t,'x')==nil and 1 or 0",
                  1);

  return f ? 1 : 0;
}
