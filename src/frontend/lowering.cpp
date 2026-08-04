#include "frontend/lowering.hpp"

#include "common/common.hpp"
#include "runtime/string.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace luatier {

namespace {

struct FuncState;
[[noreturn]] void error_limit(FuncState& fs, int limit, const char* what);
void check_limit(FuncState& fs, int v, int limit, const char* what);

struct Local {
  std::string name;
  int reg = 0;
  int startpc = 0;
  bool active = true;
  bool captured = false;
};

struct FuncState {
  State* L = nullptr;
  FuncState* prev = nullptr;
  Proto* proto = nullptr;
  std::vector<Local> locals;
  int nactvar = 0;
  int freereg = 0;
  int maxstack = 0;
  int reg_hwm = 0; // high-water mark; never reuse regs below this after captures
  std::vector<int> break_list;
  // Innermost loop's first local register (break closes captured locals >= this).
  std::vector<int> loop_break_level;
  struct LabelInfo {
    int pc = 0;
    int nactvar = 0; // active locals at label (PUC Labeldesc.nactvar)
  };
  std::unordered_map<std::string, LabelInfo> labels;
  struct PendingGoto {
    std::string name;
    int jmp_pc = 0;
    int nactvar = 0; // actvar count at goto site
  };
  std::vector<PendingGoto> pending_gotos;
  // Block-entry nactvar stack for "last label" rule (exclude block locals).
  std::vector<int> block_entry_nactvar;
  bool vararg = false;
  int lastline = 1;

  static int goto_close_a(int goto_nactvar, int label_nactvar) {
    return (goto_nactvar > label_nactvar) ? (label_nactvar + 1) : 0;
  }

  int pc() const { return static_cast<int>(proto->code.size()); }
  void emit(Instruction i, int line = -1) {
    if (line < 0)
      line = lastline;
    proto->code.push_back(i);
    proto->lineinfo.push_back(line);
  }
  int code_abc(OpCode op, int a, int b, int c, int line = -1) {
    emit(encode_abc(op, static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c)),
         line);
    return pc() - 1;
  }
  int code_abx(OpCode op, int a, int bx, int line = -1) {
    emit(encode_abx(op, static_cast<uint8_t>(a), static_cast<uint16_t>(bx)), line);
    return pc() - 1;
  }
  int code_asbx(OpCode op, int a, int sbx, int line = -1) {
    emit(encode_asbx(op, static_cast<uint8_t>(a), sbx), line);
    return pc() - 1;
  }
  void fix_sbx(int pc, int dest) {
    int sbx = dest - (pc + 1);
    auto op = static_cast<OpCode>(op_get(proto->code[pc]));
    proto->code[pc] = encode_asbx(op, op_a(proto->code[pc]), sbx);
  }
  void reserve(int n) {
    freereg += n;
    reg_hwm = std::max(reg_hwm, freereg);
    maxstack = std::max(maxstack, freereg);
    check_limit(*this, freereg, MAXSTACK - 1, "registers");
  }
  int new_reg() {
    int r = freereg;
    reserve(1);
    return r;
  }
  void free_reg(int r) {
    if (r >= nactvar)
      freereg = r;
  }
  int const_index(const TValue& v) {
    // Bit-identical match so +0.0 and -0.0 stay distinct (values_equal uses IEEE ==).
    for (size_t i = 0; i < proto->constants.size(); ++i) {
      const TValue& k = proto->constants[i];
      if (k.type == v.type && k.payload == v.payload && k.aux == v.aux)
        return static_cast<int>(i);
    }
    proto->constants.push_back(v);
    return static_cast<int>(proto->constants.size() - 1);
  }
  int string_k(const std::string& s) {
    return const_index(TValue::obj(ValueTag::String, L->intern(s)));
  }
  int push_local(const std::string& name, int reg, int line) {
    check_limit(*this, nactvar + 1, MAXVARS, "local variables");
    Local loc;
    loc.name = name;
    loc.reg = reg;
    loc.startpc = pc();
    locals.push_back(loc);
    nactvar = std::max(nactvar, reg + 1);
    freereg = std::max(freereg, nactvar);
    (void)line;
    return static_cast<int>(locals.size() - 1);
  }
  // Record active locals into proto->locvars without emitting clear/close.
  void flush_locvars_to(int nlocals_before) {
    std::vector<LocVar> closing;
    while (static_cast<int>(locals.size()) > nlocals_before) {
      auto& loc = locals.back();
      LocVar lv;
      lv.name = loc.name;
      lv.reg = loc.reg;
      lv.startpc = loc.startpc;
      lv.endpc = pc();
      closing.push_back(lv);
      locals.pop_back();
    }
    for (auto it = closing.rbegin(); it != closing.rend(); ++it)
      proto->locvars.push_back(*it);
    nactvar = 0;
    for (auto& loc : locals)
      nactvar = std::max(nactvar, loc.reg + 1);
  }

  int captured_close_level() const {
    int close_level = -1;
    for (auto& l : locals) {
      if (l.active && l.captured && (close_level < 0 || l.reg < close_level))
        close_level = l.reg;
    }
    return close_level;
  }

  // clear_dead: when true, emit LOADNIL for dead locals (debug cleanliness).
  // For-loops pass false so instruction counts match PUC (count hooks / db.lua).
  void leave_block(int nlocals_before, bool clear_dead = true) {
    int close_level = -1;
    int nil_from = -1;
    int nil_to = -1;
    std::vector<LocVar> closing;
    while (static_cast<int>(locals.size()) > nlocals_before) {
      auto& loc = locals.back();
      if (loc.captured) {
        if (close_level < 0 || loc.reg < close_level)
          close_level = loc.reg;
      } else {
        if (nil_from < 0 || loc.reg < nil_from)
          nil_from = loc.reg;
        if (nil_to < 0 || loc.reg > nil_to)
          nil_to = loc.reg;
      }
      LocVar lv;
      lv.name = loc.name;
      lv.reg = loc.reg;
      lv.startpc = loc.startpc;
      lv.endpc = pc();
      closing.push_back(lv);
      locals.pop_back();
    }
    for (auto it = closing.rbegin(); it != closing.rend(); ++it)
      proto->locvars.push_back(*it);
    if (clear_dead && nil_from >= 0 && close_level < 0) {
      // Clear through the high-water mark of temporaries used in this block.
      // freereg may already have been rewound to nactvar, leaving dirty temps.
      int clear_to = std::max(nil_to, reg_hwm - 1);
      if (clear_to >= nil_from)
        code_abc(OpCode::LOADNIL, nil_from, clear_to - nil_from, 0);
    }
    nactvar = 0;
    for (auto& l : locals)
      if (l.active)
        nactvar = std::max(nactvar, l.reg + 1);
    // Never reuse register slots for the rest of this function once any
    // captured local has lived there (closed upvalues must not alias new locals).
    // Skip close JMP after a terminal RETURN/TAILCALL — those paths close (or
    // never fall through). A trailing JMP after TAILCALL is dead but confusing.
    bool terminal = false;
    if (!proto->code.empty()) {
      auto op = static_cast<OpCode>(op_get(proto->code.back()));
      terminal = (op == OpCode::RETURN || op == OpCode::TAILCALL);
    }
    if (close_level >= 0 && !terminal) {
      code_asbx(OpCode::JMP, close_level + 1, 0);
      freereg = std::max(reg_hwm, nactvar);
    } else {
      freereg = nactvar;
    }
    reg_hwm = std::max(reg_hwm, freereg);
  }
  int find_local(const std::string& name) {
    for (int i = static_cast<int>(locals.size()) - 1; i >= 0; --i)
      if (locals[i].active && locals[i].name == name)
        return locals[i].reg;
    return -1;
  }
};

[[noreturn]] void error_limit(FuncState& fs, int limit, const char* what) {
  int line = fs.proto ? fs.proto->linedefined : 0;
  std::string where =
      (line == 0) ? "main function" : ("function at line " + std::to_string(line));
  panic(std::string("too many ") + what + " (limit is " + std::to_string(limit) + ") in " +
        where);
}

void check_limit(FuncState& fs, int v, int limit, const char* what) {
  if (v > limit)
    error_limit(fs, limit, what);
}

struct Expdesc {
  enum Kind { Void, Relocable, Nonrelocable, Constant, Local, Upval, Global, Indexed, Call } kind =
      Void;
  // For Indexed: how `key` is encoded in GET*/SET*.
  enum class IndexKey : uint8_t { Reg, IntImm, ConstStr };
  int info = 0;
  int t = -1; // patch list true
  int f = -1; // patch list false
  int reg = -1;
  TValue k{};
  int table = -1;
  int key = -1;
  bool key_is_rk = false;
  IndexKey index_key = IndexKey::Reg;
};

