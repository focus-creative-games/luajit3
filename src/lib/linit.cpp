#include "lib/libs.hpp"

#include "runtime/value.hpp"

namespace luatier {

static void mark_loaded_global(State* L, const char* name) {
  TValue v = L->globals->get(TValue::obj(ValueTag::String, L->intern(name)));
  if (!v.is_nil())
    package_set_loaded(L, name, v);
}

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
  // PUC luaL_requiref: stdlibs are already present in package.loaded.
  mark_loaded_global(L, "coroutine");
  mark_loaded_global(L, "package");
  mark_loaded_global(L, "string");
  mark_loaded_global(L, "table");
  mark_loaded_global(L, "math");
  mark_loaded_global(L, "utf8");
  mark_loaded_global(L, "io");
  mark_loaded_global(L, "os");
  mark_loaded_global(L, "debug");
  L->gc.set_running(was_running);
}

} // namespace luatier
