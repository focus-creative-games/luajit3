int test_lang53_types_coercion();
int test_lang53_ops();
int test_lang53_statements();
int test_lang53_tables_meta();
int test_lang53_functions();
int test_lang53_visibility_goto();
int test_lang53_errors_coro();
int test_lang53_baselib();

int test_lang53() {
  int f = 0;
  f += test_lang53_types_coercion();
  f += test_lang53_ops();
  f += test_lang53_statements();
  f += test_lang53_tables_meta();
  f += test_lang53_functions();
  f += test_lang53_visibility_goto();
  f += test_lang53_errors_coro();
  f += test_lang53_baselib();
  return f ? 1 : 0;
}