// True if constant is an integer (or integer-valued float) in [0, 255].
bool const_uint8_index(const TValue& v, int* out) {
  int64_t i = 0;
  if (v.is_int()) {
    i = v.as_int();
  } else if (v.is_float()) {
    double d = v.as_float();
    if (d != d || d != std::floor(d))
      return false;
    if (d < 0.0 || d > 255.0)
      return false;
    i = static_cast<int64_t>(d);
  } else {
    return false;
  }
  if (i < 0 || i > 255)
    return false;
  *out = static_cast<int>(i);
  return true;
}

void emit_get_index(FuncState& fs, int dest, const Expdesc& idx, int line = -1) {
  if (idx.index_key == Expdesc::IndexKey::IntImm)
    fs.code_abc(OpCode::GETI, dest, idx.table, idx.key, line);
  else if (idx.index_key == Expdesc::IndexKey::ConstStr)
    fs.code_abc(OpCode::GETFIELD, dest, idx.table, idx.key, line);
  else
    fs.code_abc(OpCode::GETTABLE, dest, idx.table, idx.key, line);
}

void emit_set_index(FuncState& fs, const Expdesc& idx, int val_reg, int line = -1) {
  if (idx.index_key == Expdesc::IndexKey::IntImm)
    fs.code_abc(OpCode::SETI, idx.table, idx.key, val_reg, line);
  else if (idx.index_key == Expdesc::IndexKey::ConstStr)
    fs.code_abc(OpCode::SETFIELD, idx.table, idx.key, val_reg, line);
  else
    fs.code_abc(OpCode::SETTABLE, idx.table, idx.key, val_reg, line);
}

void expr(FuncState& fs, Expdesc& e, Expr& node);
void statement(FuncState& fs, AstNode& node);
void block(FuncState& fs, Block& b, bool allow_last_label = true, bool close = true);

// Rewrite the most recent CALL as TAILCALL (return f(...)).
bool patch_last_call_to_tailcall(FuncState& fs) {
  auto& code = fs.proto->code;
  for (int pi = static_cast<int>(code.size()) - 1; pi >= 0; --pi) {
    if (static_cast<OpCode>(op_get(code[static_cast<size_t>(pi)])) != OpCode::CALL)
      continue;
    uint8_t ca = op_a(code[static_cast<size_t>(pi)]);
    uint8_t cb = op_b(code[static_cast<size_t>(pi)]);
    code[static_cast<size_t>(pi)] = encode_abc(OpCode::TAILCALL, ca, cb, 0);
    return true;
  }
  return false;
}

// Patch the most recent CALL's C field. nresults==-1 means LUA_MULTRET (C=0).
uint8_t patch_last_call_results(FuncState& fs, int nresults) {
  auto& code = fs.proto->code;
  for (int pi = static_cast<int>(code.size()) - 1; pi >= 0; --pi) {
    if (static_cast<OpCode>(op_get(code[static_cast<size_t>(pi)])) != OpCode::CALL)
      continue;
    uint8_t ca = op_a(code[static_cast<size_t>(pi)]);
    uint8_t cb = op_b(code[static_cast<size_t>(pi)]);
    uint8_t cc = (nresults < 0) ? 0 : static_cast<uint8_t>(nresults + 1);
    code[static_cast<size_t>(pi)] = encode_abc(OpCode::CALL, ca, cb, cc);
    return ca;
  }
  return 0;
}

void mark_local_captured(FuncState& fs, int reg) {
  for (auto& l : fs.locals)
    if (l.active && l.reg == reg)
      l.captured = true;
}

int add_upval(FuncState& fs, const std::string& name, bool instack, int idx);

int ensure_env_upval(FuncState& fs) {
  for (size_t i = 0; i < fs.proto->upvalues.size(); ++i) {
    if (fs.proto->upvalues[i].name == "_ENV")
      return static_cast<int>(i);
  }
  if (!fs.prev) {
    if (fs.proto->upvalues.empty() || fs.proto->upvalues[0].name != "_ENV")
      panic("_ENV upvalue missing in main chunk");
    return 0;
  }
  // Prefer an enclosing local `_ENV` (lexical environments) over chaining the
  // chunk's upvalue — needed for `do local _ENV = mt; function f() A=1 end end`.
  int reg = fs.prev->find_local("_ENV");
  if (reg >= 0)
    return add_upval(fs, "_ENV", true, reg);
  for (size_t i = 0; i < fs.prev->proto->upvalues.size(); ++i) {
    if (fs.prev->proto->upvalues[i].name == "_ENV")
      return add_upval(fs, "_ENV", false, static_cast<int>(i));
  }
  int parent = ensure_env_upval(*fs.prev);
  return add_upval(fs, "_ENV", false, parent);
}

int add_upval(FuncState& fs, const std::string& name, bool instack, int idx) {
  for (size_t i = 0; i < fs.proto->upvalues.size(); ++i) {
    auto& u = fs.proto->upvalues[i];
    if (u.instack == instack && u.idx == static_cast<uint8_t>(idx))
      return static_cast<int>(i);
  }
  check_limit(fs, static_cast<int>(fs.proto->upvalues.size()) + 1, MAXUPVAL, "upvalues");
  if (instack && fs.prev)
    mark_local_captured(*fs.prev, idx);
  UpvalDesc u;
  u.name = name;
  u.instack = instack;
  u.idx = static_cast<uint8_t>(idx);
  fs.proto->upvalues.push_back(u);
  return static_cast<int>(fs.proto->upvalues.size() - 1);
}

int search_var(FuncState& fs, const std::string& name, Expdesc& e) {
  int reg = fs.find_local(name);
  if (reg >= 0) {
    e.kind = Expdesc::Local;
    e.info = reg;
    return 0;
  }
  // Already an upvalue of this function (main chunk's _ENV, or prior captures).
  for (size_t i = 0; i < fs.proto->upvalues.size(); ++i) {
    if (fs.proto->upvalues[i].name == name) {
      e.kind = Expdesc::Upval;
      e.info = static_cast<int>(i);
      return 0;
    }
  }
  if (fs.prev) {
    Expdesc pe;
    int r = search_var(*fs.prev, name, pe);
    if (r == 0 && pe.kind == Expdesc::Local) {
      int idx = add_upval(fs, name, true, pe.info);
      e.kind = Expdesc::Upval;
      e.info = idx;
      return 0;
    }
    if (r == 0 && pe.kind == Expdesc::Upval) {
      int idx = add_upval(fs, name, false, pe.info);
      e.kind = Expdesc::Upval;
      e.info = idx;
      return 0;
    }
  }
  // global via _ENV[name]. Prefer a local `_ENV` in this function (block scope).
  int env_local = fs.find_local("_ENV");
  if (env_local >= 0) {
    e.kind = Expdesc::Global;
    e.info = fs.string_k(name);
    e.table = env_local; // local _ENV register
    e.key = -2;          // sentinel: index local env, not GETTABUP
    return 0;
  }
  (void)ensure_env_upval(fs);
  e.kind = Expdesc::Global;
  e.info = fs.string_k(name);
  e.key = -1;
  return 0;
}

void discharge(FuncState& fs, Expdesc& e) {
  switch (e.kind) {
  case Expdesc::Local:
    e.reg = e.info;
    e.kind = Expdesc::Nonrelocable;
    break;
  case Expdesc::Upval:
    e.reg = fs.new_reg();
    fs.code_abc(OpCode::GETUPVAL, e.reg, e.info, 0);
    e.kind = Expdesc::Nonrelocable;
    break;
  case Expdesc::Global: {
    e.reg = fs.new_reg();
    if (e.key == -2) {
      // `_ENV` is a local in this function: index local_env[name]
      if (e.info <= 255) {
        fs.code_abc(OpCode::GETFIELD, e.reg, e.table, e.info);
      } else {
        int kreg = fs.new_reg();
        fs.code_abx(OpCode::LOADK, kreg, static_cast<uint16_t>(e.info));
        fs.code_abc(OpCode::GETTABLE, e.reg, e.table, kreg);
        fs.freereg = e.reg + 1;
      }
    } else {
      int env = ensure_env_upval(fs);
      if (e.info <= 255) {
        fs.code_abc(OpCode::GETTABUP, e.reg, env, e.info);
      } else {
        // C field is 8-bit: fall back to GETUPVAL + LOADK + GETTABLE.
        fs.code_abc(OpCode::GETUPVAL, e.reg, env, 0);
        int kreg = fs.new_reg();
        fs.code_abx(OpCode::LOADK, kreg, static_cast<uint16_t>(e.info));
        fs.code_abc(OpCode::GETTABLE, e.reg, e.reg, kreg);
        fs.freereg = e.reg + 1;
      }
    }
    e.kind = Expdesc::Nonrelocable;
    break;
  }
  case Expdesc::Constant: {
    e.reg = fs.new_reg();
    if (e.k.is_nil())
      fs.code_abc(OpCode::LOADNIL, e.reg, 0, 0);
    else if (e.k.is_bool())
      fs.code_abc(OpCode::LOADBOOL, e.reg, e.k.payload ? 1 : 0, 0);
    else if (e.k.is_int() && e.k.as_int() >= -32768 && e.k.as_int() <= 32767)
      fs.code_asbx(OpCode::LOADINT, e.reg, static_cast<int>(e.k.as_int()));
    else {
      int k = fs.const_index(e.k);
      fs.code_abx(OpCode::LOADK, e.reg, k);
    }
    e.kind = Expdesc::Nonrelocable;
    break;
  }
  case Expdesc::Indexed: {
    int r = fs.new_reg();
    emit_get_index(fs, r, e);
    // Keep result at r; do not freereg back over it (would clobber for t.a+t.b).
    fs.freereg = r + 1;
    e.reg = r;
    e.kind = Expdesc::Nonrelocable;
    break;
  }
  case Expdesc::Call:
    // results already placed starting at info; single value
    e.reg = e.info;
    e.kind = Expdesc::Nonrelocable;
    break;
  default:
    break;
  }
}

