#pragma once

#include "runtime/value.hpp"
#include "vm/state.hpp"

#include <string>
#include <string_view>

namespace luatier {

// luaO_chunkid
std::string format_chunkid(std::string_view source);

// luaT_objtypename (+ __name)
std::string obj_type_name(State* L, const TValue& v);

// Symbolic name of register at current PC: " (global 'x')" / "" etc.
std::string varinfo_reg(State* L, int reg);
// Absolute stack slot → frame-relative register for varinfo.
std::string varinfo_abs(State* L, int abs_index);

// Prefix with "source:line: " when possible (luaG_addinfo).
[[noreturn]] void runerror(State* L, const std::string& msg);

[[noreturn]] void typeerror(State* L, const TValue& v, int reg, const char* op);
[[noreturn]] void typerror_no_reg(State* L, const TValue& v, const char* op);

// Compare / binary op helpers.
[[noreturn]] void compareerror(State* L, const TValue& a, const TValue& b);
[[noreturn]] void opinterror(State* L, const TValue& v, int reg, const char* msg);
[[noreturn]] void tointerror(State* L, const TValue& v, int reg);

// Exported for debug.getinfo name resolution.
const char* debug_getobjname(Proto* p, int lastpc, int reg, const char** name);
const char* debug_funcnamefromcode(CallFrame* caller, const char** name);
const char* debug_local_name(Proto* p, int reg, int pc);
const char* debug_upval_name(Proto* p, int uv);

} // namespace luatier
