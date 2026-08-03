// LuaTier stand-alone interpreter — PUC Lua 5.3 lua.c semantics.

#include "lib/lib_util.hpp"
#include "lib/libs.hpp"
#include "luatier/lua.h"
#include "runtime/value.hpp"
#include "vm/debug_hook.hpp"
#include "vm/interpreter.hpp"
#include "vm/ldebug.hpp"
#include "vm/meta.hpp"
#include "vm/state.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define luatier_isatty(fd) _isatty(fd)
#define luatier_fileno(f) _fileno(f)
#else
#include <unistd.h>
#define luatier_isatty(fd) isatty(fd)
#define luatier_fileno(f) fileno(f)
#endif

namespace luatier {
namespace {

using namespace lib;

constexpr const char* kPrompt = "> ";
constexpr const char* kPrompt2 = ">> ";
constexpr int kMaxInput = 512;
constexpr const char* kEofMark = "<eof>";

State* g_state = nullptr;
const char* g_progname = "luatier";

void l_message(const char* pname, const char* msg) {
  if (pname)
    std::fprintf(stderr, "%s: ", pname);
  std::fprintf(stderr, "%s\n", msg);
  std::fflush(stderr);
}

TValue take_err_obj(State* L, const char* fallback) {
  if (L->current->err_obj_set) {
    L->current->err_obj_set = false;
    return L->current->err_obj;
  }
  return TValue::obj(ValueTag::String, L->intern(fallback ? fallback : ""));
}

const char* type_name(const TValue& v) {
  switch (v.tag()) {
  case ValueTag::Nil:
    return "nil";
  case ValueTag::Bool:
    return "boolean";
  case ValueTag::Int:
  case ValueTag::Float:
    return "number";
  case ValueTag::String:
    return "string";
  case ValueTag::Table:
    return "table";
  case ValueTag::Function:
    return "function";
  case ValueTag::Userdata:
    return "userdata";
  case ValueTag::Thread:
    return "thread";
  case ValueTag::LightUserdata:
    return "userdata";
  default:
    return "no value";
  }
}

int report(State* L, int status) {
  if (status != LUA_OK && L->gettop() >= 1) {
    std::string msg = value_to_string(*L->at(L->gettop()));
    l_message(g_progname, msg.c_str());
    L->pop();
  }
  return status;
}

// Message handler: __tostring / typename / traceback (PUC msghandler).
void apply_msghandler(State* L, TValue obj) {
  std::string msg;
  if (obj.is_string()) {
    msg = std::string(obj.as_string()->view());
  } else {
    TValue mm = get_metamethod(L, obj, "__tostring");
    if (mm.is_function()) {
      L->settop(0);
      L->push(mm);
      L->push(obj);
      try {
        call_closure(L, mm.as_closure(), 1, 1);
        if (L->gettop() >= 1 && L->at(1)->is_string())
          msg = std::string(L->at(1)->as_string()->view());
      } catch (const LuatierError&) {
        L->current->err_obj_set = false;
      }
    }
    if (msg.empty())
      msg = std::string("(error object is a ") + type_name(obj) + " value)";
  }

  // Append traceback via debug.traceback when available.
  TValue dbg = L->globals->get(TValue::obj(ValueTag::String, L->intern("debug")));
  if (dbg.is_table()) {
    TValue tb = dbg.as_table()->get(TValue::obj(ValueTag::String, L->intern("traceback")));
    if (tb.is_function()) {
      L->settop(0);
      L->push(tb);
      push_string(L, msg);
      L->push(TValue::integer(2));
      try {
        call_closure(L, tb.as_closure(), 2, 1);
        if (L->gettop() >= 1 && L->at(1)->is_string()) {
          L->push(*L->at(1));
          return;
        }
      } catch (const LuatierError&) {
        L->current->err_obj_set = false;
      }
    }
  }
  L->settop(0);
  push_string(L, msg);
}

static int interrupt_hook(State* L) {
  debug_sethook_thread(L->current, nullptr, 0, 0);
  L->current->err_obj = TValue::obj(ValueTag::String, L->intern("interrupted!"));
  L->current->err_obj_set = true;
  panic("interrupted!");
}

static void laction(int) {
  std::signal(SIGINT, SIG_DFL);
  if (!g_state)
    return;
  Closure* hook = closure_new_c(g_state, interrupt_hook);
  hook->cname = "interrupt_hook";
  debug_sethook_thread(g_state->current, hook,
                       DEBUG_HOOK_CALL | DEBUG_HOOK_RET | DEBUG_HOOK_COUNT, 1);
}

int docall(State* L, int narg, int nres) {
  int func_idx = L->gettop() - narg;
  if (func_idx < 1 || !L->at(func_idx)->is_function()) {
    L->settop(0);
    push_string(L, "attempt to call a non-function value");
    return LUA_ERRRUN;
  }
  TValue func = *L->at(func_idx);
  std::vector<TValue> args(static_cast<size_t>(narg));
  for (int i = 0; i < narg; ++i)
    args[static_cast<size_t>(i)] = *L->at(func_idx + 1 + i);

  const int protect_frames = static_cast<int>(L->current->frames.size());
  g_state = L;
  std::signal(SIGINT, laction);
  try {
    L->settop(0);
    L->push(func);
    for (auto& a : args)
      L->push(a);
    int st = call_closure(L, func.as_closure(), narg, nres);
    std::signal(SIGINT, SIG_DFL);
    return st;
  } catch (const LuatierError& e) {
    std::signal(SIGINT, SIG_DFL);
    while (static_cast<int>(L->current->frames.size()) > protect_frames) {
      L->close_upvals(L->current, L->current->frames.back().base);
      L->current->frames.pop_back();
    }
    TValue emsg = take_err_obj(L, e.what());
    apply_msghandler(L, emsg);
    return LUA_ERRRUN;
  }
}

void print_usage(const char* badoption) {
  std::fprintf(stderr, "%s: ", g_progname);
  if (badoption[1] == 'e' || badoption[1] == 'l')
    std::fprintf(stderr, "'%s' needs argument\n", badoption);
  else
    std::fprintf(stderr, "unrecognized option '%s'\n", badoption);
  std::fprintf(stderr,
               "usage: %s [options] [script [args]]\n"
               "Available options are:\n"
               "  -e stat  execute string 'stat'\n"
               "  -i       enter interactive mode after executing 'script'\n"
               "  -l name  require library 'name'\n"
               "  -v       show version information\n"
               "  -E       ignore environment variables\n"
               "  --       stop handling options\n"
               "  -        stop handling options and execute stdin\n",
               g_progname);
}

void print_version() {
  std::fwrite(LUA_COPYRIGHT, sizeof(char), std::strlen(LUA_COPYRIGHT), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void create_arg_table(State* L, char** argv, int argc, int script) {
  if (script == argc)
    script = 0;
  Table* t = table_new(L, argc, 0);
  for (int i = 0; i < argc; ++i)
    t->set_int(L, i - script, TValue::obj(ValueTag::String, L->intern(argv[i] ? argv[i] : "")));
  set_global_value(L, "arg", TValue::obj(ValueTag::Table, t));
}

int dochunk(State* L, int status) {
  if (status == LUA_OK)
    status = docall(L, 0, 0);
  return report(L, status);
}

int load_buffer(State* L, const char* s, size_t len, const char* name) {
  return L->load_string(std::string(s, len), name ? name : "=(load)");
}

int load_file(State* L, const char* filename) {
  std::string source;
  std::string chunk_name;
  if (filename == nullptr) {
    chunk_name = "=stdin";
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    source = ss.str();
  } else {
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
      push_string(L, std::string("cannot open ") + filename + ": No such file or directory");
      return LUA_ERRFILE;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    source = ss.str();
    chunk_name = std::string("@") + filename;
  }
  if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
      static_cast<unsigned char>(source[1]) == 0xBB &&
      static_cast<unsigned char>(source[2]) == 0xBF)
    source.erase(0, 3);
  return L->load_string(source, chunk_name);
}

int dofile(State* L, const char* name) { return dochunk(L, load_file(L, name)); }

int dostring(State* L, const char* s, const char* name) {
  return dochunk(L, load_buffer(L, s, std::strlen(s), name));
}

int dolibrary(State* L, const char* name) {
  TValue req = L->globals->get(TValue::obj(ValueTag::String, L->intern("require")));
  L->settop(0);
  L->push(req);
  push_string(L, name);
  int status = docall(L, 1, 1);
  if (status == LUA_OK && L->gettop() >= 1)
    set_global_value(L, name, *L->at(1));
  return report(L, status);
}

const char* get_prompt(State* L, bool firstline) {
  TValue p = L->globals->get(
      TValue::obj(ValueTag::String, L->intern(firstline ? "_PROMPT" : "_PROMPT2")));
  if (p.is_string())
    return p.as_string()->data;
  return firstline ? kPrompt : kPrompt2;
}

bool incomplete(State* L, int status) {
  if (status != LUA_ERRSYNTAX || L->gettop() < 1 || !L->at(L->gettop())->is_string())
    return false;
  std::string_view msg = L->at(L->gettop())->as_string()->view();
  const size_t marklen = std::strlen(kEofMark);
  if (msg.size() >= marklen && msg.substr(msg.size() - marklen) == kEofMark) {
    L->pop();
    return true;
  }
  return false;
}

bool pushline(State* L, bool firstline) {
  char buffer[kMaxInput];
  const char* prmt = get_prompt(L, firstline);
  std::fputs(prmt, stdout);
  std::fflush(stdout);
  if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr)
    return false;
  size_t l = std::strlen(buffer);
  if (l > 0 && buffer[l - 1] == '\n')
    buffer[--l] = '\0';
  if (firstline && buffer[0] == '=') {
    std::string ret = std::string("return ") + (buffer + 1);
    push_string(L, ret);
  } else {
    push_string(L, std::string_view(buffer, l));
  }
  return true;
}

int addreturn(State* L) {
  std::string line = std::string(L->at(L->gettop())->as_string()->view());
  std::string retline = "return " + line + ";";
  int status = load_buffer(L, retline.c_str(), retline.size(), "=stdin");
  if (status == LUA_OK) {
    TValue fn = *L->at(L->gettop());
    L->settop(0);
    L->push(fn);
  } else {
    L->pop(); // drop load error; leave original line
  }
  return status;
}

int multiline(State* L) {
  for (;;) {
    std::string line = std::string(L->at(1)->as_string()->view());
    int status = load_buffer(L, line.c_str(), line.size(), "=stdin");
    if (!incomplete(L, status) || !pushline(L, false)) {
      if (L->gettop() >= 2) {
        TValue top = *L->at(L->gettop());
        L->settop(0);
        L->push(top);
      }
      return status;
    }
    std::string cont = std::string(L->at(2)->as_string()->view());
    L->settop(0);
    push_string(L, line + "\n" + cont);
  }
}

int loadline(State* L) {
  L->settop(0);
  if (!pushline(L, true))
    return -1;
  int status = addreturn(L);
  if (status != LUA_OK)
    status = multiline(L);
  return status;
}

void l_print(State* L) {
  int n = L->gettop();
  if (n <= 0)
    return;
  TValue pr = L->globals->get(TValue::obj(ValueTag::String, L->intern("print")));
  if (!pr.is_function())
    return;
  std::vector<TValue> results(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    results[static_cast<size_t>(i)] = *L->at(i + 1);
  L->settop(0);
  L->push(pr);
  for (auto& r : results)
    L->push(r);
  try {
    call_closure(L, pr.as_closure(), n, 0);
  } catch (const LuatierError& e) {
    TValue emsg = take_err_obj(L, e.what());
    std::string m = "error calling 'print' (" + value_to_string(emsg) + ")";
    l_message(g_progname, m.c_str());
    L->settop(0);
  }
}

void do_repl(State* L) {
  const char* old = g_progname;
  g_progname = nullptr;
  int status;
  while ((status = loadline(L)) != -1) {
    if (status == LUA_OK)
      status = docall(L, 0, LUA_MULTRET);
    if (status == LUA_OK)
      l_print(L);
    else
      report(L, status);
  }
  L->settop(0);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  g_progname = old;
}

int pushargs(State* L) {
  TValue arg = L->globals->get(TValue::obj(ValueTag::String, L->intern("arg")));
  if (!arg.is_table())
    panic("'arg' is not a table");
  int64_t n = table_length(arg.as_table());
  for (int64_t i = 1; i <= n; ++i)
    L->push(arg.as_table()->get_int(i));
  return static_cast<int>(n);
}

int handle_script(State* L, char** argv) {
  const char* fname = argv[0];
  if (std::strcmp(fname, "-") == 0 && std::strcmp(argv[-1], "--") != 0)
    fname = nullptr;
  int status = load_file(L, fname);
  if (status == LUA_OK) {
    int n = pushargs(L);
    status = docall(L, n, LUA_MULTRET);
  }
  return report(L, status);
}

enum {
  has_error = 1,
  has_i = 2,
  has_v = 4,
  has_e = 8,
  has_E = 16,
};

int collectargs(char** argv, int* first) {
  int args = 0;
  int i;
  for (i = 1; argv[i] != nullptr; ++i) {
    *first = i;
    if (argv[i][0] != '-')
      return args;
    switch (argv[i][1]) {
    case '-':
      if (argv[i][2] != '\0')
        return has_error;
      *first = i + 1;
      return args;
    case '\0':
      return args;
    case 'E':
      if (argv[i][2] != '\0')
        return has_error;
      args |= has_E;
      break;
    case 'i':
      args |= has_i;
      // fallthrough
    case 'v':
      if (argv[i][2] != '\0')
        return has_error;
      args |= has_v;
      break;
    case 'e':
      args |= has_e;
      // fallthrough
    case 'l':
      if (argv[i][2] == '\0') {
        ++i;
        if (argv[i] == nullptr || argv[i][0] == '-')
          return has_error;
      }
      break;
    default:
      return has_error;
    }
  }
  *first = i;
  return args;
}

int runargs(State* L, char** argv, int n) {
  for (int i = 1; i < n; ++i) {
    int option = argv[i][1];
    if (option == 'e' || option == 'l') {
      const char* extra = argv[i] + 2;
      if (*extra == '\0')
        extra = argv[++i];
      int status = (option == 'e') ? dostring(L, extra, "=(command line)") : dolibrary(L, extra);
      if (status != LUA_OK)
        return 0;
    }
  }
  return 1;
}

int handle_luainit(State* L) {
  const char* name = "=LUA_INIT" LUA_VERSUFFIX;
  const char* init = std::getenv(name + 1);
  if (init == nullptr) {
    name = "=LUA_INIT";
    init = std::getenv("LUA_INIT");
  }
  if (init == nullptr)
    return LUA_OK;
  if (init[0] == '@')
    return dofile(L, init + 1);
  return dostring(L, init, name);
}

int pmain(State* L, int argc, char** argv) {
  int script = 0;
  int args = collectargs(argv, &script);
  if (argv[0] && argv[0][0])
    g_progname = argv[0];
  if (args == has_error) {
    print_usage(argv[script]);
    return 0;
  }
  if (args & has_v)
    print_version();
  if (args & has_E) {
    L->registry->set(L, TValue::obj(ValueTag::String, L->intern("LUA_NOENV")),
                     TValue::boolean(true));
    package_reapply_paths(L);
  }
  create_arg_table(L, argv, argc, script);
  if (!(args & has_E)) {
    if (handle_luainit(L) != LUA_OK)
      return 0;
  }
  if (!runargs(L, argv, script))
    return 0;
  if (script < argc && handle_script(L, argv + script) != LUA_OK)
    return 0;
  if (args & has_i)
    do_repl(L);
  else if (script == argc && !(args & (has_e | has_v))) {
    if (luatier_isatty(luatier_fileno(stdin))) {
      print_version();
      do_repl(L);
    } else {
      dofile(L, nullptr);
    }
  }
  return 1;
}

} // namespace

int standalone_main(int argc, char** argv) {
  auto holder = new_state();
  State* L = holder.get();
  g_state = L;
  int ok = 0;
  try {
    ok = pmain(L, argc, argv);
  } catch (const LuatierError& e) {
    l_message(g_progname, e.what());
    ok = 0;
  }
  g_state = nullptr;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace luatier
