#include "test_harness.hpp"

#include <cstdlib>

int test_gc_stress() {
  _putenv_s("LJ3_STRESS_GC_EVERY_SAFEPOINT", "1");
  int f = 0;
  f += lj3test::expect_int("gc",
                           "local t={}; for i=1,200 do t[i]=i end; local s=0; "
                           "for i=1,200 do s=s+t[i] end; return s",
                           20100);
  f += lj3test::expect_int("gc",
                           "local function mk(n) "
                           "  if n==0 then return 0 end "
                           "  return mk(n-1)+1 "
                           "end; return mk(50)",
                           50);
  _putenv_s("LJ3_STRESS_GC_EVERY_SAFEPOINT", "0");
  return f ? 1 : 0;
}
