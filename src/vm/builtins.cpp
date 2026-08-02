#include "vm/builtins.hpp"

#include "lib/libs.hpp"

namespace lj3 {

void open_base(State* L) { open_libs(L); }

} // namespace lj3