int exp2reg(FuncState& fs, Expdesc& e) {
  discharge(fs, e);
  if (e.kind == Expdesc::Relocable) {
    e.reg = fs.new_reg();
    // relocable unused in simplified emitter
  }
  return e.reg;
}

void expr_const(Expdesc& e, TValue v) {
  e.kind = Expdesc::Constant;
  e.k = v;
}

OpCode bin_arith_op(BinOp op) {
  switch (op) {
  case BinOp::Add: return OpCode::ADD;
  case BinOp::Sub: return OpCode::SUB;
  case BinOp::Mul: return OpCode::MUL;
  case BinOp::Div: return OpCode::DIV;
  case BinOp::IDiv: return OpCode::IDIV;
  case BinOp::Mod: return OpCode::MOD;
  case BinOp::Pow: return OpCode::POW;
  case BinOp::Band: return OpCode::BAND;
  case BinOp::Bor: return OpCode::BOR;
  case BinOp::Bxor: return OpCode::BXOR;
  case BinOp::Shl: return OpCode::SHL;
  case BinOp::Shr: return OpCode::SHR;
  default: panic("not arith");
  }
}

void expr(FuncState& fs, Expdesc& e, Expr& node) {
  e = {};
  if (node.line > 0)
    fs.lastline = node.line;
  switch (node.kind) {
  case AstKind::ExprNil:
    expr_const(e, TValue::nil());
    break;
  case AstKind::ExprBool:
    expr_const(e, TValue::boolean(static_cast<ExprBool&>(node).value));
    break;
  case AstKind::ExprInt:
    expr_const(e, TValue::integer(static_cast<ExprInt&>(node).value));
    break;
  case AstKind::ExprFloat:
    expr_const(e, TValue::number(static_cast<ExprFloat&>(node).value));
    break;
  case AstKind::ExprString:
    expr_const(e, TValue::obj(ValueTag::String, fs.L->intern(static_cast<ExprString&>(node).value)));
    break;
  case AstKind::ExprName:
    search_var(fs, static_cast<ExprName&>(node).name, e);
    break;
  case AstKind::ExprVararg: {
    int r = fs.new_reg();
    fs.code_abc(OpCode::VARARG, r, 2, 0); // one vararg value (contexts may rewrite)
    e.kind = Expdesc::Nonrelocable;
    e.reg = r;
    break;
  }
  case AstKind::ExprParen: {
    auto& n = static_cast<ExprParen&>(node);
    expr(fs, e, *n.inner);
    // Truncate multret: expose only the first result to outer contexts.
    if (e.kind == Expdesc::Call) {
      e.reg = e.info;
      e.kind = Expdesc::Nonrelocable;
    }
    break;
  }
  case AstKind::ExprUn: {
    auto& n = static_cast<ExprUn&>(node);
    Expdesc o;
    expr(fs, o, *n.operand);
    // Fold unary minus on constants. PUC intop(-,0,mininteger) wraps to mininteger.
    if (n.op == UnOp::Neg && o.kind == Expdesc::Constant) {
      if (o.k.is_int()) {
        auto u = static_cast<uint64_t>(o.k.as_int());
        expr_const(e, TValue::integer(static_cast<int64_t>(0u - u)));
        break;
      }
      if (o.k.is_float()) {
        expr_const(e, TValue::number(-o.k.as_float()));
        break;
      }
    }
    int r = exp2reg(fs, o);
    int dest = fs.new_reg();
    OpCode op = OpCode::UNM;
    if (n.op == UnOp::Not)
      op = OpCode::NOT;
    else if (n.op == UnOp::Len)
      op = OpCode::LEN;
    else if (n.op == UnOp::Bnot)
      op = OpCode::BNOT;
    fs.code_abc(op, dest, r, 0, n.line > 0 ? n.line : -1);
    if (n.line > 0)
      fs.lastline = n.line;
    fs.free_reg(r);
    e.kind = Expdesc::Nonrelocable;
    e.reg = dest;
    break;
  }
  case AstKind::ExprBin: {
    auto& n = static_cast<ExprBin&>(node);
    if (n.op == BinOp::And || n.op == BinOp::Or) {
      auto is_rel = [](Expr& x) -> ExprBin* {
        if (x.kind != AstKind::ExprBin)
          return nullptr;
        auto* b = static_cast<ExprBin*>(&x);
        switch (b->op) {
        case BinOp::Eq:
        case BinOp::Ne:
        case BinOp::Lt:
        case BinOp::Le:
        case BinOp::Gt:
        case BinOp::Ge:
          return b;
        default:
          return nullptr;
        }
      };
      // PUC-style: `cmp and cmp` stays in jump form and materializes one bool.
      // Value-producing LOADBOOL per comparison is too heavy for count hooks
      // (db.lua: assert(1000 < a and a < 1012) under count=1).
      auto* lrel = is_rel(*n.lhs);
      auto* rrel = is_rel(*n.rhs);
      if (n.op == BinOp::And && lrel && rrel) {
        // Preserve freereg floor so we do not clobber an in-progress CALL base.
        const int free_floor = fs.freereg;
        auto emit_rel_failjmp = [&](ExprBin& rel, std::vector<int>& jfalse) {
          fs.freereg = std::max(fs.freereg, free_floor);
          Expdesc l, r;
          expr(fs, l, *rel.lhs);
          int rb = exp2reg(fs, l);
          expr(fs, r, *rel.rhs);
          int rc = exp2reg(fs, r);
          OpCode cmp = OpCode::EQ;
          int left = rb, right = rc;
          // A=0: skip next (the fail JMP) when comparison holds.
          int acond = 0;
          switch (rel.op) {
          case BinOp::Eq: cmp = OpCode::EQ; break;
          case BinOp::Ne: cmp = OpCode::EQ; acond = 1; break;
          case BinOp::Lt: cmp = OpCode::LT; break;
          case BinOp::Gt: cmp = OpCode::LT; left = rc; right = rb; break;
          case BinOp::Le: cmp = OpCode::LE; break;
          case BinOp::Ge: cmp = OpCode::LE; left = rc; right = rb; break;
          default: break;
          }
          fs.code_abc(cmp, acond, left, right);
          jfalse.push_back(fs.code_asbx(OpCode::JMP, 0, 0));
          fs.freereg = free_floor;
        };
        std::vector<int> jfalse;
        emit_rel_failjmp(*lrel, jfalse);
        emit_rel_failjmp(*rrel, jfalse);
        fs.freereg = free_floor;
        int dest = fs.new_reg();
        fs.code_abc(OpCode::LOADBOOL, dest, 1, 0);
        int jend = fs.code_asbx(OpCode::JMP, 0, 0);
        int flab = fs.pc();
        for (int j : jfalse)
          fs.fix_sbx(j, flab);
        fs.code_abc(OpCode::LOADBOOL, dest, 0, 0);
        fs.fix_sbx(jend, fs.pc());
        fs.freereg = dest + 1;
        e.kind = Expdesc::Nonrelocable;
        e.reg = dest;
        break;
      }
      // and: dest=lhs; if not dest then goto end; dest=rhs; end:
      // or:  dest=lhs; if dest then goto end; dest=rhs; end:
      Expdesc l;
      expr(fs, l, *n.lhs);
      int lr = exp2reg(fs, l);
      int dest = fs.new_reg();
      fs.code_abc(OpCode::MOVE, dest, lr, 0);
      if (n.op == BinOp::And)
        fs.code_abc(OpCode::TEST, dest, 0, 0); // truthy => skip JMP
      else
        fs.code_abc(OpCode::TEST, dest, 0, 1); // falsy => skip JMP
      int jend = fs.code_asbx(OpCode::JMP, 0, 0); // taken when short-circuit
      fs.freereg = dest + 1;
      Expdesc r;
      expr(fs, r, *n.rhs);
      int rr = exp2reg(fs, r);
      if (rr != dest)
        fs.code_abc(OpCode::MOVE, dest, rr, 0);
      fs.fix_sbx(jend, fs.pc());
      fs.freereg = dest + 1;
      e.kind = Expdesc::Nonrelocable;
      e.reg = dest;
      break;
    }
    if (n.op == BinOp::Concat) {
      // Place operands in consecutive temps and write the result to a fresh
      // register — never CONCAT in-place over the LHS (can clobber CALL base).
      Expdesc l, r;
      expr(fs, l, *n.lhs);
      int lr = exp2reg(fs, l);
      expr(fs, r, *n.rhs);
      int rr = exp2reg(fs, r);
      int a = fs.new_reg();
      int bslot = a;
      int cslot = a + 1;
      if (fs.freereg < a + 2)
        fs.reserve(a + 2 - fs.freereg);
      if (lr != bslot)
        fs.code_abc(OpCode::MOVE, bslot, lr, 0);
      if (rr != cslot)
        fs.code_abc(OpCode::MOVE, cslot, rr, 0);
      fs.code_abc(OpCode::CONCAT, a, bslot, cslot);
      fs.freereg = a + 1;
      e.kind = Expdesc::Nonrelocable;
      e.reg = a;
      break;
    }
    if (n.op == BinOp::Eq || n.op == BinOp::Ne || n.op == BinOp::Lt || n.op == BinOp::Le ||
        n.op == BinOp::Gt || n.op == BinOp::Ge) {
      Expdesc l, r;
      expr(fs, l, *n.lhs);
      int rb = exp2reg(fs, l);
      expr(fs, r, *n.rhs);
      int rc = exp2reg(fs, r);
      OpCode cmp = OpCode::EQ;
      int acond = 1;
      int left = rb, right = rc;
      switch (n.op) {
      case BinOp::Eq: cmp = OpCode::EQ; acond = 1; break;
      case BinOp::Ne: cmp = OpCode::EQ; acond = 0; break;
      case BinOp::Lt: cmp = OpCode::LT; acond = 1; break;
      case BinOp::Gt: cmp = OpCode::LT; acond = 1; left = rc; right = rb; break;
      case BinOp::Le: cmp = OpCode::LE; acond = 1; break;
      case BinOp::Ge: cmp = OpCode::LE; acond = 1; left = rc; right = rb; break;
      default: break;
      }
      int dest = fs.new_reg();
      // EQ/LT/LE A B C: if ((B op C) ~= A) then pc++.
      // Value form: cmp; JMP 1; LOADBOOL dest 0 1; LOADBOOL dest 1 0
      fs.code_abc(cmp, acond, left, right);
      fs.code_asbx(OpCode::JMP, 0, 1);
      fs.code_abc(OpCode::LOADBOOL, dest, 0, 1);
      fs.code_abc(OpCode::LOADBOOL, dest, 1, 0);
      e.kind = Expdesc::Nonrelocable;
      e.reg = dest;
      break;
    }
    Expdesc l, r;
    expr(fs, l, *n.lhs);
    int rb = exp2reg(fs, l);
    expr(fs, r, *n.rhs);
    int rc = exp2reg(fs, r);
    // Prefer in-place into a temporary operand (PUC-style) so expressions like
    // `(a+1)+f()` leave the partial result in the next local slot for getlocal.
    int dest;
    if (rb >= fs.nactvar)
      dest = rb;
    else if (rc >= fs.nactvar)
      dest = rc;
    else
      dest = fs.new_reg();
    // Attribute the arithmetic op to the operator's line (not the RHS).
    int op_line = n.line > 0 ? n.line : fs.lastline;
    fs.code_abc(bin_arith_op(n.op), dest, rb, rc, op_line);
    fs.lastline = op_line;
    // Drop temps above dest (rhs may have bumped freereg past dest+1).
    fs.freereg = dest + 1;
    e.kind = Expdesc::Nonrelocable;
    e.reg = dest;
    break;
  }
  case AstKind::ExprIndex: {
    auto& n = static_cast<ExprIndex&>(node);
    Expdesc t, k;
    expr(fs, t, *n.table);
    int rt = exp2reg(fs, t);
    expr(fs, k, *n.key);
    e.kind = Expdesc::Indexed;
    e.table = rt;
    e.index_key = Expdesc::IndexKey::Reg;
    int imm = 0;
    if (k.kind == Expdesc::Constant && const_uint8_index(k.k, &imm)) {
      e.index_key = Expdesc::IndexKey::IntImm;
      e.key = imm;
    } else if (k.kind == Expdesc::Constant && k.k.is_string()) {
      int ki = fs.const_index(k.k);
      if (ki <= 255) {
        e.index_key = Expdesc::IndexKey::ConstStr;
        e.key = ki;
      } else {
        e.key = exp2reg(fs, k);
      }
    } else {
      e.key = exp2reg(fs, k);
    }
    break;
  }
  case AstKind::ExprField: {
    auto& n = static_cast<ExprField&>(node);
    Expdesc t;
    expr(fs, t, *n.table);
    int rt = exp2reg(fs, t);
    int ki = fs.string_k(n.field);
    e.kind = Expdesc::Indexed;
    e.table = rt;
    if (ki <= 255) {
      e.index_key = Expdesc::IndexKey::ConstStr;
      e.key = ki;
    } else {
      e.index_key = Expdesc::IndexKey::Reg;
      int rk = fs.new_reg();
      fs.code_abx(OpCode::LOADK, rk, ki);
      e.key = rk;
    }
    break;
  }
  case AstKind::ExprCall: {
    auto& n = static_cast<ExprCall&>(node);
    // Emit callee so the CALL lands at current freereg (needed for multret arg slots).
    // Never place a call frame over active locals — CALL/C-API windows can clobber them.
    if (fs.freereg < fs.nactvar)
      fs.freereg = fs.nactvar;
    int base = fs.freereg;
    fs.reserve(1);
    Expdesc c;
    expr(fs, c, *n.callee);
    if (c.kind == Expdesc::Indexed) {
      emit_get_index(fs, base, c);
    } else {
      int cr = exp2reg(fs, c);
      if (cr != base)
        fs.code_abc(OpCode::MOVE, base, cr, 0);
    }
    // Never call in-place over a live local — CALL writes results onto R[base].
    if (base < fs.nactvar) {
      int nb = fs.new_reg();
      fs.code_abc(OpCode::MOVE, nb, base, 0);
      base = nb;
    }
    fs.freereg = base + 1;
    if (n.is_method) {
      int key = fs.new_reg();
      fs.code_abx(OpCode::LOADK, key, fs.string_k(n.method));
      fs.code_abc(OpCode::SELF, base, base, key);
      fs.freereg = base + 2;
    }
    bool last_is_vararg = false;
    bool last_is_multret_call = false;
    for (size_t ai = 0; ai < n.args.size(); ++ai) {
      int dest = fs.freereg;
      auto& arg = n.args[ai];
      bool is_last = (ai + 1 == n.args.size());
      if (is_last && arg->kind == AstKind::ExprVararg) {
        fs.freereg = dest;
        fs.reserve(1);
        fs.code_abc(OpCode::VARARG, dest, 0, 0); // all varargs
        last_is_vararg = true;
        break;
      }
      // Last call arg must land at `dest` so results are contiguous after prior args.
      if (is_last && arg->kind == AstKind::ExprCall) {
        fs.freereg = dest;
        Expdesc ae;
        expr(fs, ae, *arg);
        patch_last_call_results(fs, -1);
        last_is_multret_call = true;
        break;
      }
      fs.freereg = dest + 1;
      fs.maxstack = std::max(fs.maxstack, fs.freereg);
      Expdesc ae;
      expr(fs, ae, *arg);
      int r = exp2reg(fs, ae);
      if (r != dest)
        fs.code_abc(OpCode::MOVE, dest, r, 0);
      fs.freereg = dest + 1;
    }
    int b_field = (last_is_vararg || last_is_multret_call) ? 0 : (fs.freereg - base);
    // CALL line is the callee (PUC: a\n(\n23) errors on line of `a`).
    int call_line = n.line > 0 ? n.line : -1;
    fs.code_abc(OpCode::CALL, base, static_cast<uint8_t>(b_field), 2, call_line);
    if (call_line > 0)
      fs.lastline = call_line;
    fs.freereg = base + 1;
    e.kind = Expdesc::Call;
    e.info = base;
    e.reg = base;
    break;
  }
  case AstKind::ExprTable: {
    auto& n = static_cast<ExprTable&>(node);
    int r = fs.new_reg();
    fs.code_abc(OpCode::NEWTABLE, r, 0, 0);
    // Array fields go into registers R[r+1]... then SETLIST in blocks of 50
    // (PUC LFIELDS_PER_FLUSH) so large constructors stay within MAXSTACK.
    int na = 0;       // total array elements so far
    int tostore = 0;  // pending elements since last flush
    bool arr_multret = false;
    for (size_t fi = 0; fi < n.fields.size(); ++fi) {
      auto& f = n.fields[fi];
      if (!f.name.empty()) {
        int ki = fs.string_k(f.name);
        Expdesc v;
        expr(fs, v, *f.value);
        int rv = exp2reg(fs, v);
        if (ki <= 255) {
          fs.code_abc(OpCode::SETFIELD, r, ki, rv);
        } else {
          int k = fs.new_reg();
          fs.code_abx(OpCode::LOADK, k, ki);
          fs.code_abc(OpCode::SETTABLE, r, k, rv);
        }
        fs.freereg = r + 1 + tostore;
      } else if (f.key) {
        Expdesc k, v;
        expr(fs, k, *f.key);
        expr(fs, v, *f.value);
        int rv = exp2reg(fs, v);
        int imm = 0;
        if (k.kind == Expdesc::Constant && const_uint8_index(k.k, &imm)) {
          fs.code_abc(OpCode::SETI, r, imm, rv);
        } else if (k.kind == Expdesc::Constant && k.k.is_string()) {
          int ki = fs.const_index(k.k);
          if (ki <= 255)
            fs.code_abc(OpCode::SETFIELD, r, ki, rv);
          else {
            int rk = exp2reg(fs, k);
            fs.code_abc(OpCode::SETTABLE, r, rk, rv);
          }
        } else {
          int rk = exp2reg(fs, k);
          fs.code_abc(OpCode::SETTABLE, r, rk, rv);
        }
        fs.freereg = r + 1 + tostore;
      } else {
        bool last_array = true;
        for (size_t j = fi + 1; j < n.fields.size(); ++j) {
          if (n.fields[j].name.empty() && !n.fields[j].key) {
            last_array = false;
            break;
          }
        }
        int dest = r + 1 + tostore;
        fs.freereg = dest;
        if (last_array && f.value->kind == AstKind::ExprVararg) {
          fs.reserve(1);
          fs.code_abc(OpCode::VARARG, dest, 0, 0);
          arr_multret = true;
          break;
        }
        if (last_array && f.value->kind == AstKind::ExprCall) {
          Expdesc v;
          expr(fs, v, *f.value);
          patch_last_call_results(fs, -1);
          arr_multret = true;
          break;
        }
        Expdesc v;
        expr(fs, v, *f.value);
        int rv = exp2reg(fs, v);
        if (rv != dest)
          fs.code_abc(OpCode::MOVE, dest, rv, 0);
        ++tostore;
        ++na;
        fs.freereg = dest + 1;
        if (tostore == 50) {
          int c = (na - 1) / 50 + 1;
          fs.code_abc(OpCode::SETLIST, r, 50, static_cast<uint8_t>(c));
          tostore = 0;
          fs.freereg = r + 1;
        }
      }
    }
    if (tostore > 0 || arr_multret) {
      int b = arr_multret ? 0 : tostore;
      int c = (na == 0) ? 1 : ((na - 1) / 50 + 1);
      fs.code_abc(OpCode::SETLIST, r, static_cast<uint8_t>(b), static_cast<uint8_t>(c));
    }
    fs.freereg = r + 1;
    e.kind = Expdesc::Nonrelocable;
    e.reg = r;
    break;
  }
  case AstKind::ExprFunction: {
    auto& n = static_cast<ExprFunction&>(node);
    auto* p = fs.L->gc.create<Proto>(GcKind::Proto);
    fs.proto->protos.push_back(p);
    FuncState child;
    child.L = fs.L;
    child.prev = &fs;
    child.proto = p;
    child.lastline = node.line;
    child.vararg = n.is_vararg;
    p->is_vararg = n.is_vararg;
    p->numparams = static_cast<int>(n.params.size());
    p->source = fs.proto->source;
    p->linedefined = node.line;
    // _ENV is added on demand when the body references globals.
    child.freereg = 0;
    for (auto& param : n.params) {
      int reg = child.new_reg();
      child.push_local(param, reg, node.line);
    }
    block(child, *n.body);
    for (auto& g : child.pending_gotos) {
      auto it = child.labels.find(g.name);
      if (it == child.labels.end())
        panic("no visible label for goto: " + g.name);
      int a = FuncState::goto_close_a(g.nactvar, it->second.nactvar);
      int sbx = it->second.pc - (g.jmp_pc + 1);
      child.proto->code[static_cast<size_t>(g.jmp_pc)] =
          encode_asbx(OpCode::JMP, static_cast<uint8_t>(a), sbx);
    }
    child.lastline = n.lastline > 0 ? n.lastline : child.lastline;
    child.code_abc(OpCode::RETURN, 0, 1, 0, child.lastline);
    // Parameters stay active for the whole function; record them now.
    child.flush_locvars_to(0);
    p->lastlinedefined = n.lastline > 0 ? n.lastline : node.line;
    for (int li : p->lineinfo)
      if (li > p->lastlinedefined)
        p->lastlinedefined = li;
    p->maxstack = std::max(child.maxstack, 2);
    int dest = fs.new_reg();
    // PUC attributes OP_CLOSURE to the function's `end` line (ls->lastline
    // after matching END). Needed for db.lua local-A line-hook checks.
    if (n.lastline > 0)
      fs.lastline = n.lastline;
    fs.code_abx(OpCode::CLOSURE, dest, static_cast<int>(fs.proto->protos.size() - 1));
    e.kind = Expdesc::Nonrelocable;
    e.reg = dest;
    break;
  }
  default:
    panic("unsupported expression in lowering");
  }
}

