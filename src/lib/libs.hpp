#pragma once

#include "vm/state.hpp"

namespace luatier {

void open_base_lib(State* L);
void open_coroutine_lib(State* L);
void open_package_lib(State* L);
void open_string_lib(State* L);
void open_table_lib(State* L);
void open_math_lib(State* L);
void open_utf8_lib(State* L);
void open_io_lib(State* L);
void open_os_lib(State* L);
void open_debug_lib(State* L);

// Register a standard library in package.loaded (PUC luaL_requiref).
void package_set_loaded(State* L, const char* name, const TValue& mod);

// Re-apply package.path / package.cpath from env (or defaults if registry.LUA_NOENV).
void package_reapply_paths(State* L);

void open_libs(State* L);

} // namespace luatier
