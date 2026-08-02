#pragma once

#include "common/common.hpp"

#include <cstdint>
#include <string>

namespace lj3 {

using Instruction = uint32_t;

enum class OpCode : uint8_t {
  MOVE = 0,
  LOADNIL,
  LOADBOOL,
  LOADINT,
  LOADFLOAT,
  LOADK,
  LOADKX,
  EXTRAARG,
  ADD,
  SUB,
  MUL,
  DIV,
  IDIV,
  MOD,
  POW,
  BAND,
  BOR,
  BXOR,
  SHL,
  SHR,
  UNM,
  BNOT,
  NOT,
  LEN,
  CONCAT,
  EQ,
  LT,
  LE,
  TEST,
  TESTSET,
  NEWTABLE,
  GETTABLE,
  SETTABLE,
  GETFIELD,
  SETFIELD,
  GETI,
  SETI,
  GETTABUP,
  SETTABUP,
  SETLIST,
  CLOSURE,
  GETUPVAL,
  SETUPVAL,
  CALL,
  TAILCALL,
  RETURN,
  VARARG,
  SELF,
  JMP,
  FORPREP,
  FORLOOP,
  TFORCALL,
  TFORLOOP,
  CHECKGC,
  SAFEPOINT,
  OP_COUNT
};

inline uint8_t op_get(Instruction i) { return static_cast<uint8_t>(i & 0xFF); }
inline uint8_t op_a(Instruction i) { return static_cast<uint8_t>((i >> 8) & 0xFF); }
inline uint8_t op_b(Instruction i) { return static_cast<uint8_t>((i >> 16) & 0xFF); }
inline uint8_t op_c(Instruction i) { return static_cast<uint8_t>((i >> 24) & 0xFF); }
inline uint16_t op_bx(Instruction i) { return static_cast<uint16_t>((i >> 16) & 0xFFFF); }
inline int op_sbx(Instruction i) { return static_cast<int>(op_bx(i)) - 32768; }
inline uint32_t op_ax(Instruction i) { return i >> 8; }

inline Instruction encode_abc(OpCode op, uint8_t a, uint8_t b, uint8_t c) {
  return static_cast<uint8_t>(op) | (a << 8) | (b << 16) | (c << 24);
}
inline Instruction encode_abx(OpCode op, uint8_t a, uint16_t bx) {
  return static_cast<uint8_t>(op) | (a << 8) | (static_cast<uint32_t>(bx) << 16);
}
inline Instruction encode_asbx(OpCode op, uint8_t a, int sbx) {
  return encode_abx(op, a, static_cast<uint16_t>(sbx + 32768));
}
inline Instruction encode_ax(OpCode op, uint32_t ax) {
  return static_cast<uint8_t>(op) | ((ax & 0xFFFFFF) << 8);
}

struct UpvalDesc {
  bool instack = false;
  uint8_t idx = 0;
  std::string name;
};

const char* opcode_name(OpCode op);
std::string disassemble_ins(Instruction i);

} // namespace lj3
