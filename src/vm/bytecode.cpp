#include "vm/bytecode.hpp"

namespace lj3 {

const char* opcode_name(OpCode op) {
  switch (op) {
  case OpCode::MOVE: return "MOVE";
  case OpCode::LOADNIL: return "LOADNIL";
  case OpCode::LOADBOOL: return "LOADBOOL";
  case OpCode::LOADINT: return "LOADINT";
  case OpCode::LOADFLOAT: return "LOADFLOAT";
  case OpCode::LOADK: return "LOADK";
  case OpCode::ADD: return "ADD";
  case OpCode::SUB: return "SUB";
  case OpCode::MUL: return "MUL";
  case OpCode::DIV: return "DIV";
  case OpCode::IDIV: return "IDIV";
  case OpCode::MOD: return "MOD";
  case OpCode::POW: return "POW";
  case OpCode::UNM: return "UNM";
  case OpCode::NOT: return "NOT";
  case OpCode::LEN: return "LEN";
  case OpCode::CONCAT: return "CONCAT";
  case OpCode::EQ: return "EQ";
  case OpCode::LT: return "LT";
  case OpCode::LE: return "LE";
  case OpCode::TEST: return "TEST";
  case OpCode::JMP: return "JMP";
  case OpCode::CALL: return "CALL";
  case OpCode::RETURN: return "RETURN";
  case OpCode::CLOSURE: return "CLOSURE";
  case OpCode::GETUPVAL: return "GETUPVAL";
  case OpCode::SETUPVAL: return "SETUPVAL";
  case OpCode::GETTABUP: return "GETTABUP";
  case OpCode::SETTABUP: return "SETTABUP";
  case OpCode::GETTABLE: return "GETTABLE";
  case OpCode::SETTABLE: return "SETTABLE";
  case OpCode::NEWTABLE: return "NEWTABLE";
  case OpCode::FORPREP: return "FORPREP";
  case OpCode::FORLOOP: return "FORLOOP";
  case OpCode::TFORCALL: return "TFORCALL";
  case OpCode::TFORLOOP: return "TFORLOOP";
  case OpCode::VARARG: return "VARARG";
  case OpCode::SETLIST: return "SETLIST";
  case OpCode::GETFIELD: return "GETFIELD";
  case OpCode::SETFIELD: return "SETFIELD";
  default: return "OP";
  }
}

std::string disassemble_ins(Instruction i) {
  auto op = static_cast<OpCode>(op_get(i));
  return std::string(opcode_name(op)) + " " + std::to_string(op_a(i)) + " " +
         std::to_string(op_b(i)) + " " + std::to_string(op_c(i));
}

} // namespace lj3
