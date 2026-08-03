#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "vm/debug_hook.hpp"
#include "vm/interpreter.hpp"
#include "vm/ldebug.hpp"
#include "vm/meta.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

namespace luatier {
using namespace lib;

namespace {

constexpr int kIdSize = 60 - 1; // LUA_IDSIZE (60) minus trailing NUL
constexpr const char* kDefaultWhat = "Slnuft";
constexpr const char* kValidWhat = "nSlutTfL";

std::string chunkid(std::string_view source) {
  return format_chunkid(source);
}

void validate_what(std::string_view what) {
  for (char c : what) {
    if (!std::strchr(kValidWhat, c))
      panic("invalid option");
  }
}

bool what_has(std::string_view what, char c) {
  return what.find(c) != std::string_view::npos;
}

struct DebugTarget {
  Thread* th = nullptr;
  Closure* cl = nullptr;
  Proto* proto = nullptr;
  CallFrame* frame = nullptr;
  bool is_c = false;
};

bool resolve_target(State* L, int arg, DebugTarget* out, Thread* th_opt = nullptr) {
  Thread* th = th_opt ? th_opt : L->current;
  // Only auto-detect thread at arg when caller did not already bind one.
  if (!th_opt && L->gettop() >= arg && L->at(arg)->is_thread()) {
    th = L->at(arg)->as_thread();
    ++arg;
  }
  if (L->gettop() < arg)
    panic("level or function expected");

  out->th = th;

  if (L->at(arg)->is_function()) {
    out->cl = L->at(arg)->as_closure();
    out->is_c = out->cl->is_c;
    out->proto = out->cl->is_c ? nullptr : out->cl->proto;
    return true;
  }

  if (!L->at(arg)->is_number())
    panic("number or function expected");

  double dn = L->at(arg)->to_number();
  int level = static_cast<int>(dn);
  if (static_cast<double>(level) != dn)
    return false;

  // level 0 = top frame (often a C frame); level 1 = its caller; …
  if (level < 0 || level >= static_cast<int>(th->frames.size()))
    return false;

  CallFrame& fr = th->frames[static_cast<size_t>(th->frames.size() - 1 - static_cast<size_t>(level))];
  out->frame = &fr;
  out->cl = fr.cl;
  out->proto = fr.proto;
  out->is_c = !out->proto || (out->cl && out->cl->is_c);
  return true;
}

void set_field_str(State* L, Table* t, const char* name, std::string_view value) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)),
         TValue::obj(ValueTag::String, L->intern(value)));
}

void set_field_int(State* L, Table* t, const char* name, int64_t value) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)), TValue::integer(value));
}

constexpr const char* kEnvName = "_ENV";

const char* local_name(Proto* p, int reg, int pc) {
  for (const auto& lv : p->locvars) {
    if (lv.reg == reg && lv.startpc <= pc && pc < lv.endpc)
      return lv.name.c_str();
  }
  return nullptr;
}

const char* upval_name(Proto* p, int uv) {
  if (uv < 0 || static_cast<size_t>(uv) >= p->upvalues.size())
    return "?";
  const std::string& n = p->upvalues[static_cast<size_t>(uv)].name;
  return n.empty() ? "?" : n.c_str();
}

int filter_pc(int pc, int jmptarget) {
  return (pc < jmptarget) ? -1 : pc;
}

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

const char* getobjname(Proto* p, int lastpc, int reg, const char** name, int depth);

