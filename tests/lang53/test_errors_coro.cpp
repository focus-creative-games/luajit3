#include "test_harness.hpp"

using namespace luatiertest;

int test_lang53_errors_coro() {
  int f = 0;
  const char* G = "lang53/errcoro";

  // pcall / error / assert
  f += expect_int(G, "local ok,err=pcall(function() error('boom') end); return ok and 1 or 0", 0);
  f += expect_str(G, "local ok,err=pcall(function() error('xyz', 0) end); return err", "xyz");
  f += expect_int(G, "local ok,v=pcall(function() return 42 end); return ok and v or 0", 42);
  f += expect_int(G,
                  "local ok,a,b=pcall(function() return 1,2 end); "
                  "return (ok and a+b) or 0",
                  3);
  f += expect_int(G,
                  "local ok,err=xpcall(function() error('e', 0) end, function(m) return 'h:'..m end); "
                  "return (ok and 0) or ((err=='h:e') and 1 or 0)",
                  1);
  f += expect_int(G, "local ok=pcall(function() assert(false,'nope') end); return ok and 1 or 0", 0);
  f += expect_int(G,
                  "local ok,err=pcall(function() assert(false,'nope') end); "
                  "return (not ok and type(err)=='string' and err:find('nope',1,true)) and 1 or 0",
                  1);

  // Coroutines
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
  f += expect_int(G,
                  "local co=coroutine.create(function() "
                  "  local a,b=coroutine.yield(1,2); return a+b end); "
                  "local ok,x,y=coroutine.resume(co); "
                  "local ok2,v=coroutine.resume(co,3,4); "
                  "return (ok and ok2 and x==1 and y==2 and v==7) and 1 or 0",
                  1);
  f += expect_str(G,
                  "local co=coroutine.create(function() end); "
                  "coroutine.resume(co); return coroutine.status(co)",
                  "dead");
  f += expect_int(G,
                  "local ok=pcall(function() coroutine.yield(1) end); return ok and 1 or 0",
                  0);
  f += expect_int(G,
                  "local co=coroutine.create(function() end); "
                  "coroutine.resume(co); "
                  "local ok=coroutine.resume(co); return ok and 1 or 0",
                  0);
  f += expect_true(G, "local th,ismain=coroutine.running(); return ismain");
  f += expect_int(G,
                  "local outer; "
                  "outer=coroutine.create(function() "
                  "  local inner=coroutine.create(function() "
                  "    local st=coroutine.status(outer); "
                  "    coroutine.yield(st) "
                  "  end); "
                  "  local ok,st=coroutine.resume(inner); "
                  "  return st "
                  "end); "
                  "local ok,st=coroutine.resume(outer); "
                  "return (ok and st=='normal') and 1 or 0",
                  1);

  return f ? 1 : 0;
}
