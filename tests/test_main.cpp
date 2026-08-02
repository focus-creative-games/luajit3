#include <iostream>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

int test_lexer();
int test_value();
int test_vm();
int test_differential();
int test_lang53();
int test_parser_sema();
int test_gc_stress();

int main() {
#ifdef _MSC_VER
  // Avoid assert message-box modal hang; print to stderr instead.
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
  int failed = 0;
  failed += test_lexer();
  failed += test_value();
  failed += test_parser_sema();
  failed += test_vm();
  failed += test_differential();
  failed += test_lang53();
  failed += test_gc_stress();
  if (failed) {
    std::cerr << failed << " test group(s) failed\n";
    return 1;
  }
  std::cout << "all tests passed\n";
  return 0;
}
