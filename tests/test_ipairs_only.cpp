#include "test_harness.hpp"
#include <iostream>
int main() {
  std::cerr << "start\n" << std::flush;
  int r = lj3test::expect_int("ip", "local s=0; for i,v in ipairs({10,20,30}) do s=s+v end; return s", 60);
  std::cerr << "r=" << r << "\n";
  return r;
}