void kname_reg(Proto* p, int pc, int reg, const char** name) {
  const char* what = getobjname(p, pc, reg, name, 0);
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

const char* getobjname(Proto* p, int lastpc, int reg, const char** name, int depth) {
  *name = local_name(p, reg, lastpc);
  if (*name)
    return "local";

  const int pc = findsetreg(p, lastpc, reg);
  if (pc < 0)
    return nullptr;

  Instruction i = p->code[static_cast<size_t>(pc)];
  OpCode op = static_cast<OpCode>(op_get(i));
  switch (op) {
  case OpCode::MOVE: {
    // PUC only follows b < a; our lowering often MOVE-s into a lower call base.
    int b = op_b(i);
    if (b != op_a(i) && depth < 8)
      return getobjname(p, pc, b, name, depth + 1);
    break;
  }
  case OpCode::GETTABUP:
  case OpCode::GETTABLE:
  case OpCode::GETFIELD: {
    const char* vn = nullptr;
    if (op == OpCode::GETTABLE)
      vn = local_name(p, op_b(i), pc);
    else if (op == OpCode::GETFIELD)
      vn = local_name(p, op_b(i), pc);
    else
      vn = upval_name(p, op_b(i));

    if (op == OpCode::GETTABLE)
      kname_reg(p, pc, op_c(i), name);
    else
      kname_k(p, op_c(i), name);

    if (vn && std::strcmp(vn, kEnvName) == 0)
      return "global";
    return "field";
  }
  case OpCode::GETUPVAL:
    *name = upval_name(p, op_b(i));
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

const char* funcnamefromcode(CallFrame* caller, const char** name) {
  if (!caller)
    return nullptr;
  // Interrupted by a debug hook → callee is reported as namewhat "hook".
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
    return getobjname(p, pc, op_a(i), name, 0);
  case OpCode::TFORCALL:
    *name = "for iterator";
    return "for iterator";
  // Calls through metamethods (PUC ldebug.c funcnamefromcode).
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

CallFrame* caller_frame(Thread* th, CallFrame* fr) {
  for (size_t i = 0; i < th->frames.size(); ++i) {
    if (&th->frames[i] == fr) {
      if (i == 0)
        return nullptr;
      return &th->frames[i - 1];
    }
  }
  return nullptr;
}

const char* getfuncname(Thread* th, CallFrame* fr, const char** name) {
  if (!fr || !fr->cl)
    return nullptr;
  // PUC CIST_FIN: finalizer reported as metamethod __gc.
  if (fr->finalizer) {
    *name = "__gc";
    return "metamethod";
  }
  if (fr->cl->is_c || !fr->proto)
    return nullptr;
  CallFrame* caller = caller_frame(th, fr);
  return funcnamefromcode(caller, name);
}

int frame_current_pc(const CallFrame& fr) {
  int pc = fr.saved_pc - 1;
  return pc < 0 ? 0 : pc;
}

// Resolve stack level: 0 = top frame; 1 = its caller; …
bool resolve_level(Thread* th, int level, CallFrame** fr_out, bool* is_c) {
  *fr_out = nullptr;
  *is_c = false;
  if (level < 0 || level >= static_cast<int>(th->frames.size()))
    return false;
  *fr_out = &th->frames[static_cast<size_t>(th->frames.size() - 1 - static_cast<size_t>(level))];
  *is_c = !(*fr_out)->proto || ((*fr_out)->cl && (*fr_out)->cl->is_c);
  return true;
}

void fill_name_fields(State* L, Table* info, Thread* th, CallFrame* frame, Closure* cl) {
  // Finalizer carrying frame may be C (e.g. collectgarbage); check before
  // the C-frame early-out (PUC CIST_FIN on getinfo level 2).
  if (frame && frame->finalizer) {
    set_field_str(L, info, "namewhat", "metamethod");
    set_field_str(L, info, "name", "__gc");
    return;
  }
  if (!frame || !cl || cl->is_c || !cl->proto) {
    set_field_str(L, info, "namewhat", "");
    info->set(L, TValue::obj(ValueTag::String, L->intern("name")), TValue::nil());
    return;
  }

  const char* fname = nullptr;
  const char* namewhat = getfuncname(th, frame, &fname);
  if (namewhat) {
    set_field_str(L, info, "namewhat", namewhat);
    if (fname)
      set_field_str(L, info, "name", fname);
    else
      info->set(L, TValue::obj(ValueTag::String, L->intern("name")), TValue::nil());
  } else {
    set_field_str(L, info, "namewhat", "");
    info->set(L, TValue::obj(ValueTag::String, L->intern("name")), TValue::nil());
  }
}

void fill_activelines(State* L, Table* info, Proto* proto) {
  Table* lines = table_new(L, 0, 16);
  std::unordered_set<int> seen;
  for (int li : proto->lineinfo) {
    if (li <= 0 || !seen.insert(li).second)
      continue;
    lines->set_int(L, li, TValue::boolean(true));
  }
  info->set(L, TValue::obj(ValueTag::String, L->intern("activelines")),
            TValue::obj(ValueTag::Table, lines));
}

Table* build_info(State* L, const DebugTarget& target, std::string_view what, int func_arg) {
  Table* info = table_new(L, 0, 16);

  if (what_has(what, 'S')) {
    if (target.is_c) {
      set_field_str(L, info, "source", "[C]");
      set_field_str(L, info, "short_src", "[C]");
      set_field_str(L, info, "what", "C");
    } else if (target.proto) {
      set_field_str(L, info, "source", target.proto->source);
      set_field_str(L, info, "short_src", chunkid(target.proto->source));
      // PUC: main chunk is linedefined==0 → what="main"; else "Lua".
      set_field_str(L, info, "what",
                    target.proto->linedefined == 0 ? "main" : "Lua");
      set_field_int(L, info, "linedefined", target.proto->linedefined);
      set_field_int(L, info, "lastlinedefined", target.proto->lastlinedefined);
    }
  }

  if (what_has(what, 'l')) {
    int currentline = -1;
    if (target.frame && target.proto) {
      int pc = frame_current_pc(*target.frame);
      if (pc >= 0 && pc < static_cast<int>(target.proto->lineinfo.size()))
        currentline = target.proto->lineinfo[static_cast<size_t>(pc)];
    }
    set_field_int(L, info, "currentline", currentline);
  }

  if (what_has(what, 'u')) {
    int nups = target.cl ? static_cast<int>(target.cl->upvals.size()) : 0;
    set_field_int(L, info, "nups", nups);
    // PUC: all C functions are vararg with nparams=0.
    if (target.is_c || !target.proto) {
      info->set(L, TValue::obj(ValueTag::String, L->intern("isvararg")),
                TValue::boolean(true));
      set_field_int(L, info, "nparams", 0);
    } else {
      info->set(L, TValue::obj(ValueTag::String, L->intern("isvararg")),
                TValue::boolean(target.proto->is_vararg));
      set_field_int(L, info, "nparams", target.proto->numparams);
    }
  }

  if (what_has(what, 't') || what_has(what, 'T')) {
    bool istail = target.frame && target.frame->tailcall;
    info->set(L, TValue::obj(ValueTag::String, L->intern("istailcall")),
              TValue::boolean(istail));
  }

  if (what_has(what, 'n'))
    fill_name_fields(L, info, target.th, target.frame, target.cl);

  if (what_has(what, 'f')) {
    if (func_arg > 0 && L->at(func_arg)->is_function())
      info->set(L, TValue::obj(ValueTag::String, L->intern("func")), *L->at(func_arg));
    else if (target.cl)
      info->set(L, TValue::obj(ValueTag::String, L->intern("func")),
                TValue::obj(ValueTag::Function, target.cl));
  }

  if (what_has(what, 'L')) {
    if (target.is_c || !target.proto)
      info->set(L, TValue::obj(ValueTag::String, L->intern("activelines")), TValue::nil());
    else
      fill_activelines(L, info, target.proto);
  }

  return info;
}

} // namespace

static int debug_getinfo(State* L) {
  int arg = 1;
  int func_arg = 1;
  Thread* th = L->current;
  if (L->gettop() >= 1 && L->at(1)->is_thread()) {
    th = L->at(1)->as_thread();
    arg = 2;
    func_arg = 2;
  }

  int what_arg = arg + 1;
  std::string_view what = opt_string(L, what_arg, kDefaultWhat);
  validate_what(what);

  DebugTarget target;
  if (!resolve_target(L, arg, &target, th)) {
    L->push(TValue::nil());
    return 1;
  }

  Table* info = build_info(L, target, what, func_arg);
  L->push(TValue::obj(ValueTag::Table, info));
  return 1;
}

// PUC lauxlib findfield: search table (and nested tables) for `func`.
bool findfield(State* L, Table* t, Closure* func, int level, std::string* out) {
  if (!t || !func || level <= 0)
    return false;
  auto consider = [&](const TValue& key, const TValue& val) -> bool {
    if (!key.is_string())
      return false;
    if (val.is_function() && val.as_closure() == func) {
      *out = key.as_string()->view();
      return true;
    }
    if (val.is_table() && level > 1) {
      std::string nested;
      if (findfield(L, val.as_table(), func, level - 1, &nested)) {
        *out = std::string(key.as_string()->view()) + "." + nested;
        return true;
      }
    }
    return false;
  };
  for (size_t i = 1; i <= t->array.size(); ++i) {
    TValue key = TValue::integer(static_cast<int64_t>(i));
    if (consider(key, t->array[i - 1]))
      return true;
  }
  for (auto& n : t->hash) {
    if (!n.used)
      continue;
    if (consider(n.key, n.value))
      return true;
  }
  return false;
}

// PUC pushglobalfuncname: name a C/Lua function via package.loaded.
bool pushglobalfuncname(State* L, Closure* func, std::string* out) {
  if (!func)
    return false;
  TValue pkg_v = L->globals->get(TValue::obj(ValueTag::String, L->intern("package")));
  if (pkg_v.is_table()) {
    TValue loaded_v =
        pkg_v.as_table()->get(TValue::obj(ValueTag::String, L->intern("loaded")));
    if (loaded_v.is_table()) {
      std::string name;
      if (findfield(L, loaded_v.as_table(), func, 2, &name)) {
        if (name.rfind("_G.", 0) == 0)
          name = name.substr(3);
        *out = std::move(name);
        return true;
      }
    }
  }
  // Fallback: search _G at depth 2 (e.g. coroutine.yield, string.find).
  std::string name;
  if (L->globals && findfield(L, L->globals, func, 2, &name)) {
    *out = std::move(name);
    return true;
  }
  return false;
}

std::string traceback_funcname(State* L, Thread* th, CallFrame* fr, bool is_c) {
  if (is_c) {
    std::string gname;
    if (fr && fr->cl && pushglobalfuncname(L, fr->cl, &gname))
      return "function '" + gname + "'";
    const char* fname = nullptr;
    const char* namewhat = getfuncname(th, fr, &fname);
    if (namewhat && *namewhat && fname)
      return std::string(namewhat) + " '" + fname + "'";
    return "?";
  }
  if (!fr || !fr->proto)
    return "?";
  std::string gname;
  if (fr->cl && pushglobalfuncname(L, fr->cl, &gname))
    return "function '" + gname + "'";
  const char* fname = nullptr;
  const char* namewhat = getfuncname(th, fr, &fname);
  if (namewhat && *namewhat && fname)
    return std::string(namewhat) + " '" + fname + "'";
  if (fr->proto->linedefined == 0)
    return "main chunk";
  return "function <" + chunkid(fr->proto->source) + ":" +
         std::to_string(fr->proto->linedefined) + ">";
}

static int debug_traceback(State* L) {
  int arg = 1;
  Thread* th = L->current;
  if (L->gettop() >= 1 && L->at(1)->is_thread()) {
    th = L->at(1)->as_thread();
    arg = 2;
  }

  // Non-string message: return it untouched (Lua 5.3).
  if (L->gettop() >= arg && !L->at(arg)->is_nil() && !L->at(arg)->is_string() &&
      !L->at(arg)->is_number()) {
    TValue v = *L->at(arg);
    L->settop(0);
    L->push(v);
    return 1;
  }

  std::string msg;
  if (L->gettop() >= arg && !L->at(arg)->is_nil())
    msg = value_to_string(*L->at(arg));
  int level = 1;
  if (L->gettop() >= arg + 1 && L->at(arg + 1)->is_number())
    level = static_cast<int>(L->at(arg + 1)->to_number());
  else if (th != L->current)
    level = 0;

  std::string tb;
  if (!msg.empty()) {
    tb = msg;
    tb += '\n';
  }
  tb += "stack traceback:";

  // PUC luaL_traceback: keep LEVELS1 head + LEVELS2 tail with a "..." elision.
  constexpr int kLevels1 = 10;
  constexpr int kLevels2 = 11;
  const int nframes = static_cast<int>(th->frames.size());
  // Highest valid debug level is nframes-1 (resolve_level).
  const int last = nframes > 0 ? nframes - 1 : -1;
  int n1 = (last - level > kLevels1 + kLevels2) ? kLevels1 : -1;

  for (int lvl = level; lvl <= last; ++lvl) {
    // Match `if (n1-- == 0) { ... } else { print }` from luaL_traceback.
    if (n1 == 0) {
      tb += "\n\t...";
      --n1;
      lvl = last - kLevels2; // for-loop ++lvl → last - LEVELS2 + 1
      continue;
    }
    if (n1 > 0)
      --n1;

    CallFrame* fr = nullptr;
    bool is_c = false;
    if (!resolve_level(th, lvl, &fr, &is_c))
      break;
    if (is_c) {
      tb += "\n\t[C]: in ";
      tb += traceback_funcname(L, th, fr, true);
      continue;
    }
    if (!fr || !fr->proto)
      continue;

    std::string short_src = chunkid(fr->proto->source);
    int currentline = -1;
    int pc = frame_current_pc(*fr);
    if (pc >= 0 && pc < static_cast<int>(fr->proto->lineinfo.size()))
      currentline = fr->proto->lineinfo[static_cast<size_t>(pc)];

    tb += "\n\t";
    tb += short_src;
    if (currentline > 0) {
      tb += ':';
      tb += std::to_string(currentline);
    }
    tb += ": in ";
    tb += traceback_funcname(L, th, fr, false);
    if (fr->tailcall)
      tb += "\n\t(...tail calls...)";
  }

  L->settop(0);
  push_string(L, tb);
  return 1;
}

// n-th active local at pc (1-based), ordered by register (PUC packs locals at 0..k-1).
// Our locvars are flushed on leave-block, so array order is not declaration order.
const char* proto_getlocalname(Proto* p, int n, int pc, int* reg_out) {
  if (!p || n <= 0)
    return nullptr;
  std::vector<const LocVar*> active;
  active.reserve(p->locvars.size());
  for (const auto& lv : p->locvars) {
    if (lv.startpc <= pc && pc < lv.endpc)
      active.push_back(&lv);
  }
  std::sort(active.begin(), active.end(),
            [](const LocVar* a, const LocVar* b) { return a->reg < b->reg; });
  if (static_cast<size_t>(n) > active.size())
    return nullptr;
  const LocVar* lv = active[static_cast<size_t>(n - 1)];
  if (reg_out)
    *reg_out = lv->reg;
  return lv->name.c_str();
}

// Find local/vararg/temporary; returns name or nullptr. *slot is writable storage.
const char* find_local(Thread* th, CallFrame* fr, bool is_c, int n, TValue** slot) {
  *slot = nullptr;
  if (!fr || !fr->cl)
    return nullptr;

  if (is_c || !fr->proto || fr->cl->is_c) {
    // C frame: slots are arguments (and temps) above the function.
    int base = fr->base + 1;
    int limit = th->top;
    size_t fi = 0;
    for (; fi < th->frames.size(); ++fi) {
      if (&th->frames[fi] == fr)
        break;
    }
    if (fi + 1 < th->frames.size())
      limit = th->frames[fi + 1].base;
    else if (th->stack_base > fr->base)
      limit = th->top; // active C window
    if (n <= 0 || n > limit - base)
      return nullptr;
    *slot = &th->stack[static_cast<size_t>(base + (n - 1))];
    return "(*temporary)";
  }

  Proto* p = fr->proto;
  if (n < 0) {
    int i = -n;
    if (i < 1 || static_cast<size_t>(i) > fr->varargs.size())
      return nullptr;
    *slot = &fr->varargs[static_cast<size_t>(i - 1)];
    return "(*vararg)";
  }

  int base = fr->base;
  int pc = frame_current_pc(*fr);
  int reg = n - 1;
  const char* name = proto_getlocalname(p, n, pc, &reg);
  if (!name) {
    int limit = th->top;
    size_t fi = 0;
    for (; fi < th->frames.size(); ++fi) {
      if (&th->frames[fi] == fr)
        break;
    }
    if (fi + 1 < th->frames.size())
      limit = th->frames[fi + 1].base;
    else if (th->stack_base > fr->base)
      limit = th->stack_base; // C call sits above this Lua frame
    if (n <= 0 || n > limit - base)
      return nullptr;
    name = "(*temporary)";
    reg = n - 1;
  }
  *slot = &th->stack[static_cast<size_t>(base + reg)];
  return name;
}

static int debug_getlocal(State* L) {
  int arg = 1;
  Thread* th = L->current;
  if (L->gettop() >= 1 && L->at(1)->is_thread()) {
    th = L->at(1)->as_thread();
    arg = 2;
  }
  check_any(L, arg, "getlocal");
  check_any(L, arg + 1, "getlocal");
  int nvar = static_cast<int>(check_int(L, arg + 1));

  // Function form: return parameter name only (at pc 0).
  if (L->at(arg)->is_function()) {
    Closure* cl = L->at(arg)->as_closure();
    const char* name = nullptr;
    if (!cl->is_c && cl->proto)
      name = proto_getlocalname(cl->proto, nvar, 0, nullptr);
    L->settop(0);
    if (name)
      push_string(L, name);
    else
      L->push(TValue::nil());
    return 1;
  }

  int level = static_cast<int>(check_int(L, arg));
  CallFrame* fr = nullptr;
  bool is_c = false;
  if (!resolve_level(th, level, &fr, &is_c))
    panic("level out of range");

  TValue* slot = nullptr;
  const char* name = find_local(th, fr, is_c, nvar, &slot);
  // Copy before settop: C-frame slots live in the API window cleared by settop.
  TValue value = (name && slot) ? *slot : TValue::nil();
  L->settop(0);
  if (!name) {
    L->push(TValue::nil());
    return 1;
  }
  push_string(L, name);
  L->push(value);
  return 2;
}

static int debug_setlocal(State* L) {
  int arg = 1;
  Thread* th = L->current;
  if (L->gettop() >= 1 && L->at(1)->is_thread()) {
    th = L->at(1)->as_thread();
    arg = 2;
  }
  int level = static_cast<int>(check_int(L, arg));
  int nvar = static_cast<int>(check_int(L, arg + 1));
  check_any(L, arg + 2, "setlocal");
  TValue value = *L->at(arg + 2);

  CallFrame* fr = nullptr;
  bool is_c = false;
  if (!resolve_level(th, level, &fr, &is_c))
    panic("level out of range");

  TValue* slot = nullptr;
  const char* name = find_local(th, fr, is_c, nvar, &slot);
  if (name && slot)
    *slot = value;
  L->settop(0);
  if (!name) {
    L->push(TValue::nil());
    return 1;
  }
  push_string(L, name);
  return 1;
}

static int debug_getupvalue(State* L) {
  check_type(L, 1, ValueTag::Function, "getupvalue");
  int n = static_cast<int>(check_int(L, 2));
  Closure* cl = L->at(1)->as_closure();
  if (n < 1 || static_cast<size_t>(n) > cl->upvals.size() || !cl->upvals[static_cast<size_t>(n - 1)])
    return 0;
  // PUC aux_upvalue: C → ""; Lua with NULL/empty name → "(*no name)".
  const char* name = "";
  if (cl->is_c) {
    name = "";
  } else if (cl->proto && static_cast<size_t>(n - 1) < cl->proto->upvalues.size()) {
    const std::string& un = cl->proto->upvalues[static_cast<size_t>(n - 1)].name;
    name = un.empty() ? "(*no name)" : un.c_str();
  } else {
    name = "(*no name)";
  }
  TValue val = cl->upvals[static_cast<size_t>(n - 1)]->get();
  L->settop(0);
  push_string(L, name);
  L->push(val);
  return 2;
}

static int debug_setupvalue(State* L) {
  check_type(L, 1, ValueTag::Function, "setupvalue");
  int n = static_cast<int>(check_int(L, 2));
  check_any(L, 3, "setupvalue");
  Closure* cl = L->at(1)->as_closure();
  if (n < 1 || static_cast<size_t>(n) > cl->upvals.size() || !cl->upvals[static_cast<size_t>(n - 1)])
    return 0;
  const char* name = "";
  if (cl->is_c) {
    name = "";
  } else if (cl->proto && static_cast<size_t>(n - 1) < cl->proto->upvalues.size()) {
    const std::string& un = cl->proto->upvalues[static_cast<size_t>(n - 1)].name;
    name = un.empty() ? "(*no name)" : un.c_str();
  } else {
    name = "(*no name)";
  }
  cl->upvals[static_cast<size_t>(n - 1)]->set(L, *L->at(3));
  L->settop(0);
  push_string(L, name);
  return 1;
}

static int debug_sethook(State* L) {
  int arg = 1;
  Thread* th = L->current;
  if (L->gettop() >= 1 && L->at(1)->is_thread()) {
    th = L->at(1)->as_thread();
    arg = 2;
  }
  if (L->gettop() < arg || L->at(arg)->is_nil()) {
    debug_sethook_thread(th, nullptr, 0, 0);
    return 0;
  }
  if (!L->at(arg)->is_function())
    panic("function expected");
  Closure* hook = L->at(arg)->as_closure();
  std::string_view mask = opt_string(L, arg + 1, "");
  int maskbits = 0;
  for (char c : mask) {
    switch (c) {
    case 'c':
      maskbits |= DEBUG_HOOK_CALL;
      break;
    case 'r':
      maskbits |= DEBUG_HOOK_RET;
      break;
    case 'l':
      maskbits |= DEBUG_HOOK_LINE;
      break;
    default:
      panic("invalid mask");
    }
  }
  int count = 0;
  if (L->gettop() >= arg + 2 && L->at(arg + 2)->is_number())
    count = static_cast<int>(L->at(arg + 2)->to_number());
  // PUC: a positive count enables LUA_MASKCOUNT even when the mask string is "".
  if (count > 0)
    maskbits |= DEBUG_HOOK_COUNT;
  debug_sethook_thread(th, hook, maskbits, count);
  return 0;
}

static int debug_gethook(State* L) {
  Thread* th = L->current;
  if (L->gettop() >= 1 && L->at(1)->is_thread())
    th = L->at(1)->as_thread();
  if (!th->hook.func) {
    L->push(TValue::nil());
    return 1;
  }
  L->push(TValue::obj(ValueTag::Function, th->hook.func));
  std::string mask;
  if (th->hook.mask & DEBUG_HOOK_CALL)
    mask += 'c';
  if (th->hook.mask & DEBUG_HOOK_RET)
    mask += 'r';
  if (th->hook.mask & DEBUG_HOOK_LINE)
    mask += 'l';
  push_string(L, mask);
  L->push(TValue::integer(th->hook.count));
  return 3;
}

static int debug_getregistry(State* L) {
  L->settop(0);
  L->push(TValue::obj(ValueTag::Table, L->registry));
  return 1;
}

static int debug_upvalueid(State* L) {
  check_type(L, 1, ValueTag::Function, "upvalueid");
  int n = static_cast<int>(check_int(L, 2));
  Closure* cl = L->at(1)->as_closure();
  if (n < 1 || static_cast<size_t>(n) > cl->upvals.size() || !cl->upvals[static_cast<size_t>(n - 1)])
    panic("invalid upvalue index");
  // PUC lua_upvalueid: Lua → UpVal*; C → address of upvalue slot.
  // We store both as UpVal*; the pointer identity is what tests compare.
  TValue id;
  id.type = static_cast<uint32_t>(ValueTag::LightUserdata);
  id.payload = reinterpret_cast<uint64_t>(cl->upvals[static_cast<size_t>(n - 1)]);
  L->settop(0);
  L->push(id);
  return 1;
}

static int debug_upvaluejoin(State* L) {
  check_type(L, 1, ValueTag::Function, "upvaluejoin");
  check_type(L, 3, ValueTag::Function, "upvaluejoin");
  int n1 = static_cast<int>(check_int(L, 2));
  int n2 = static_cast<int>(check_int(L, 4));
  Closure* f1 = L->at(1)->as_closure();
  Closure* f2 = L->at(3)->as_closure();
  if (f1->is_c || f2->is_c)
    panic("Lua function expected");
  if (n1 < 1 || static_cast<size_t>(n1) > f1->upvals.size() || !f1->upvals[static_cast<size_t>(n1 - 1)])
    panic("invalid upvalue index");
  if (n2 < 1 || static_cast<size_t>(n2) > f2->upvals.size() || !f2->upvals[static_cast<size_t>(n2 - 1)])
    panic("invalid upvalue index");
  f1->upvals[static_cast<size_t>(n1 - 1)] = f2->upvals[static_cast<size_t>(n2 - 1)];
  return 0;
}

static int debug_getmetatable(State* L) {
  Table* mt = get_metatable(L, *L->at(1));
  L->settop(0);
  if (mt)
    L->push(TValue::obj(ValueTag::Table, mt));
  else
    L->push(TValue::nil());
  return 1;
}

static int debug_setmetatable(State* L) {
  TValue obj = *L->at(1);
  Table* mt = nullptr;
  if (L->gettop() >= 2 && !L->at(2)->is_nil()) {
    if (!L->at(2)->is_table())
      panic("table expected");
    mt = L->at(2)->as_table();
  }
  if (obj.is_table()) {
    obj.as_table()->metatable = mt;
    obj.as_table()->update_weak_mode(L);
    if (mt)
      L->gc.barrier(obj.as_gc(), TValue::obj(ValueTag::Table, mt));
  } else if (obj.is_userdata()) {
    obj.as_userdata()->metatable = mt;
    if (mt)
      L->gc.barrier(obj.as_gc(), TValue::obj(ValueTag::Table, mt));
  } else {
    ValueTag t = obj.tag();
    if (t == ValueTag::Int)
      t = ValueTag::Float; // Int/Float share one number metatable
    L->type_mt[static_cast<size_t>(t)] = mt;
  }
  L->settop(0);
  L->push(obj);
  return 1;
}

static int debug_getuservalue(State* L) {
  TValue* v = L->at(1);
  L->settop(0);
  if (v->is_userdata())
    L->push(v->as_userdata()->uservalue);
  else
    L->push(TValue::nil());
  return 1;
}

static int debug_setuservalue(State* L) {
  TValue* v = L->at(1);
  if (v->tag() == ValueTag::LightUserdata)
    panic("light userdata");
  if (!v->is_userdata())
    panic("userdata expected");
  TValue uv = L->gettop() >= 2 ? *L->at(2) : TValue::nil();
  v->as_userdata()->uservalue = uv;
  L->gc.barrier(v->as_gc(), uv);
  L->settop(0);
  L->push(*v);
  return 1;
}

static int open_debug_module(State* L) {
  Table* dbg = new_lib(L, 16);
  set_field(L, dbg, "getinfo", debug_getinfo);
  set_field(L, dbg, "traceback", debug_traceback);
  set_field(L, dbg, "getlocal", debug_getlocal);
  set_field(L, dbg, "setlocal", debug_setlocal);
  set_field(L, dbg, "getupvalue", debug_getupvalue);
  set_field(L, dbg, "setupvalue", debug_setupvalue);
  set_field(L, dbg, "upvalueid", debug_upvalueid);
  set_field(L, dbg, "upvaluejoin", debug_upvaluejoin);
  set_field(L, dbg, "sethook", debug_sethook);
  set_field(L, dbg, "gethook", debug_gethook);
  set_field(L, dbg, "getregistry", debug_getregistry);
  set_field(L, dbg, "getmetatable", debug_getmetatable);
  set_field(L, dbg, "setmetatable", debug_setmetatable);
  set_field(L, dbg, "getuservalue", debug_getuservalue);
  set_field(L, dbg, "setuservalue", debug_setuservalue);
  L->settop(0);
  L->push(TValue::obj(ValueTag::Table, dbg));
  return 1;
}

void open_debug_lib(State* L) {
  open_debug_module(L);
  TValue dbg = *L->at(1);
  L->settop(0);
  set_global_value(L, "debug", dbg);
  Table* pkg = L->globals->get(TValue::obj(ValueTag::String, L->intern("package"))).as_table();
  Table* preload = pkg->get(TValue::obj(ValueTag::String, L->intern("preload"))).as_table();
  preload->set(L, TValue::obj(ValueTag::String, L->intern("debug")),
               TValue::obj(ValueTag::Function, closure_new_c(L, open_debug_module)));
}

} // namespace luatier
