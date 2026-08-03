#pragma once

#include "runtime/closure.hpp"

#include <string>

namespace luatier {

std::string dump_proto(Proto* p, bool strip);
Proto* undump_proto(State* L, const std::string& blob, const std::string& name);

// True if blob looks like a binary chunk (PUC: first byte is LUA_SIGNATURE[0]).
bool is_proto_dump(std::string_view blob);

} // namespace luatier
