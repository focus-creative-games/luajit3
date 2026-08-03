#include "vm/ldebug.hpp"

#include "common/common.hpp"
#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "vm/bytecode.hpp"
#include "vm/meta.hpp"

#include <cstring>
#include <string>

namespace luatier {
namespace {

constexpr int kIdSize = 60;
constexpr const char* kEnvName = "_ENV";

bool sets_register_a(OpCode op) {
  switch (op) {
  case OpCode::MOVE:
  case OpCode::LOADBOOL:
  case OpCode::LOADINT:
  case OpCode::LOADK:
  case OpCode::LOADFLOAT:
  case OpCode::GETUPVAL:
  case OpCode::GETTABUP:
  case OpCode::GETTABLE:
  case OpCode::GETFIELD:
  case OpCode::GETI:
  case OpCode::NEWTABLE:
  case OpCode::ADD:
  case OpCode::SUB:
  case OpCode::MUL:
  case OpCode::DIV:
  case OpCode::IDIV:
  case OpCode::MOD:
  case OpCode::POW:
  case OpCode::BAND:
  case OpCode::BOR:
  case OpCode::BXOR:
  case OpCode::SHL:
  case OpCode::SHR:
  case OpCode::UNM:
  case OpCode::BNOT:
  case OpCode::NOT:
  case OpCode::LEN:
  case OpCode::CONCAT:
  case OpCode::CLOSURE:
  case OpCode::SELF:
  case OpCode::TESTSET:
    return true;
  default:
    return false;
  }
}

int filter_pc(int pc, int jmptarget) { return (pc < jmptarget) ? -1 : pc; }

int findsetreg(Proto* p, int lastpc, int reg) {
  int setreg = -1;
  int jmptarget = 0;
  for (int pc = 0; pc < lastpc; ++pc) {
    Instruction i = p->code[static_cast<size_t>(pc)];
    OpCode op = static_cast<OpCode>(op_get(i));
    int a = op_a(i);
    switch (op) {
    case OpCode::LOADNIL: {
      int b = op_b(i);
      if (a <= reg && reg <= a + b)
        setreg = filter_pc(pc, jmptarget);
      break;
    }
    case OpCode::TFORCALL:
      if (reg >= a + 2)
        setreg = filter_pc(pc, jmptarget);
      break;
    case OpCode::CALL:
    case OpCode::TAILCALL:
      if (reg >= a)
        setreg = filter_pc(pc, jmptarget);
      break;
    case OpCode::JMP: {
      int dest = pc + 1 + op_sbx(i);
      if (pc < dest && dest <= lastpc && dest > jmptarget)
        jmptarget = dest;
      break;
    }
    default:
      if (sets_register_a(op) && reg == a)
        setreg = filter_pc(pc, jmptarget);
      break;
    }
  }
  return setreg;
}

const char* getobjname_impl(Proto* p, int lastpc, int reg, const char** name, int depth);

void kname_reg(Proto* p, int pc, int reg, const char** name) {
  const char* what = getobjname_impl(p, pc, reg, name, 0);
  if (what && std::strcmp(what, "constant") == 0)
    return;
  *name = "?";
}

void kname_k(Proto* p, int kidx, const char** name) {
  if (kidx >= 0 && static_cast<size_t>(kidx) < p->constants.size() &&
      p->constants[static_cast<size_t>(kidx)].is_string()) {
    *name = p->constants[static_cast<size_t>(kidx)].as_string()->view().data();
    return;
  }
  *name = "?";
}

const char* getobjname_impl(Proto* p, int lastpc, int reg, const char** name, int depth) {
  *name = debug_local_name(p, reg, lastpc);
  if (*name)
    return "local";

  const int pc = findsetreg(p, lastpc, reg);
  if (pc < 0)
    return nullptr;

  Instruction i = p->code[static_cast<size_t>(pc)];
  OpCode op = static_cast<OpCode>(op_get(i));
  switch (op) {
  case OpCode::MOVE: {
    int b = op_b(i);
    if (b != op_a(i) && depth < 8)
      return getobjname_impl(p, pc, b, name, depth + 1);
    break;
  }
  case OpCode::GETTABUP:
  case OpCode::GETTABLE:
  case OpCode::GETFIELD: {
    const char* vn = nullptr;
    if (op == OpCode::GETTABUP) {
      vn = debug_upval_name(p, op_b(i));
    } else {
      // Follow temps / MOVEs so `_ENV` loaded into a register still counts
      // as a global (needed when many constants force non-RK paths).
      const char* what = getobjname_impl(p, pc, op_b(i), &vn, depth + 1);
      if (!what)
        vn = debug_local_name(p, op_b(i), pc);
    }

    if (op == OpCode::GETTABLE)
      kname_reg(p, pc, op_c(i), name);
    else
      kname_k(p, op_c(i), name);

    if (vn && std::strcmp(vn, kEnvName) == 0)
      return "global";
    return "field";
  }
  case OpCode::GETUPVAL:
    *name = debug_upval_name(p, op_b(i));
    return "upvalue";
  case OpCode::LOADK:
  case OpCode::LOADFLOAT: {
    int k = op_bx(i);
    if (k >= 0 && static_cast<size_t>(k) < p->constants.size() &&
        p->constants[static_cast<size_t>(k)].is_string()) {
      *name = p->constants[static_cast<size_t>(k)].as_string()->view().data();
      return "constant";
    }
    break;
  }
  case OpCode::SELF:
    kname_reg(p, pc, op_c(i), name);
    return "method";
  default:
    break;
  }
  return nullptr;
}

CallFrame* current_lua_frame(State* L) {
  Thread* th = L->current;
  for (int i = static_cast<int>(th->frames.size()) - 1; i >= 0; --i) {
    CallFrame& fr = th->frames[static_cast<size_t>(i)];
    if (fr.proto && fr.cl && !fr.cl->is_c)
      return &fr;
  }
  return nullptr;
}

int current_pc(const CallFrame& fr) {
  int pc = fr.saved_pc - 1;
  return pc < 0 ? 0 : pc;
}

} // namespace

std::string format_chunkid(std::string_view source) {
  // Match luaO_chunkid with LUA_IDSIZE==60; visible length is at most 59.
  constexpr size_t kMax = 59;
  std::string out;
  out.reserve(kMax);

  if (!source.empty() && source[0] == '=') {
    size_t len = source.size() - 1;
    if (len <= kMax)
      out.assign(source.substr(1));
    else
      out.assign(source.substr(1, kMax));
    return out;
  }
  if (!source.empty() && source[0] == '@') {
    size_t len = source.size() - 1;
    if (len <= kMax) {
      out.assign(source.substr(1));
    } else {
      out.append("...");
      out.append(source.substr(source.size() - (kMax - 3)));
    }
    return out;
  }
  // [string "source"]  — prefix 9 + suffix 2 = 11; "..." adds 3 when truncated.
  out.append("[string \"");
  constexpr size_t wrap = 11;
  constexpr size_t dots = 3;
  size_t room = kMax - wrap;          // 48 if fits without dots
  size_t room_trunc = kMax - wrap - dots; // 45 with dots
  size_t len = source.size();
  size_t nl = source.find('\n');
  if (nl != std::string_view::npos)
    len = nl;
  if (len <= room && nl == std::string_view::npos) {
    out.append(source.substr(0, len));
  } else {
    if (len > room_trunc)
      len = room_trunc;
    out.append(source.substr(0, len));
    out.append("...");
  }
  out.append("\"]");
  return out;
}

const char* debug_local_name(Proto* p, int reg, int pc) {
  for (const auto& lv : p->locvars) {
    if (lv.reg == reg && lv.startpc <= pc && pc < lv.endpc)
      return lv.name.c_str();
  }
  return nullptr;
}

const char* debug_upval_name(Proto* p, int uv) {
  if (uv < 0 || static_cast<size_t>(uv) >= p->upvalues.size())
    return "?";
  const std::string& n = p->upvalues[static_cast<size_t>(uv)].name;
  return n.empty() ? "?" : n.c_str();
}

const char* debug_getobjname(Proto* p, int lastpc, int reg, const char** name) {
  return getobjname_impl(p, lastpc, reg, name, 0);
}

const char* debug_funcnamefromcode(CallFrame* caller, const char** name) {
  if (!caller)
    return nullptr;
  if (caller->hooked) {
    *name = "?";
    return "hook";
  }
  if (!caller->proto || !caller->cl || caller->cl->is_c)
    return nullptr;

  Proto* p = caller->proto;
  const int pc = caller->saved_pc - 1;
  if (pc < 0 || pc >= static_cast<int>(p->code.size()))
    return nullptr;

  Instruction i = p->code[static_cast<size_t>(pc)];
  OpCode op = static_cast<OpCode>(op_get(i));
  switch (op) {
  case OpCode::CALL:
  case OpCode::TAILCALL:
    return getobjname_impl(p, pc, op_a(i), name, 0);
  case OpCode::TFORCALL:
    *name = "for iterator";
    return "for iterator";
  case OpCode::SELF:
  case OpCode::GETTABUP:
  case OpCode::GETTABLE:
  case OpCode::GETFIELD:
  case OpCode::GETI:
    *name = "__index";
    return "metamethod";
  case OpCode::SETTABUP:
  case OpCode::SETTABLE:
  case OpCode::SETFIELD:
  case OpCode::SETI:
    *name = "__newindex";
    return "metamethod";
  case OpCode::ADD: *name = "__add"; return "metamethod";
  case OpCode::SUB: *name = "__sub"; return "metamethod";
  case OpCode::MUL: *name = "__mul"; return "metamethod";
  case OpCode::MOD: *name = "__mod"; return "metamethod";
  case OpCode::POW: *name = "__pow"; return "metamethod";
  case OpCode::DIV: *name = "__div"; return "metamethod";
  case OpCode::IDIV: *name = "__idiv"; return "metamethod";
  case OpCode::BAND: *name = "__band"; return "metamethod";
  case OpCode::BOR: *name = "__bor"; return "metamethod";
  case OpCode::BXOR: *name = "__bxor"; return "metamethod";
  case OpCode::SHL: *name = "__shl"; return "metamethod";
  case OpCode::SHR: *name = "__shr"; return "metamethod";
  case OpCode::UNM: *name = "__unm"; return "metamethod";
  case OpCode::BNOT: *name = "__bnot"; return "metamethod";
  case OpCode::LEN: *name = "__len"; return "metamethod";
  case OpCode::CONCAT: *name = "__concat"; return "metamethod";
  case OpCode::EQ: *name = "__eq"; return "metamethod";
  case OpCode::LT: *name = "__lt"; return "metamethod";
  case OpCode::LE: *name = "__le"; return "metamethod";
  default:
    return nullptr;
  }
}

std::string obj_type_name(State* L, const TValue& v) {
  Table* mt = get_metatable(L, v);
  if (mt) {
    TValue name = mt->get(TValue::obj(ValueTag::String, L->intern("__name")));
    if (name.is_string())
      return std::string(name.as_string()->view());
  }
  switch (v.tag()) {
  case ValueTag::Nil: return "nil";
  case ValueTag::Bool: return "boolean";
  case ValueTag::Int:
  case ValueTag::Float: return "number";
  case ValueTag::String: return "string";
  case ValueTag::Table: return "table";
  case ValueTag::Function: return "function";
  case ValueTag::Userdata:
  case ValueTag::LightUserdata: return "userdata";
  case ValueTag::Thread: return "thread";
  default: return "value";
  }
}

std::string varinfo_reg(State* L, int reg) {
  CallFrame* fr = current_lua_frame(L);
  if (!fr || !fr->proto)
    return "";
  if (reg < 0)
    reg = L->current->err_reg;
  if (reg < 0)
    return "";
  const char* name = nullptr;
  const char* kind = getobjname_impl(fr->proto, current_pc(*fr), reg, &name, 0);
  if (!kind || !name)
    return "";
  // PUC luaG_varinfo: only these kinds get a suffix.
  if (std::strcmp(kind, "global") != 0 && std::strcmp(kind, "local") != 0 &&
      std::strcmp(kind, "field") != 0 && std::strcmp(kind, "method") != 0 &&
      std::strcmp(kind, "upvalue") != 0)
    return "";
  return std::string(" (") + kind + " '" + name + "')";
}

std::string varinfo_abs(State* L, int abs_index) {
  CallFrame* fr = current_lua_frame(L);
  if (!fr)
    return "";
  return varinfo_reg(L, abs_index - fr->base);
}

[[noreturn]] void runerror(State* L, const std::string& msg) {
  std::string full = msg;
  CallFrame* fr = current_lua_frame(L);
  if (fr && fr->proto) {
    int pc = current_pc(*fr);
    int line = -1;
    if (pc >= 0 && pc < static_cast<int>(fr->proto->lineinfo.size()))
      line = fr->proto->lineinfo[static_cast<size_t>(pc)];
    if (line > 0)
      full = format_chunkid(fr->proto->source) + ":" + std::to_string(line) + ": " + msg;
    else if (fr->proto->lineinfo.empty())
      // Stripped debug info (string.dump(..., true)): PUC uses "?:-1:".
      full = "?:-1: " + msg;
  }
  L->current->err_obj = TValue::obj(ValueTag::String, L->intern(full));
  L->current->err_obj_set = true;
  panic(full);
}

[[noreturn]] void typeerror(State* L, const TValue& v, int reg, const char* op) {
  runerror(L, "attempt to " + std::string(op) + " a " + obj_type_name(L, v) + " value" +
                  varinfo_reg(L, reg));
}

[[noreturn]] void typerror_no_reg(State* L, const TValue& v, const char* op) {
  runerror(L, "attempt to " + std::string(op) + " a " + obj_type_name(L, v) + " value");
}

[[noreturn]] void compareerror(State* L, const TValue& a, const TValue& b) {
  std::string ta = obj_type_name(L, a);
  std::string tb = obj_type_name(L, b);
  if (ta == tb)
    runerror(L, "attempt to compare two " + ta + " values");
  runerror(L, "attempt to compare " + ta + " with " + tb);
}

[[noreturn]] void opinterror(State* L, const TValue& v, int reg, const char* msg) {
  runerror(L, std::string(msg) + obj_type_name(L, v) + " value" + varinfo_reg(L, reg));
}

[[noreturn]] void tointerror(State* L, const TValue& v, int reg) {
  (void)v;
  runerror(L, "number has no integer representation" + varinfo_reg(L, reg));
}

} // namespace luatier
