#include "frontend/parser.hpp"
#include "frontend/sema.hpp"
#include "test_harness.hpp"

#include <iostream>

using namespace luatier;

int test_parser_sema() {
  int f = 0;
  try {
    auto c = parse("local x = 1; return x");
    sema_analyze(*c);
  } catch (const LuatierError& e) {
    f += luatiertest::fail("parser", e.what());
  }
  try {
    auto c = parse("break");
    sema_analyze(*c);
    f += luatiertest::fail("sema", "expected break outside loop error");
  } catch (const LuatierError&) {
    // ok
  }
  try {
    auto c = parse("for i=1,3 do break end");
    sema_analyze(*c);
  } catch (const LuatierError& e) {
    f += luatiertest::fail("sema", e.what());
  }
  // long string / comment
  try {
    auto c = parse("return [=[ab]=]");
    sema_analyze(*c);
  } catch (const LuatierError& e) {
    f += luatiertest::fail("parser-longstr", e.what());
  }
  // empty statement
  try {
    auto c = parse("do ; end");
    sema_analyze(*c);
  } catch (const LuatierError& e) {
    f += luatiertest::fail("parser-semi", e.what());
  }
  return f ? 1 : 0;
}
