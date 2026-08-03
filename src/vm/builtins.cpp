#include "vm/builtins.hpp"

#include "lib/libs.hpp"

namespace luatier {

void open_base(State* L) { open_libs(L); }

} // namespace luatier