void storevar(FuncState& fs, Expdesc& var, int expr_reg) {
  if (var.kind == Expdesc::Local) {
    fs.code_abc(OpCode::MOVE, var.info, expr_reg, 0);
  } else if (var.kind == Expdesc::Upval) {
    fs.code_abc(OpCode::SETUPVAL, expr_reg, var.info, 0);
  } else if (var.kind == Expdesc::Global) {
    if (var.key == -2) {
      if (var.info <= 255) {
        fs.code_abc(OpCode::SETFIELD, var.table, var.info, expr_reg);
      } else {
        int kreg = fs.new_reg();
        fs.code_abx(OpCode::LOADK, kreg, static_cast<uint16_t>(var.info));
        fs.code_abc(OpCode::SETTABLE, var.table, kreg, expr_reg);
      }
    } else {
      int env = ensure_env_upval(fs);
      if (var.info <= 255) {
        fs.code_abc(OpCode::SETTABUP, env, var.info, expr_reg);
      } else {
        int treg = fs.new_reg();
        fs.code_abc(OpCode::GETUPVAL, treg, env, 0);
        int kreg = fs.new_reg();
        fs.code_abx(OpCode::LOADK, kreg, static_cast<uint16_t>(var.info));
        fs.code_abc(OpCode::SETTABLE, treg, kreg, expr_reg);
      }
    }
  } else if (var.kind == Expdesc::Indexed) {
    emit_set_index(fs, var, expr_reg);
  } else {
    panic("invalid assignment target");
  }
}

