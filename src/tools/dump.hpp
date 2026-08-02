#pragma once

#include "runtime/closure.hpp"

#include <string>

namespace lj3 {

std::string dump_proto(const Proto* p);
void dump_proto_to_stderr(const Proto* p);

} // namespace lj3
