#include "luajit3/lua.h"

#include "frontend/parser.hpp"
#include "frontend/sema.hpp"
#include "frontend/lowering.hpp"
#include "tools/dump.hpp"
#include "tools/profile.hpp"
#include "vm/state.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static void usage() {
  std::cout << "luajit3 " << LJ3_VERSION << " — Lua 5.3+ runtime (from-scratch)\n"
            << "Usage:\n"
            << "  luajit3 [options] [script [args]]\n"
            << "  luajit3 -e \"chunk\"\n"
            << "Options:\n"
            << "  --version        print version\n"
            << "  --dump-bc        dump LBC of -e/script before run\n"
            << "  -e <chunk>       execute string\n";
}

int main(int argc, char** argv) {
#ifdef _MSC_VER
  // Avoid assert message-box modal hang; print to stderr instead.
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
  bool dump_bc = false;
  std::string eval;
  std::string file;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--version" || a == "-v") {
      std::cout << "LuaJIT3 " << LJ3_VERSION << " (Lua " << LUA_VERSION_MAJOR << "."
                << LUA_VERSION_MINOR << "+)\n";
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
    if (a == "-e") {
      if (i + 1 >= argc) {
        std::cerr << "missing argument to -e\n";
        return 1;
      }
      eval = argv[++i];
      continue;
    }
    if (a[0] != '-') {
      file = a;
      break;
    }
    std::cerr << "unknown option: " << a << "\n";
    return 1;
  }

  auto* L = luaL_newstate();
  int st = LUA_OK;
  if (!eval.empty()) {
    if (dump_bc) {
      try {
        auto st2 = lj3::new_state();
        auto chunk = lj3::parse(eval, "=(command line)");
        lj3::sema_analyze(*chunk);
        auto* p = lj3::lower_chunk(st2.get(), *chunk, "=(command line)");
        lj3::dump_proto_to_stderr(p);
      } catch (const std::exception& e) {
        std::cerr << "dump failed: " << e.what() << "\n";
      }
    }
    st = luaL_loadstring(L, eval.c_str());
    if (st == LUA_OK)
      st = lua_pcall(L, 0, 0, 0);
  } else if (!file.empty()) {
    st = luaL_loadfile(L, file.c_str());
    if (st == LUA_OK) {
      if (dump_bc) {
        // Keep flag useful for future IR dumps; currently signals verbose run.
        std::cerr << "running " << file << " top=" << lua_gettop(L) << "\n";
      }
      st = lua_pcall(L, 0, 0, 0);
    }
  } else {
    usage();
    lua_close(L);
    return 0;
  }

  if (st != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    std::cerr << (msg ? msg : "error") << "\n";
    lua_close(L);
    return 1;
  }
  lua_close(L);
  return 0;
}