void block(FuncState& fs, Block& b, bool allow_last_label, bool close) {
  int saved = static_cast<int>(fs.locals.size());
  fs.block_entry_nactvar.push_back(fs.nactvar);
  const size_t pending_before = fs.pending_gotos.size();

  // Label names defined in this block (for uniqueness + shadowing).
  std::unordered_set<std::string> names_here;
  for (auto& sp : b.stmts) {
    if (sp->kind == AstKind::Label)
      names_here.insert(static_cast<LabelStmt&>(*sp).name);
  }
  std::unordered_set<std::string> defined_here;
  std::unordered_map<std::string, FuncState::LabelInfo> shadowed;

  for (size_t i = 0; i < b.stmts.size(); ++i) {
    AstNode& s = *b.stmts[i];
    if (s.kind == AstKind::Label) {
      auto& n = static_cast<LabelStmt&>(s);
      if (defined_here.count(n.name))
        panic("label already defined: " + n.name);
      bool last = allow_last_label;
      if (last) {
        for (size_t j = i + 1; j < b.stmts.size(); ++j) {
          if (b.stmts[j]->kind != AstKind::Label) {
            last = false;
            break;
          }
        }
      }
      int nv = last ? fs.block_entry_nactvar.back() : fs.nactvar;
      if (fs.labels.count(n.name) && !defined_here.count(n.name))
        shadowed[n.name] = fs.labels[n.name];
      fs.labels[n.name] = {fs.pc(), nv};
      defined_here.insert(n.name);
      continue;
    }
    if (s.kind == AstKind::Goto) {
      auto& n = static_cast<GotoStmt&>(s);
      // Prefer a label belonging to this block (incl. forward) over an outer one.
      if (names_here.count(n.label)) {
        if (defined_here.count(n.label)) {
          auto& info = fs.labels[n.label];
          int a = FuncState::goto_close_a(fs.nactvar, info.nactvar);
          fs.code_asbx(OpCode::JMP, a, info.pc - (fs.pc() + 1));
        } else {
          int j = fs.code_asbx(OpCode::JMP, 0, 0);
          fs.pending_gotos.push_back({n.label, j, fs.nactvar});
        }
      } else if (fs.labels.count(n.label)) {
        auto& info = fs.labels[n.label];
        int a = FuncState::goto_close_a(fs.nactvar, info.nactvar);
        fs.code_asbx(OpCode::JMP, a, info.pc - (fs.pc() + 1));
      } else {
        int j = fs.code_asbx(OpCode::JMP, 0, 0);
        fs.pending_gotos.push_back({n.label, j, fs.nactvar});
      }
      continue;
    }
    statement(fs, s);
  }

  // Patch only gotos opened in this block that target this block's labels.
  std::vector<FuncState::PendingGoto> remain;
  for (size_t i = 0; i < fs.pending_gotos.size(); ++i) {
    auto& g = fs.pending_gotos[i];
    if (i >= pending_before && defined_here.count(g.name)) {
      auto& info = fs.labels[g.name];
      int a = FuncState::goto_close_a(g.nactvar, info.nactvar);
      int sbx = info.pc - (g.jmp_pc + 1);
      fs.proto->code[static_cast<size_t>(g.jmp_pc)] =
          encode_asbx(OpCode::JMP, static_cast<uint8_t>(a), sbx);
    } else {
      remain.push_back(g);
    }
  }
  fs.pending_gotos = std::move(remain);
  for (auto& name : defined_here) {
    fs.labels.erase(name);
    auto it = shadowed.find(name);
    if (it != shadowed.end())
      fs.labels[name] = it->second;
  }

  fs.block_entry_nactvar.pop_back();
  if (close)
    fs.leave_block(saved);
}

