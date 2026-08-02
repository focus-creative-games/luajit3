#include "tools/dump.hpp"

#include "vm/bytecode.hpp"

#include <iostream>
#include <sstream>

namespace lj3 {

std::string dump_proto(const Proto* p) {
  std::ostringstream os;
  os << ".proto source=" << p->source << " maxstack=" << p->maxstack
     << " params=" << p->numparams << "\n";
  for (size_t i = 0; i < p->code.size(); ++i)
    os << "  [" << i << "] " << disassemble_ins(p->code[i]) << "\n";
  for (auto* ch : p->protos)
    os << dump_proto(ch);
  return os.str();
}

void dump_proto_to_stderr(const Proto* p) { std::cerr << dump_proto(p); }

} // namespace lj3
