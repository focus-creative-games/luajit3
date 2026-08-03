#include "luajit3/lua.h"
#include "luajit3/lauxlib.h"

#include "frontend/lowering.hpp"
#include "frontend/parser.hpp"
#include "frontend/sema.hpp"
#include "tools/dump.hpp"
#include "vm/state.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static void usage() {
  std::cout << "luajit3 " << LJ3_VERSION << " — Lua 5.3+ runtime (from-scratch)\n"
            << "Usage:\n"
            << "  luajit3 [options] [script [args]]\n"
            << "Options:\n"
            << "  -e <chunk>   execute string (repeatable)\n"
            << "  -l <mod>     require library\n"
            << "  -i           enter interactive mode after script (stub: no-op)\n"
            << "  -v           print version\n"
            << "  --version    print version\n"
            << "  --dump-bc    dump LBC for -e chunks\n"
            << "  --           stop handling options\n";
}

static int report(lua_State* L, int status) {
  if (status != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    std::cerr << (msg ? msg : "error") << "\n";
    lua_pop(L, 1);
  }
  return status;
}

static void create_arg_table(lua_State* L, int argc, char** argv, int script) {
  lua_newtable(L);
  // arg[0] = script name (or "")
  if (script > 0)
    lua_pushstring(L, argv[script]);
  else
    lua_pushstring(L, "");
  lua_rawseti(L, -2, 0);
  // positive: args after script; negative: args before script
  int narg = 0;
  if (script > 0) {
    for (int i = script + 1; i < argc; ++i) {
      lua_pushstring(L, argv[i]);
      lua_rawseti(L, -2, ++narg);
    }
    for (int i = 1; i < script; ++i) {
      lua_pushstring(L, argv[i]);
      lua_rawseti(L, -2, i - script);
    }
  } else {
    for (int i = 1; i < argc; ++i) {
      lua_pushstring(L, argv[i]);
      lua_rawseti(L, -2, i);
    }
  }
  lua_setglobal(L, "arg");
}

static int dochunk(lua_State* L, int status) {
  if (status == LUA_OK)
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
  return report(L, status);
}

static int dolibrary(lua_State* L, const char* name) {
  lua_getglobal(L, "require");
  lua_pushstring(L, name);
  return report(L, lua_pcall(L, 1, 0, 0));
}

int main(int argc, char** argv) {
#ifdef _MSC_VER
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif

  bool dump_bc = false;
  bool has_exec = false;
  int script = 0;
  std::vector<std::pair<char, std::string>> early; // 'e' or 'l'

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--") {
      if (i + 1 < argc)
        script = i + 1;
      break;
    }
    if (a == "--version" || a == "-v") {
      std::cout << "LuaJIT3 " << LJ3_VERSION << " (" << LUA_VERSION << "."
                << LUA_VERSION_RELEASE << ")\n";
      return 0;
    }
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    if (a == "--dump-bc") {
      dump_bc = true;
      continue;
    }
    if (a == "-i")
      continue;
    if (a == "-e" || (a.size() >= 2 && a[0] == '-' && a[1] == 'e')) {
      std::string chunk;
      if (a == "-e") {
        if (i + 1 >= argc) {
          std::cerr << "missing argument to -e\n";
          return 1;
        }
        chunk = argv[++i];
      } else {
        chunk = a.substr(2); // -eCHUNK
      }
      early.emplace_back('e', chunk);
      has_exec = true;
      continue;
    }
    if (a == "-l" || (a.size() >= 2 && a[0] == '-' && a[1] == 'l')) {
      std::string mod;
      if (a == "-l") {
        if (i + 1 >= argc) {
          std::cerr << "missing argument to -l\n";
          return 1;
        }
        mod = argv[++i];
      } else {
        mod = a.substr(2);
      }
      early.emplace_back('l', mod);
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      std::cerr << "unknown option: " << a << "\n";
      return 1;
    }
    script = i;
    break;
  }

  if (!has_exec && script == 0) {
    usage();
    return 0;
  }

  lua_State* L = luaL_newstate();
  luaL_openlibs(L); // already opened in State ctor; safe no-op if idempotent
  create_arg_table(L, argc, argv, script);

  int status = LUA_OK;
  for (auto& op : early) {
    if (op.first == 'l') {
      status = dolibrary(L, op.second.c_str());
      if (status != LUA_OK)
        break;
    } else {
      if (dump_bc) {
        try {
          auto st2 = lj3::new_state();
          auto chunk = lj3::parse(op.second, "=(command line)");
          lj3::sema_analyze(*chunk);
          auto* p = lj3::lower_chunk(st2.get(), *chunk, "=(command line)");
          lj3::dump_proto_to_stderr(p);
        } catch (const std::exception& e) {
          std::cerr << "dump failed: " << e.what() << "\n";
        }
      }
      if (!dump_bc) {
        status = dochunk(L, luaL_loadstring(L, op.second.c_str()));
        if (status != LUA_OK)
          break;
      }
    }
  }

  if (status == LUA_OK && script > 0) {
    if (dump_bc) {
      try {
        std::ifstream in(argv[script]);
        std::ostringstream ss;
        ss << in.rdbuf();
        auto st2 = lj3::new_state();
        auto chunk = lj3::parse(ss.str(), argv[script]);
        lj3::sema_analyze(*chunk);
        auto* p = lj3::lower_chunk(st2.get(), *chunk, argv[script]);
        lj3::dump_proto_to_stderr(p);
      } catch (const std::exception& e) {
        std::cerr << "dump failed: " << e.what() << "\n";
      }
    }
    // --dump-bc already printed; skip running the chunk (avoids hanging on
    // scripts whose only purpose is inspection).
    if (!dump_bc)
      status = dochunk(L, luaL_loadfile(L, argv[script]));
  }

  int code = (status == LUA_OK) ? 0 : 1;
  lua_close(L);
  return code;
}