void statement(FuncState& fs, AstNode& node) {
  fs.lastline = node.line;
  switch (node.kind) {
  case AstKind::LocalDecl: {
    // Evaluate RHS before registering names so `local l = table.remove(l,1)`
    // binds the RHS to the outer `l` (Lua 5.3 scope rules).
    // Emit each RHS into its destination register (PUC exp2nextreg style).
    // Reserving all slots first forced temps above locals and overflowed R[255].
    fs.freereg = fs.nactvar;
    auto& n = static_cast<LocalDecl&>(node);
    int nnames = static_cast<int>(n.names.size());
    int base = fs.freereg;
    fs.maxstack = std::max(fs.maxstack, base + nnames);
    int filled = 0;
    for (size_t i = 0; i < n.values.size(); ++i) {
      bool is_last = (i + 1 == n.values.size());
      int dest = base + static_cast<int>(i);
      int need = nnames - static_cast<int>(i);
      if (is_last && n.values[i]->kind == AstKind::ExprVararg) {
        fs.freereg = dest;
        fs.code_abc(OpCode::VARARG, dest, static_cast<uint8_t>(need + 1), 0);
        filled = static_cast<int>(i) + need;
        fs.freereg = base + nnames;
        break;
      }
      if (is_last && n.values[i]->kind == AstKind::ExprCall) {
        fs.freereg = dest;
        Expdesc e;
        expr(fs, e, *n.values[i]);
        uint8_t ca = patch_last_call_results(fs, need);
        for (int j = 0; j < need; ++j) {
          int from = static_cast<int>(ca) + j;
          int to = dest + j;
          if (from != to)
            fs.code_abc(OpCode::MOVE, to, from, 0);
        }
        filled = static_cast<int>(i) + need;
        fs.freereg = base + nnames;
        break;
      }
      fs.freereg = dest;
      Expdesc e;
      expr(fs, e, *n.values[i]);
      int er = exp2reg(fs, e);
      if (er != dest)
        fs.code_abc(OpCode::MOVE, dest, er, 0);
      filled = static_cast<int>(i) + 1;
      fs.freereg = dest + 1;
    }
    for (int i = filled; i < nnames; ++i)
      fs.code_abc(OpCode::LOADNIL, base + i, 0, 0);
    for (int i = 0; i < nnames; ++i)
      fs.push_local(n.names[static_cast<size_t>(i)], base + i, node.line);
    fs.freereg = fs.nactvar;
    break;
  }
  case AstKind::Assign: {
    auto& n = static_cast<Assign&>(node);
    fs.freereg = fs.nactvar;
    std::vector<Expdesc> vars;
    for (auto& v : n.vars) {
      Expdesc e;
      expr(fs, e, *v);
      if (e.kind == Expdesc::Indexed) {
        // Snapshot table & key so later stores in this statement cannot clobber
        // pre-assign values (PUC multi-assign: `i, a[i], a, ... = ...`).
        int t = fs.new_reg();
        fs.code_abc(OpCode::MOVE, t, e.table, 0);
        e.table = t;
        if (e.index_key == Expdesc::IndexKey::Reg) {
          int k = fs.new_reg();
          fs.code_abc(OpCode::MOVE, k, e.key, 0);
          e.key = k;
        }
        // IntImm / ConstStr keys are immediates — no register to snapshot.
      }
      vars.push_back(e);
    }
    int nvars = static_cast<int>(vars.size());
    int base = fs.freereg;
    fs.maxstack = std::max(fs.maxstack, base + nvars);
    int filled = 0;
    for (size_t i = 0; i < n.values.size(); ++i) {
      bool is_last = (i + 1 == n.values.size());
      int dest = base + static_cast<int>(i);
      int need = nvars - static_cast<int>(i);
      if (is_last && n.values[i]->kind == AstKind::ExprVararg) {
        fs.freereg = dest;
        fs.code_abc(OpCode::VARARG, dest, static_cast<uint8_t>(need + 1), 0);
        filled = static_cast<int>(i) + need;
        fs.freereg = base + nvars;
        break;
      }
      if (is_last && n.values[i]->kind == AstKind::ExprCall) {
        fs.freereg = dest;
        Expdesc e;
        expr(fs, e, *n.values[i]);
        uint8_t ca = patch_last_call_results(fs, need);
        for (int j = 0; j < need; ++j) {
          int from = static_cast<int>(ca) + j;
          int to = dest + j;
          if (from != to)
            fs.code_abc(OpCode::MOVE, to, from, 0);
        }
        filled = static_cast<int>(i) + need;
        fs.freereg = base + nvars;
        break;
      }
      fs.freereg = dest;
      Expdesc e;
      expr(fs, e, *n.values[i]);
      int er = exp2reg(fs, e);
      if (er != dest)
        fs.code_abc(OpCode::MOVE, dest, er, 0);
      filled = static_cast<int>(i) + 1;
      fs.freereg = dest + 1;
    }
    for (int i = filled; i < nvars; ++i)
      fs.code_abc(OpCode::LOADNIL, base + i, 0, 0);
    for (int i = 0; i < nvars; ++i)
      storevar(fs, vars[static_cast<size_t>(i)], base + i);
    fs.freereg = fs.nactvar;
    break;
  }
  case AstKind::CallStmt: {
    auto& n = static_cast<CallStmt&>(node);
    Expdesc e;
    expr(fs, e, *n.call);
    // CALL already emitted with 1 result; discard
    if (e.reg >= 0)
      fs.freereg = fs.nactvar;
    break;
  }
  case AstKind::Return: {
    auto& n = static_cast<ReturnStmt&>(node);
    if (n.values.empty()) {
      fs.code_abc(OpCode::RETURN, 0, 1, 0);
    } else if (n.values.size() == 1 && n.values[0]->kind == AstKind::ExprCall) {
      // `return f(...)` — proper tail call (PUC OP_TAILCALL).
      // Close captured locals FIRST (while their stack slots are intact), then
      // TAILCALL. Sliding the callee onto those slots before close would make
      // nested closures capture the wrong values (e.g. g's upvalue f becomes g
      // → infinite recursion / multi-GB stack growth).
      fs.freereg = fs.nactvar;
      Expdesc e;
      expr(fs, e, *n.values[0]);
      // Insert a close-only JMP immediately before the CALL we are about to patch.
      auto& code = fs.proto->code;
      int call_pc = -1;
      for (int pi = static_cast<int>(code.size()) - 1; pi >= 0; --pi) {
        if (static_cast<OpCode>(op_get(code[static_cast<size_t>(pi)])) == OpCode::CALL) {
          call_pc = pi;
          break;
        }
      }
      int close_level = fs.captured_close_level();
      if (call_pc >= 0 && close_level >= 0) {
        Instruction jmp = encode_asbx(OpCode::JMP, static_cast<uint8_t>(close_level + 1), 0);
        code.insert(code.begin() + call_pc, jmp);
        fs.proto->lineinfo.insert(fs.proto->lineinfo.begin() + call_pc,
                                  fs.proto->lineinfo[static_cast<size_t>(call_pc)]);
        call_pc += 1;
      }
      if (call_pc >= 0) {
        uint8_t ca = op_a(code[static_cast<size_t>(call_pc)]);
        uint8_t cb = op_b(code[static_cast<size_t>(call_pc)]);
        code[static_cast<size_t>(call_pc)] = encode_abc(OpCode::TAILCALL, ca, cb, 0);
      } else {
        patch_last_call_results(fs, -1);
        fs.code_abc(OpCode::RETURN, e.info >= 0 ? e.info : fs.nactvar, 0, 0);
      }
    } else {
      // Evaluate from nactvar so return values occupy consecutive stack slots
      // immediately after locals (needed for debug.getlocal of temporaries).
      fs.freereg = fs.nactvar;
      int base = fs.freereg;
      bool multret = false;
      for (size_t i = 0; i < n.values.size(); ++i) {
        int dest = base + static_cast<int>(i);
        bool is_last = (i + 1 == n.values.size());
        if (is_last && n.values[i]->kind == AstKind::ExprVararg) {
          fs.freereg = dest;
          fs.reserve(1);
          fs.code_abc(OpCode::VARARG, dest, 0, 0);
          multret = true;
          break;
        }
        if (is_last && n.values[i]->kind == AstKind::ExprCall) {
          fs.freereg = dest;
          Expdesc e;
          expr(fs, e, *n.values[i]);
          patch_last_call_results(fs, -1);
          multret = true;
          break;
        }
        fs.freereg = dest;
        Expdesc e;
        expr(fs, e, *n.values[i]);
        int r = exp2reg(fs, e);
        if (r != dest)
          fs.code_abc(OpCode::MOVE, dest, r, 0);
        fs.freereg = std::max(fs.freereg, dest + 1);
      }
      if (multret)
        fs.code_abc(OpCode::RETURN, base, 0, 0);
      else {
        int nret = static_cast<int>(n.values.size());
        fs.code_abc(OpCode::RETURN, base, nret + 1, 0);
      }
    }
    break;
  }
  case AstKind::If: {
    auto& n = static_cast<IfStmt&>(node);
    std::vector<int> escape;
    for (size_t i = 0; i < n.branches.size(); ++i) {
      Expdesc c;
      expr(fs, c, *n.branches[i].cond);
      int cr = exp2reg(fs, c);
      // Attribute the test/jump to `then` (Lua lineinfo for db.lua traces).
      if (n.branches[i].then_line > 0)
        fs.lastline = n.branches[i].then_line;
      fs.code_abc(OpCode::TEST, cr, 0, 0);
      int jf = fs.code_asbx(OpCode::JMP, 0, 0);
      fs.freereg = fs.nactvar;
      block(fs, *n.branches[i].body);
      if (n.end_line > 0)
        fs.lastline = n.end_line;
      int je = fs.code_asbx(OpCode::JMP, 0, 0);
      escape.push_back(je);
      fs.fix_sbx(jf, fs.pc());
    }
    if (n.else_body) {
      if (n.else_line > 0)
        fs.lastline = n.else_line;
      block(fs, *n.else_body);
    }
    for (int j : escape)
      fs.fix_sbx(j, fs.pc());
    if (n.end_line > 0)
      fs.lastline = n.end_line;
    break;
  }
  case AstKind::While: {
    auto& n = static_cast<WhileStmt&>(node);
    int loop = fs.pc();
    Expdesc c;
    expr(fs, c, *n.cond);
    int cr = exp2reg(fs, c);
    fs.code_abc(OpCode::TEST, cr, 0, 0);
    int jf = fs.code_asbx(OpCode::JMP, 0, 0);
    fs.freereg = fs.nactvar;
    int breaks_before = static_cast<int>(fs.break_list.size());
    fs.loop_break_level.push_back(fs.nactvar);
    block(fs, *n.body);
    fs.loop_break_level.pop_back();
    fs.code_asbx(OpCode::JMP, 0, loop - (fs.pc() + 1));
    // Condition-fail lands on `end` line marker; `break` skips it (db.lua).
    int end_marker_pc = fs.pc();
    if (n.end_line > 0) {
      fs.lastline = n.end_line;
      int r = fs.new_reg();
      fs.code_abc(OpCode::LOADNIL, r, 0, 0);
      fs.freereg = fs.nactvar;
    }
    int after_while = fs.pc();
    fs.fix_sbx(jf, n.end_line > 0 ? end_marker_pc : after_while);
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), after_while);
      fs.break_list.pop_back();
    }
    break;
  }
  case AstKind::Repeat: {
    auto& n = static_cast<RepeatStmt&>(node);
    int saved = static_cast<int>(fs.locals.size());
    int loop = fs.pc();
    int breaks_before = static_cast<int>(fs.break_list.size());
    fs.loop_break_level.push_back(fs.nactvar);
    // Labels/gotos in the body; keep locals open for the until condition.
    block(fs, *n.body, /*allow_last_label=*/false, /*close=*/false);
    // Condition may reference body locals — evaluate before closing.
    Expdesc c;
    expr(fs, c, *n.cond);
    int cr = exp2reg(fs, c);
    fs.code_abc(OpCode::TEST, cr, 0, 1); // exit if true
    int exit_jmp = fs.code_asbx(OpCode::JMP, 0, 0);
    // Continue: close captured body locals, then jump to loop head (PUC).
    int close_level = -1;
    for (int li = saved; li < static_cast<int>(fs.locals.size()); ++li) {
      auto& loc = fs.locals[static_cast<size_t>(li)];
      if (loc.active && loc.captured && (close_level < 0 || loc.reg < close_level))
        close_level = loc.reg;
    }
    int cont_a = close_level >= 0 ? close_level + 1 : 0;
    fs.code_asbx(OpCode::JMP, cont_a, loop - (fs.pc() + 1));
    fs.fix_sbx(exit_jmp, fs.pc());
    fs.loop_break_level.pop_back();
    fs.leave_block(saved);
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), fs.pc());
      fs.break_list.pop_back();
    }
    break;
  }
  case AstKind::Break: {
    // PUC: break jumps out of the loop and closes upvalues of that loop's scope.
    int a = 0;
    if (!fs.loop_break_level.empty()) {
      int min_reg = fs.loop_break_level.back();
      int close_level = -1;
      for (auto& l : fs.locals) {
        if (l.active && l.captured && l.reg >= min_reg &&
            (close_level < 0 || l.reg < close_level))
          close_level = l.reg;
      }
      if (close_level >= 0)
        a = close_level + 1;
    }
    fs.break_list.push_back(fs.code_asbx(OpCode::JMP, a, 0));
    break;
  }
  case AstKind::ForNum: {
    auto& n = static_cast<ForNum&>(node);
    int base = fs.freereg;
    Expdesc e1, e2, e3;
    expr(fs, e1, *n.from);
    int r1 = exp2reg(fs, e1);
    if (r1 != base)
      fs.code_abc(OpCode::MOVE, base, r1, 0);
    fs.freereg = base + 1;
    expr(fs, e2, *n.to);
    int r2 = exp2reg(fs, e2);
    if (r2 != base + 1)
      fs.code_abc(OpCode::MOVE, base + 1, r2, 0);
    fs.freereg = base + 2;
    if (n.step) {
      expr(fs, e3, *n.step);
      int r3 = exp2reg(fs, e3);
      if (r3 != base + 2)
        fs.code_abc(OpCode::MOVE, base + 2, r3, 0);
    } else {
      fs.code_asbx(OpCode::LOADINT, base + 2, 1);
    }
    fs.freereg = base + 3;
    fs.lastline = node.line;
    int prep = fs.code_asbx(OpCode::FORPREP, base, 0);
    int loop = fs.pc();
    // Loop variable is scoped to each iteration (PUC leaveblock before FORLOOP)
    // so closures capture a distinct closed upvalue per iteration.
    int before_i = static_cast<int>(fs.locals.size());
    int ireg = fs.new_reg();
    fs.push_local(n.name, ireg, node.line);
    fs.locals.back().reg = base + 3;
    fs.nactvar = base + 4;
    fs.freereg = base + 4;
    int breaks_before = static_cast<int>(fs.break_list.size());
    fs.loop_break_level.push_back(base + 3);
    block(fs, *n.body); // body locals + labels; keeps `i`
    fs.loop_break_level.pop_back();
    // Close captured `i` at end of each iteration.
    fs.leave_block(before_i, /*clear_dead=*/false);
    fs.lastline = node.line;
    fs.code_asbx(OpCode::FORLOOP, base, loop - (fs.pc() + 1));
    int forloop_pc = fs.pc() - 1;
    fs.fix_sbx(prep, forloop_pc);
    fs.proto->code[static_cast<size_t>(forloop_pc)] =
        encode_asbx(OpCode::FORLOOP, static_cast<uint8_t>(base), loop - (forloop_pc + 1));
    // Do not emit an end-line LOADNIL: PUC has no extra insn here. The chunk
    // RETURN (or next statement) inherits lastline=end for line-hook traces.
    if (n.end_line > 0)
      fs.lastline = n.end_line;
    int after_for = fs.pc();
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), after_for);
      fs.break_list.pop_back();
    }
    fs.freereg = base;
    break;
  }
  case AstKind::LocalFunction: {
    auto& n = static_cast<LocalFunction&>(node);
    int r = fs.new_reg();
    fs.push_local(n.name, r, node.line);
    Expdesc e;
    expr(fs, e, *n.fn);
    int er = exp2reg(fs, e);
    if (er != r)
      fs.code_abc(OpCode::MOVE, r, er, 0);
    fs.freereg = fs.nactvar;
    break;
  }
  case AstKind::FunctionDecl: {
    auto& n = static_cast<FunctionDecl&>(node);
    int fline = n.line > 0 ? n.line : fs.lastline;
    if (n.line > 0)
      fs.lastline = n.line;
    Expdesc e;
    expr(fs, e, *n.fn);
    int er = exp2reg(fs, e);
    if (n.name_path.size() == 1) {
      // `function f()` assigns to local/upvalue/global via normal name lookup
      // (must update an in-scope local f, not always _ENV.f).
      Expdesc var;
      search_var(fs, n.name_path[0], var);
      storevar(fs, var, er);
    } else {
      // a.b.c = fn — attribute indexing errors to the `function` line.
      Expdesc t;
      search_var(fs, n.name_path[0], t);
      int reg = exp2reg(fs, t);
      for (size_t i = 1; i + 1 < n.name_path.size(); ++i) {
        int ki = fs.string_k(n.name_path[i]);
        int d = fs.new_reg();
        if (ki <= 255) {
          fs.code_abc(OpCode::GETFIELD, d, reg, ki, fline);
        } else {
          int k = fs.new_reg();
          fs.code_abx(OpCode::LOADK, k, ki, fline);
          fs.code_abc(OpCode::GETTABLE, d, reg, k, fline);
        }
        reg = d;
      }
      int ki = fs.string_k(n.name_path.back());
      if (ki <= 255) {
        fs.code_abc(OpCode::SETFIELD, reg, ki, er, fline);
      } else {
        int k = fs.new_reg();
        fs.code_abx(OpCode::LOADK, k, ki, fline);
        fs.code_abc(OpCode::SETTABLE, reg, k, er, fline);
      }
    }
    fs.freereg = fs.nactvar;
    break;
  }
  case AstKind::DoBlock: {
    auto& n = static_cast<DoBlock&>(node);
    block(fs, *n.body);
    break;
  }
  case AstKind::Label:
    // Registered in block() (needs last-label / nactvar lookahead).
    break;
  case AstKind::Goto: {
    auto& n = static_cast<GotoStmt&>(node);
    auto it = fs.labels.find(n.label);
    if (it != fs.labels.end()) {
      int a = FuncState::goto_close_a(fs.nactvar, it->second.nactvar);
      fs.code_asbx(OpCode::JMP, a, it->second.pc - (fs.pc() + 1));
    } else {
      int j = fs.code_asbx(OpCode::JMP, 0, 0);
      fs.pending_gotos.push_back({n.label, j, fs.nactvar});
    }
    break;
  }
  case AstKind::ForIn: {
    auto& n = static_cast<ForIn&>(node);
    // Adjust iterator explist to 3 values: generator, state, control.
    int base = fs.freereg;
    fs.reserve(3);
    int filled = 0;
    for (size_t i = 0; i < n.iters.size(); ++i) {
      bool is_last = (i + 1 == n.iters.size());
      int dest = base + static_cast<int>(i);
      fs.freereg = std::max(fs.freereg, dest + 1);
      Expdesc e;
      expr(fs, e, *n.iters[i]);
      if (is_last && e.kind == Expdesc::Call) {
        int need = 3 - static_cast<int>(i);
        auto& code = fs.proto->code;
        uint8_t ca = 0;
        for (int pi = static_cast<int>(code.size()) - 1; pi >= 0; --pi) {
          if (static_cast<OpCode>(op_get(code[static_cast<size_t>(pi)])) == OpCode::CALL) {
            ca = op_a(code[static_cast<size_t>(pi)]);
            uint8_t cb = op_b(code[static_cast<size_t>(pi)]);
            code[static_cast<size_t>(pi)] =
                encode_abc(OpCode::CALL, ca, cb, static_cast<uint8_t>(need + 1));
            break;
          }
        }
        for (int j = 0; j < need; ++j) {
          int from = static_cast<int>(ca) + j;
          int to = dest + j;
          if (from != to)
            fs.code_abc(OpCode::MOVE, to, from, 0);
        }
        filled = static_cast<int>(i) + need;
        fs.freereg = base + 3;
      } else {
        int r = exp2reg(fs, e);
        if (r != dest)
          fs.code_abc(OpCode::MOVE, dest, r, 0);
        filled = static_cast<int>(i) + 1;
        fs.freereg = dest + 1;
      }
    }
    for (int i = filled; i < 3; ++i)
      fs.code_abc(OpCode::LOADNIL, base + i, 0, 0);
    fs.freereg = base + 3;

    // Lua 5.3 shape: JMP into TFORCALL; TFORLOOP A=base+2 checks R[A+1]=R[base+3].
    int nvars = static_cast<int>(n.names.size());
    fs.freereg = base + 3 + nvars;
    fs.maxstack = std::max(fs.maxstack, fs.freereg);
    int jprep = fs.code_asbx(OpCode::JMP, 0, 0); // jump to TFORCALL
    int loopbody = fs.pc();
    int nlocals_before = static_cast<int>(fs.locals.size());
    for (int i = 0; i < nvars; ++i) {
      int r = base + 3 + i;
      fs.push_local(n.names[static_cast<size_t>(i)], r, node.line);
    }
    fs.nactvar = base + 3 + nvars;
    fs.freereg = fs.nactvar;
    int breaks_before = static_cast<int>(fs.break_list.size());
    fs.loop_break_level.push_back(base + 3);
    block(fs, *n.body);
    fs.loop_break_level.pop_back();
    // Locals end after the body; do not LOADNIL every iteration (PUC leaves
    // stale values — also keeps count/line hooks aligned).
    fs.leave_block(nlocals_before, /*clear_dead=*/false);
    // Attribute the generator call to the iterator expression (PUC lineerror).
    int call_line = node.line;
    if (!n.iters.empty() && n.iters.back() && n.iters.back()->line > 0)
      call_line = n.iters.back()->line;
    fs.lastline = call_line;
    fs.fix_sbx(jprep, fs.pc());
    fs.code_abc(OpCode::TFORCALL, base, 0, nvars, call_line);
    int tforloop = fs.code_asbx(OpCode::TFORLOOP, base + 2, 0);
    // fix_sbx(pc, dest) encodes sbx = dest - (pc + 1); pass absolute dest.
    fs.fix_sbx(tforloop, loopbody);
    if (n.end_line > 0)
      fs.lastline = n.end_line;
    int after_for = fs.pc();
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), after_for);
      fs.break_list.pop_back();
    }
    fs.freereg = base;
    break;
  }
  default:
    panic("unsupported statement in lowering");
  }
}

} // namespace

