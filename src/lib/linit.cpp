#include "lib/libs.hpp"

namespace lj3 {

void open_libs(State* L) {
  // Library tables are built as C locals before being published to _G; pause
  // automatic GC so incremental collection cannot sweep them mid-construction.
  bool was_running = L->gc.is_running();
  L->gc.set_running(false);
  open_base_lib(L);
  open_coroutine_lib(L);
  open_package_lib(L);
  open_string_lib(L);
  open_table_lib(L);
  open_math_lib(L);
  open_utf8_lib(L);
  open_io_lib(L);
  open_os_lib(L);
  open_debug_lib(L);
  L->gc.set_running(was_running);
}

} // namespace lj3
