#pragma once

#include "runtime/value.hpp"

namespace luatier {

struct State;

// Look up metamethod name (e.g. "__index") on object's metatable. Returns nil if absent.
TValue get_metamethod(State* L, const TValue& obj, const char* name);

// Table-length per Lua 5.3 border rules (approximate: find largest n with t[n]~=nil and t[n+1]==nil
// for array part; then check hash integers).
int64_t table_length(Table* t);

// Index / newindex with metamethod fallback.
TValue meta_index(State* L, const TValue& table, const TValue& key);
void meta_newindex(State* L, const TValue& table, const TValue& key, const TValue& value);

// Binary / unary ops with metamethod fallback. Returns true if result written to *out.
bool meta_arith(State* L, const char* mt_name, const TValue& a, const TValue& b, TValue* out);
bool meta_unary(State* L, const char* mt_name, const TValue& a, TValue* out);
bool meta_eq(State* L, const TValue& a, const TValue& b, bool* out_eq);
bool meta_lt(State* L, const TValue& a, const TValue& b, bool* out_lt);
bool meta_le(State* L, const TValue& a, const TValue& b, bool* out_le);
bool meta_concat(State* L, const TValue& a, const TValue& b, TValue* out);
bool meta_len(State* L, const TValue& a, TValue* out);

// Call value; if not function, try __call. Pushes results according to nresults.
// Stack: func at func_idx, nargs args after it. Returns status.
int meta_call(State* L, int func_idx, int nargs, int nresults);

Table* get_metatable(State* L, const TValue& v);
void set_metatable(State* L, TValue& v, Table* mt);

} // namespace luatier
