#pragma once

#include "runtime/closure.hpp"

#include <string>

namespace lj3 {

std::string dump_proto(Proto* p, bool strip);
Proto* undump_proto(State* L, const std::string& blob, const std::string& name);

bool is_proto_dump(std::string_view blob);

} // namespace lj3
