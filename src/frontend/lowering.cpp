#include "frontend/lowering.hpp"

#include "runtime/string.hpp"

#include <algorithm>
#include <utility>
#include <unordered_map>

namespace lj3 {

namespace {

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
  std::unordered_map<std::string, int> labels;      // name -> pc
  std::vector<std::pair<std::string, int>> pending_gotos; // name, jmp_pc
  bool vararg = false;
  int lastline = 1;

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
    for (size_t i = 0; i < proto->constants.size(); ++i) {
      if (values_equal(proto->constants[i], v))
        return static_cast<int>(i);
    }
    proto->constants.push_back(v);
    return static_cast<int>(proto->constants.size() - 1);
  }
  int string_k(const std::string& s) {
    return const_index(TValue::obj(ValueTag::String, L->intern(s)));
  }
  int push_local(const std::string& name, int reg, int line) {
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
  void leave_block(int nlocals_before) {
    int close_level = -1;
    int nil_from = -1;
    int nil_to = -1;
    while (static_cast<int>(locals.size()) > nlocals_before) {
      auto& loc = locals.back();
      if (loc.captured) {
        // Close from the lowest captured register upward (Lua: close >= level).
        if (close_level < 0 || loc.reg < close_level)
          close_level = loc.reg;
      } else {
        // Clear non-captured locals so they do not keep objects alive after the
        // scope ends (needed for weak-table / __gc tests).
        if (nil_from < 0 || loc.reg < nil_from)
          nil_from = loc.reg;
        if (nil_to < 0 || loc.reg > nil_to)
          nil_to = loc.reg;
      }
      LocVar lv;
      lv.name = loc.name;
      lv.startpc = loc.startpc;
      lv.endpc = pc();
      proto->locvars.push_back(lv);
      locals.pop_back();
    }
    if (nil_from >= 0 && close_level < 0) {
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
    if (close_level >= 0) {
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

struct Expdesc {
  enum Kind { Void, Relocable, Nonrelocable, Constant, Local, Upval, Global, Indexed, Call } kind =
      Void;
  int info = 0;
  int t = -1; // patch list true
  int f = -1; // patch list false
  int reg = -1;
  TValue k{};
  int table = -1;
  int key = -1;
  bool key_is_rk = false;
};

void expr(FuncState& fs, Expdesc& e, Expr& node);
void statement(FuncState& fs, AstNode& node);
void block(FuncState& fs, Block& b);

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

int add_upval(FuncState& fs, const std::string& name, bool instack, int idx) {
  for (size_t i = 0; i < fs.proto->upvalues.size(); ++i) {
    auto& u = fs.proto->upvalues[i];
    if (u.instack == instack && u.idx == static_cast<uint8_t>(idx))
      return static_cast<int>(i);
  }
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
  // global via _ENV upvalue index 0
  e.kind = Expdesc::Global;
  e.info = fs.string_k(name);
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
    fs.code_abc(OpCode::GETTABUP, e.reg, 0, static_cast<uint8_t>(e.info > 255 ? 255 : e.info));
    // if const index > 255 use LOADK path — keep indices small for v0.1
    if (e.info > 255)
      panic("too many constants");
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
    fs.code_abc(OpCode::GETTABLE, r, e.table, e.key);
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
    int r = exp2reg(fs, o);
    int dest = fs.new_reg();
    OpCode op = OpCode::UNM;
    if (n.op == UnOp::Not)
      op = OpCode::NOT;
    else if (n.op == UnOp::Len)
      op = OpCode::LEN;
    else if (n.op == UnOp::Bnot)
      op = OpCode::BNOT;
    fs.code_abc(op, dest, r, 0);
    fs.free_reg(r);
    e.kind = Expdesc::Nonrelocable;
    e.reg = dest;
    break;
  }
  case AstKind::ExprBin: {
    auto& n = static_cast<ExprBin&>(node);
    if (n.op == BinOp::And || n.op == BinOp::Or) {
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
      fs.code_abc(cmp, acond, left, right);
      int j = fs.code_asbx(OpCode::JMP, 0, 1); // skip next if false path — Lua style:
      // EQ: if ((b==c) != a) pc++; then typically JMP
      // We'll emit: cmp; JMP +1; LOADBOOL dest 0 1; LOADBOOL dest 1 0
      fs.code_abc(OpCode::LOADBOOL, dest, 0, 1);
      fs.code_abc(OpCode::LOADBOOL, dest, 1, 0);
      fs.fix_sbx(j, fs.pc() - 2);
      // Actually classic Lua:
      // EQ A B C ; if (B==C)~=A then pc++
      // JMP 1
      // LOADBOOL dest 0 1
      // LOADBOOL dest 1 0
      // Let me re-emit cleanly:
      fs.proto->code.resize(static_cast<size_t>(fs.pc() - 4));
      fs.proto->lineinfo.resize(fs.proto->code.size());
      fs.freereg = dest;
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
    int dest = fs.new_reg();
    fs.code_abc(bin_arith_op(n.op), dest, rb, rc);
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
    int rk = exp2reg(fs, k);
    e.kind = Expdesc::Indexed;
    e.table = rt;
    e.key = rk;
    break;
  }
  case AstKind::ExprField: {
    auto& n = static_cast<ExprField&>(node);
    Expdesc t;
    expr(fs, t, *n.table);
    int rt = exp2reg(fs, t);
    int k = fs.string_k(n.field);
    int rk = fs.new_reg();
    fs.code_abx(OpCode::LOADK, rk, k);
    e.kind = Expdesc::Indexed;
    e.table = rt;
    e.key = rk;
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
      fs.code_abc(OpCode::GETTABLE, base, c.table, c.key);
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
    fs.code_abc(OpCode::CALL, base, static_cast<uint8_t>(b_field), 2); // 1 result by default
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
    // Array fields go into registers R[r+1]... then SETLIST (supports trailing multret).
    int arr = 0;
    bool arr_multret = false;
    for (size_t fi = 0; fi < n.fields.size(); ++fi) {
      auto& f = n.fields[fi];
      if (!f.name.empty()) {
        int k = fs.new_reg();
        fs.code_abx(OpCode::LOADK, k, fs.string_k(f.name));
        Expdesc v;
        expr(fs, v, *f.value);
        int rv = exp2reg(fs, v);
        fs.code_abc(OpCode::SETTABLE, r, k, rv);
        fs.freereg = r + 1 + arr;
      } else if (f.key) {
        Expdesc k, v;
        expr(fs, k, *f.key);
        int rk = exp2reg(fs, k);
        expr(fs, v, *f.value);
        int rv = exp2reg(fs, v);
        fs.code_abc(OpCode::SETTABLE, r, rk, rv);
        fs.freereg = r + 1 + arr;
      } else {
        bool last_array = true;
        for (size_t j = fi + 1; j < n.fields.size(); ++j) {
          if (n.fields[j].name.empty() && !n.fields[j].key) {
            last_array = false;
            break;
          }
        }
        int dest = r + 1 + arr;
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
        ++arr;
        fs.freereg = dest + 1;
      }
    }
    if (arr > 0 || arr_multret) {
      int b = arr_multret ? 0 : arr;
      // C=1 => flush block starting at index 1
      fs.code_abc(OpCode::SETLIST, r, static_cast<uint8_t>(b), 1);
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
    // _ENV upvalue
    add_upval(child, "_ENV", false, 0);
    child.freereg = 0;
    for (auto& param : n.params) {
      int reg = child.new_reg();
      child.push_local(param, reg, node.line);
    }
    block(child, *n.body);
    child.lastline = n.lastline > 0 ? n.lastline : child.lastline;
    child.code_abc(OpCode::RETURN, 0, 1, 0, child.lastline);
    p->lastlinedefined = n.lastline > 0 ? n.lastline : node.line;
    for (int li : p->lineinfo)
      if (li > p->lastlinedefined)
        p->lastlinedefined = li;
    p->maxstack = std::max(child.maxstack, 2);
    int dest = fs.new_reg();
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
    fs.code_abc(OpCode::SETTABUP, 0, static_cast<uint8_t>(var.info), expr_reg);
  } else if (var.kind == Expdesc::Indexed) {
    fs.code_abc(OpCode::SETTABLE, var.table, var.key, expr_reg);
  } else {
    panic("invalid assignment target");
  }
}

void block(FuncState& fs, Block& b) {
  int saved = static_cast<int>(fs.locals.size());
  for (auto& s : b.stmts)
    statement(fs, *s);
  fs.leave_block(saved);
}

void statement(FuncState& fs, AstNode& node) {
  fs.lastline = node.line;
  switch (node.kind) {
  case AstKind::LocalDecl: {
    fs.freereg = fs.nactvar;
    auto& n = static_cast<LocalDecl&>(node);
    int nnames = static_cast<int>(n.names.size());
    int base = fs.freereg;
    fs.reserve(nnames);
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
      fs.freereg = std::max(fs.freereg, dest + 1);
      Expdesc e;
      expr(fs, e, *n.values[i]);
      int er = exp2reg(fs, e);
      if (er != dest)
        fs.code_abc(OpCode::MOVE, dest, er, 0);
      filled = static_cast<int>(i) + 1;
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
    std::vector<Expdesc> vars;
    for (auto& v : n.vars) {
      Expdesc e;
      expr(fs, e, *v);
      vars.push_back(e);
    }
    int nvars = static_cast<int>(vars.size());
    int base = fs.freereg;
    fs.reserve(nvars);
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
      fs.freereg = std::max(fs.freereg, dest + 1);
      Expdesc e;
      expr(fs, e, *n.values[i]);
      int er = exp2reg(fs, e);
      if (er != dest)
        fs.code_abc(OpCode::MOVE, dest, er, 0);
      filled = static_cast<int>(i) + 1;
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
    } else {
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
        if (fs.freereg <= dest)
          fs.reserve(dest - fs.freereg + 1);
        int hold = fs.freereg;
        Expdesc e;
        expr(fs, e, *n.values[i]);
        int r = exp2reg(fs, e);
        if (r != dest)
          fs.code_abc(OpCode::MOVE, dest, r, 0);
        fs.freereg = std::max(hold, dest + 1);
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
      fs.code_abc(OpCode::TEST, cr, 0, 0);
      int jf = fs.code_asbx(OpCode::JMP, 0, 0);
      fs.freereg = fs.nactvar;
      block(fs, *n.branches[i].body);
      int je = fs.code_asbx(OpCode::JMP, 0, 0);
      escape.push_back(je);
      fs.fix_sbx(jf, fs.pc());
    }
    if (n.else_body)
      block(fs, *n.else_body);
    for (int j : escape)
      fs.fix_sbx(j, fs.pc());
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
    block(fs, *n.body);
    fs.code_asbx(OpCode::JMP, 0, loop - (fs.pc() + 1));
    fs.fix_sbx(jf, fs.pc());
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), fs.pc());
      fs.break_list.pop_back();
    }
    break;
  }
  case AstKind::Repeat: {
    auto& n = static_cast<RepeatStmt&>(node);
    int saved = static_cast<int>(fs.locals.size());
    int loop = fs.pc();
    int breaks_before = static_cast<int>(fs.break_list.size());
    for (auto& s : n.body->stmts)
      statement(fs, *s);
    Expdesc c;
    expr(fs, c, *n.cond);
    int cr = exp2reg(fs, c);
    fs.code_abc(OpCode::TEST, cr, 0, 1); // exit if true
    int jf = fs.code_asbx(OpCode::JMP, 0, 0);
    fs.code_asbx(OpCode::JMP, 0, loop - (fs.pc() + 1));
    fs.fix_sbx(jf, fs.pc());
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), fs.pc());
      fs.break_list.pop_back();
    }
    fs.leave_block(saved);
    break;
  }
  case AstKind::Break: {
    fs.break_list.push_back(fs.code_asbx(OpCode::JMP, 0, 0));
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
    int prep = fs.code_asbx(OpCode::FORPREP, base, 0);
    int loop = fs.pc();
    int ireg = fs.new_reg();
    fs.push_local(n.name, ireg, node.line);
    // FORPREP uses base..base+2 and internal index at base+3
    // Align with Lua: R(A) index, R(A+1) limit, R(A+2) step, R(A+3) local
    // Our ireg should be base+3
    if (ireg != base + 3) {
      // force
    }
    fs.locals.back().reg = base + 3;
    fs.nactvar = base + 4;
    fs.freereg = base + 4;
    int breaks_before = static_cast<int>(fs.break_list.size());
    block(fs, *n.body);
    fs.code_asbx(OpCode::FORLOOP, base, loop - (fs.pc() + 1));
    fs.fix_sbx(prep, fs.pc());
    // fix prep jump to after FORPREP targeting FORLOOP properly:
    // FORPREP sbx jumps to FORLOOP; FORLOOP jumps back to loop body start
    fs.fix_sbx(prep, loop - 1); // standard: prep jumps to forloop instruction
    // Actually Lua: FORPREP A sBx -> pc += sBx (points to FORLOOP), FORLOOP jumps back with sBx
    // Re-fix: prep should jump to FORLOOP pc
    int forloop_pc = fs.pc() - 1;
    fs.fix_sbx(prep, forloop_pc);
    fs.proto->code[forloop_pc] =
        encode_asbx(OpCode::FORLOOP, static_cast<uint8_t>(base), loop - (forloop_pc + 1));
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), fs.pc());
      fs.break_list.pop_back();
    }
    fs.leave_block(static_cast<int>(fs.locals.size()) - 1);
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
    Expdesc e;
    expr(fs, e, *n.fn);
    int er = exp2reg(fs, e);
    if (n.name_path.size() == 1) {
      Expdesc g;
      g.kind = Expdesc::Global;
      g.info = fs.string_k(n.name_path[0]);
      storevar(fs, g, er);
    } else {
      // a.b.c = fn
      Expdesc t;
      search_var(fs, n.name_path[0], t);
      int reg = exp2reg(fs, t);
      for (size_t i = 1; i + 1 < n.name_path.size(); ++i) {
        int k = fs.new_reg();
        fs.code_abx(OpCode::LOADK, k, fs.string_k(n.name_path[i]));
        int d = fs.new_reg();
        fs.code_abc(OpCode::GETTABLE, d, reg, k);
        reg = d;
      }
      int k = fs.new_reg();
      fs.code_abx(OpCode::LOADK, k, fs.string_k(n.name_path.back()));
      fs.code_abc(OpCode::SETTABLE, reg, k, er);
    }
    fs.freereg = fs.nactvar;
    break;
  }
  case AstKind::DoBlock: {
    auto& n = static_cast<DoBlock&>(node);
    block(fs, *n.body);
    break;
  }
  case AstKind::Label: {
    auto& n = static_cast<LabelStmt&>(node);
    if (fs.labels.count(n.name))
      panic("label already defined: " + n.name);
    fs.labels[n.name] = fs.pc();
    break;
  }
  case AstKind::Goto: {
    auto& n = static_cast<GotoStmt&>(node);
    auto it = fs.labels.find(n.label);
    if (it != fs.labels.end()) {
      fs.code_asbx(OpCode::JMP, 0, it->second - (fs.pc() + 1));
    } else {
      int j = fs.code_asbx(OpCode::JMP, 0, 0);
      fs.pending_gotos.emplace_back(n.label, j);
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
    block(fs, *n.body);
    fs.leave_block(nlocals_before);
    fs.fix_sbx(jprep, fs.pc());
    fs.code_abc(OpCode::TFORCALL, base, 0, nvars);
    int tforloop = fs.code_asbx(OpCode::TFORLOOP, base + 2, 0);
    // fix_sbx(pc, dest) encodes sbx = dest - (pc + 1); pass absolute dest.
    fs.fix_sbx(tforloop, loopbody);
    while (static_cast<int>(fs.break_list.size()) > breaks_before) {
      fs.fix_sbx(fs.break_list.back(), fs.pc());
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
    auto it = fs.labels.find(g.first);
    if (it == fs.labels.end())
      panic("no visible label for goto: " + g.first);
    fs.fix_sbx(g.second, it->second);
  }
  fs.code_abc(OpCode::RETURN, 0, 1, 0);
  p->maxstack = std::max(fs.maxstack, 2);
  return p;
}

} // namespace lj3