Proto* lower_chunk(State* L, Chunk& chunk, const std::string& source_name) {
  auto* p = L->gc.create<Proto>(GcKind::Proto);
  p->source = source_name;
  FuncState fs;
  fs.L = L;
  fs.proto = p;
  // main chunk is vararg and has _ENV as upvalue 0 (instack from fake env at register — use upval)
  p->is_vararg = true;
  UpvalDesc env;
  env.name = "_ENV";
  env.instack = false;
  env.idx = 0;
  // Special: main closure's upval 0 will be bound to globals table by loader
  p->upvalues.push_back(env);
  fs.freereg = 0;
  block(fs, *chunk.body);
  for (auto& g : fs.pending_gotos) {
    auto it = fs.labels.find(g.name);
    if (it == fs.labels.end())
      panic("no visible label for goto: " + g.name);
    int a = FuncState::goto_close_a(g.nactvar, it->second.nactvar);
    int sbx = it->second.pc - (g.jmp_pc + 1);
    fs.proto->code[static_cast<size_t>(g.jmp_pc)] =
        encode_asbx(OpCode::JMP, static_cast<uint8_t>(a), sbx);
  }
  fs.code_abc(OpCode::RETURN, 0, 1, 0);
  fs.flush_locvars_to(0);
  p->maxstack = std::max(fs.maxstack, 2);
  return p;
}

} // namespace luatier
