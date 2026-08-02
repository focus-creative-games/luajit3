#include "test_harness.hpp"

#include <cstdlib>

namespace {

void set_env(const char* key, const char* value) {
#if defined(_WIN32)
  _putenv_s(key, value);
#else
  setenv(key, value, 1);
#endif
}

} // namespace

int test_gc_stress() {
  set_env("LJ3_STRESS_GC_EVERY_SAFEPOINT", "1");
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
  set_env("LJ3_STRESS_GC_EVERY_SAFEPOINT", "0");
  return f ? 1 : 0;
}
