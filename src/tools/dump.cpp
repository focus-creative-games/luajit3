#include "tools/dump.hpp"

#include "runtime/value.hpp"
#include "vm/bytecode.hpp"

#include <iostream>
#include <sstream>

namespace luatier {

std::string dump_proto(const Proto* p) {
  std::ostringstream os;
  os << ".proto source=" << p->source << " maxstack=" << p->maxstack
     << " params=" << p->numparams << "\n";
  for (size_t i = 0; i < p->upvalues.size(); ++i)
    os << "  upvalue[" << i << "] " << p->upvalues[i].name
       << " instack=" << p->upvalues[i].instack << " idx=" << static_cast<int>(p->upvalues[i].idx)
       << "\n";
  for (size_t i = 0; i < p->constants.size(); ++i)
    os << "  k[" << i << "] " << value_to_string(p->constants[i]) << "\n";
  for (size_t i = 0; i < p->code.size(); ++i)
    os << "  [" << i << "] L" << (i < p->lineinfo.size() ? p->lineinfo[i] : -1) << " "
       << disassemble_ins(p->code[i]) << "\n";
  for (size_t i = 0; i < p->locvars.size(); ++i)
    os << "  local[" << i << "] " << p->locvars[i].name << " r" << p->locvars[i].reg << " pc["
       << p->locvars[i].startpc << "," << p->locvars[i].endpc << ")\n";
  for (auto* ch : p->protos)
    os << dump_proto(ch);
  return os.str();
}

void dump_proto_to_stderr(const Proto* p) { std::cerr << dump_proto(p); }

} // namespace luatier
