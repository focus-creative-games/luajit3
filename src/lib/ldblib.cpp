#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "vm/interpreter.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

namespace lj3 {
using namespace lib;

namespace {

constexpr int kIdSize = 60;
constexpr const char* kDefaultWhat = "Slnuft";
constexpr const char* kValidWhat = "nSlutTfL";

std::string chunkid(std::string_view source) {
  // Match luaO_chunkid (Lua 5.3): bufflen includes trailing NUL conceptually.
  std::string out;
  out.reserve(static_cast<size_t>(kIdSize));

  if (!source.empty() && source[0] == '=') {
    size_t len = source.size() - 1;
    if (len <= static_cast<size_t>(kIdSize))
      out.assign(source.substr(1));
    else
      out.assign(source.substr(1, static_cast<size_t>(kIdSize)));
    return out;
  }

  if (!source.empty() && source[0] == '@') {
    size_t len = source.size() - 1;
    if (len <= static_cast<size_t>(kIdSize)) {
      out.assign(source.substr(1));
    } else {
      out.append("...");
      out.append(source.substr(source.size() - static_cast<size_t>(kIdSize - 3)));
    }
    return out;
  }

  // String source (including empty chunkname "") → [string "..."]
  out.append("[string \"");
  size_t budget = static_cast<size_t>(kIdSize) - 11; // "[string \"\"]" + room
  size_t len = source.size();
  size_t nl = source.find('\n');
  if (nl != std::string_view::npos)
    len = nl;
  if (len < budget && nl == std::string_view::npos) {
    out.append(source.substr(0, len));
  } else {
    if (len > budget)
      len = budget;
    if (len > 0)
      out.append(source.substr(0, len));
    out.append("...");
  }
  out.append("\"]");
  return out;
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

bool resolve_target(State* L, int arg, DebugTarget* out) {
  Thread* th = L->current;
  if (L->at(arg)->is_thread()) {
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

  int idx = level - 1;
  if (idx < 0 || idx >= static_cast<int>(th->frames.size()))
    return false;

  CallFrame& fr = th->frames[static_cast<size_t>(th->frames.size() - 1 - static_cast<size_t>(idx))];
  out->frame = &fr;
  out->cl = fr.cl;
  out->proto = fr.proto;
  out->is_c = out->cl && out->cl->is_c && !out->proto;
  return true;
}

void set_field_str(State* L, Table* t, const char* name, std::string_view value) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)),
         TValue::obj(ValueTag::String, L->intern(value)));
}

void set_field_int(State* L, Table* t, const char* name, int64_t value) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(name)), TValue::integer(value));
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
      set_field_str(L, info, "what", "Lua");
      set_field_int(L, info, "linedefined", target.proto->linedefined);
      set_field_int(L, info, "lastlinedefined", target.proto->lastlinedefined);
    }
  }

  if (what_has(what, 'l')) {
    int currentline = -1;
    if (target.frame && target.proto && target.frame->saved_pc >= 0 &&
        target.frame->saved_pc < static_cast<int>(target.proto->lineinfo.size()))
      currentline = target.proto->lineinfo[static_cast<size_t>(target.frame->saved_pc)];
    set_field_int(L, info, "currentline", currentline);
  }

  if (what_has(what, 'u')) {
    int nups = target.cl ? static_cast<int>(target.cl->upvals.size()) : 0;
    set_field_int(L, info, "nups", nups);
  }

  if (what_has(what, 't') || what_has(what, 'T'))
    info->set(L, TValue::obj(ValueTag::String, L->intern("istailcall")), TValue::boolean(false));

  if (what_has(what, 'n')) {
    info->set(L, TValue::obj(ValueTag::String, L->intern("name")), TValue::nil());
    info->set(L, TValue::obj(ValueTag::String, L->intern("namewhat")), TValue::nil());
  }

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
  int func_arg = 0;
  if (L->gettop() >= 1 && L->at(1)->is_thread()) {
    arg = 2;
    func_arg = 2;
  } else {
    func_arg = 1;
  }

  int what_arg = arg + 1;
  std::string_view what = opt_string(L, what_arg, kDefaultWhat);
  validate_what(what);

  DebugTarget target;
  if (!resolve_target(L, arg, &target)) {
    L->push(TValue::nil());
    return 1;
  }

  Table* info = build_info(L, target, what, func_arg);
  L->push(TValue::obj(ValueTag::Table, info));
  return 1;
}

static int debug_traceback(State* L) {
  std::string msg = L->gettop() >= 1 ? value_to_string(*L->at(1)) : "";
  int level = L->gettop() >= 2 ? static_cast<int>(L->at(2)->to_number()) : 1;
  std::string tb = msg;
  for (int i = level; i <= static_cast<int>(L->current->frames.size()); ++i) {
    tb += "\n\t[C]: in ?";
    (void)i;
  }
  L->settop(0);
  push_string(L, tb);
  return 1;
}

static int debug_getlocal(State* L) {
  (void)L;
  L->settop(0);
  L->push(TValue::nil());
  return 1;
}

static int debug_setlocal(State* L) {
  (void)L;
  return 0;
}

static int debug_getupvalue(State* L) {
  if (!L->at(1)->is_function())
    panic("function expected");
  L->settop(0);
  L->push(TValue::nil());
  return 1;
}

static int debug_setupvalue(State* L) {
  (void)L;
  return 0;
}

static int debug_sethook(State* L) {
  (void)L;
  return 0;
}

static int debug_gethook(State* L) {
  (void)L;
  L->push(TValue::nil());
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
  set_field(L, dbg, "sethook", debug_sethook);
  set_field(L, dbg, "gethook", debug_gethook);
  L->settop(0);
  L->push(TValue::obj(ValueTag::Table, dbg));
  return 1;
}

void open_debug_lib(State* L) {
  Table* pkg = L->globals->get(TValue::obj(ValueTag::String, L->intern("package"))).as_table();
  Table* preload = pkg->get(TValue::obj(ValueTag::String, L->intern("preload"))).as_table();
  preload->set(L, TValue::obj(ValueTag::String, L->intern("debug")),
               TValue::obj(ValueTag::Function, closure_new_c(L, open_debug_module)));
}

} // namespace lj3
