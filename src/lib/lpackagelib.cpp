#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "luatier/lua.h"
#include "vm/interpreter.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef LUA_MULTRET
#define LUA_MULTRET (-1)
#endif

namespace luatier {
using namespace lib;

#ifdef _WIN32
static constexpr const char* k_dirsep = "\\";
static constexpr const char* k_config = "\\\n;\n?\n!\n-";
// Approximate PUC Windows defaults (enough to contain "lua" for suite checks).
static constexpr const char* k_path_default =
    "!\\lua\\?.lua;!\\lua\\?\\init.lua;!\\?.lua;!\\?\\init.lua;!.\\?.lua;!.\\?\\init.lua";
static constexpr const char* k_cpath_default =
    "!\\?.dll;!\\..\\lib\\lua\\5.3\\?.dll;!\\loadall.dll;.\\?.dll";
#else
static constexpr const char* k_dirsep = "/";
static constexpr const char* k_config = "/\n;\n?\n!\n-";
// Match PUC luaconf.h Unix defaults (LUA_ROOT=/usr/local/).
static constexpr const char* k_path_default =
    "/usr/local/share/lua/5.3/?.lua;/usr/local/share/lua/5.3/?/init.lua;"
    "/usr/local/lib/lua/5.3/?.lua;/usr/local/lib/lua/5.3/?/init.lua;"
    "./?.lua;./?/init.lua";
static constexpr const char* k_cpath_default =
    "/usr/local/lib/lua/5.3/?.so;/usr/local/lib/lua/5.3/loadall.so;./?.so";
#endif

static Table* package_table(State* L) {
  TValue p = L->globals->get(TValue::obj(ValueTag::String, L->intern("package")));
  if (!p.is_table())
    panic("package table missing");
  return p.as_table();
}

void package_set_loaded(State* L, const char* name, const TValue& mod) {
  Table* pkg = package_table(L);
  Table* loaded = pkg->get(TValue::obj(ValueTag::String, L->intern("loaded"))).as_table();
  if (!loaded)
    panic("package.loaded missing");
  loaded->set(L, TValue::obj(ValueTag::String, L->intern(name)), mod);
}

static bool file_readable(const std::string& filename) {
  if (filename.empty())
    return false;
  FILE* f = std::fopen(filename.c_str(), "r");
  if (!f)
    return false;
  std::fclose(f);
  return true;
}

static std::string gsub_plain(std::string_view s, std::string_view from, std::string_view to) {
  if (from.empty())
    return std::string(s);
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t at = s.find(from, i);
    if (at == std::string_view::npos) {
      out.append(s.substr(i));
      break;
    }
    out.append(s.substr(i, at - i));
    out.append(to);
    i = at + from.size();
  }
  return out;
}

// PUC searchpath: try each `path` template (`;`-separated), replace `?` with name
// (after sep→dirsep), return first readable file or nil + error message.
static int searchpath_impl(State* L, std::string name, std::string_view path,
                           std::string_view sep, std::string_view dirsep) {
  if (!sep.empty())
    name = gsub_plain(name, sep, dirsep);

  std::string err;
  size_t start = 0;
  while (start <= path.size()) {
    while (start < path.size() && path[start] == ';')
      ++start;
    if (start >= path.size())
      break;
    size_t semi = path.find(';', start);
    if (semi == std::string_view::npos)
      semi = path.size();
    std::string_view tmpl = path.substr(start, semi - start);
    start = semi + 1;

    std::string filename = gsub_plain(tmpl, "?", name);
    if (file_readable(filename)) {
      L->settop(0);
      push_string(L, filename);
      return 1;
    }
    err += "\n\tno file '";
    err += filename;
    err += "'";
  }
  L->settop(0);
  L->push(TValue::nil());
  push_string(L, err);
  return 2;
}

static int searcher_preload(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  Table* pkg = package_table(L);
  Table* preload = pkg->get(TValue::obj(ValueTag::String, L->intern("preload"))).as_table();
  TValue f = preload->get(TValue::obj(ValueTag::String, L->intern(name)));
  L->settop(0);
  if (f.is_function()) {
    L->push(f);
    return 1;
  }
  push_string(L, "\n\tno field package.preload['" + name + "']");
  return 1;
}

static int searcher_lua(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  Table* pkg = package_table(L);
  TValue pathv = pkg->get(TValue::obj(ValueTag::String, L->intern("path")));
  if (!pathv.is_string())
    panic("'package.path' must be a string");
  std::string path = std::string(pathv.as_string()->view());

  int n = searchpath_impl(L, name, path, ".", k_dirsep);
  if (n != 1) {
    // stack: nil, err — searchers report a single string message
    TValue msg = L->gettop() >= 2 ? *L->at(2) : TValue::nil();
    L->settop(0);
    L->push(msg);
    return 1;
  }

  // stack: filename
  std::string filename = std::string(L->at(1)->as_string()->view());
  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    L->settop(0);
    push_string(L, "\n\tno file '" + filename + "'");
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string source = ss.str();
  L->settop(0);
  int st = L->load_string(source, "@" + filename);
  if (st != 0) {
    std::string msg = L->gettop() >= 1 ? value_to_string(*L->at(1)) : "load error";
    panic("error loading module '" + name + "' from file '" + filename + "':\n\t" + msg);
  }
  // loader, filename (2nd arg to module)
  push_string(L, filename);
  return 2;
}

static int pkg_require(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  Table* pkg = package_table(L);
  Table* loaded = pkg->get(TValue::obj(ValueTag::String, L->intern("loaded"))).as_table();
  TValue got = loaded->get(TValue::obj(ValueTag::String, L->intern(name)));
  // PUC: only truthy LOADED[name] counts as already loaded (false triggers reload).
  if (got.is_truthy()) {
    L->settop(0);
    L->push(got);
    return 1;
  }

  TValue searchers_v = pkg->get(TValue::obj(ValueTag::String, L->intern("searchers")));
  if (!searchers_v.is_table())
    panic("'package.searchers' must be a table");
  Table* searchers = searchers_v.as_table();

  std::string err;
  TValue loader = TValue::nil();
  TValue loader_data = TValue::nil();
  for (int64_t i = 1;; ++i) {
    TValue sfn = searchers->get_int(i);
    if (sfn.is_nil())
      break;
    L->settop(0);
    L->push(sfn);
    push_string(L, name);
    call_closure(L, sfn.as_closure(), 1, 2);
    TValue a = L->gettop() >= 1 ? *L->at(1) : TValue::nil();
    TValue b = L->gettop() >= 2 ? *L->at(2) : TValue::nil();
    if (a.is_function()) {
      loader = a;
      loader_data = b;
      break;
    }
    if (a.is_string())
      err += a.as_string()->view();
  }
  if (!loader.is_function())
    panic("module '" + name + "' not found:" + err);

  L->settop(0);
  L->push(loader);
  push_string(L, name);
  L->push(loader_data);
  call_closure(L, loader.as_closure(), 2, 1);
  TValue mod = L->gettop() >= 1 ? *L->at(1) : TValue::nil();
  if (!mod.is_nil())
    loaded->set(L, TValue::obj(ValueTag::String, L->intern(name)), mod);
  got = loaded->get(TValue::obj(ValueTag::String, L->intern(name)));
  if (got.is_nil()) {
    got = TValue::boolean(true);
    loaded->set(L, TValue::obj(ValueTag::String, L->intern(name)), got);
  }
  L->settop(0);
  L->push(got);
  return 1;
}

static int pkg_loadlib(State* L) {
  (void)check_string(L, 1);
  (void)check_string(L, 2);
  // Soft suite skips dynamic C modules under `_port`; keep a clear stub.
  L->settop(0);
  L->push(TValue::nil());
  push_string(L, "dynamic libraries not enabled; check your Lua installation");
  push_string(L, "absent");
  return 3;
}

static int pkg_searchpath(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  std::string path = std::string(check_string(L, 2)->view());
  std::string sep = L->gettop() >= 3 ? std::string(check_string(L, 3)->view()) : ".";
  std::string rep = L->gettop() >= 4 ? std::string(check_string(L, 4)->view()) : k_dirsep;
  return searchpath_impl(L, std::move(name), path, sep, rep);
}

static bool package_noenv(State* L) {
  TValue v = L->registry->get(TValue::obj(ValueTag::String, L->intern("LUA_NOENV")));
  return !v.is_nil();
}

// PUC setpath: versioned env first, then unversioned; ";;" → ";" + default + ";".
static std::string expand_path_env(const char* envname, const char* dft, bool noenv) {
  std::string versioned = std::string(envname) + LUA_VERSUFFIX;
  const char* path = std::getenv(versioned.c_str());
  if (path == nullptr)
    path = std::getenv(envname);
  if (path == nullptr || noenv)
    return dft;
  // Replace ";;" with ";\1;" then "\1" with default (AUXMARK technique).
  std::string s = path;
  std::string out;
  out.reserve(s.size() + std::strlen(dft) + 8);
  for (size_t i = 0; i < s.size();) {
    if (i + 1 < s.size() && s[i] == ';' && s[i + 1] == ';') {
      out.push_back(';');
      out.push_back('\1');
      out.push_back(';');
      i += 2;
    } else {
      out.push_back(s[i++]);
    }
  }
  std::string final;
  final.reserve(out.size() + std::strlen(dft));
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '\1')
      final += dft;
    else
      final.push_back(out[i]);
  }
  return final;
}

static void set_package_path_field(State* L, Table* pkg, const char* field, const char* envname,
                                   const char* dft) {
  std::string path = expand_path_env(envname, dft, package_noenv(L));
  set_field_value(L, pkg, field, TValue::obj(ValueTag::String, L->intern(path)));
}

void package_reapply_paths(State* L) {
  Table* pkg = package_table(L);
  set_package_path_field(L, pkg, "path", "LUA_PATH", k_path_default);
  set_package_path_field(L, pkg, "cpath", "LUA_CPATH", k_cpath_default);
}

void open_package_lib(State* L) {
  Table* pkg = new_lib(L, 16);
  Table* loaded = new_lib(L, 16);
  Table* preload = new_lib(L, 8);
  Table* searchers = table_new(L, 4, 0);

  searchers->set_int(L, 1, TValue::obj(ValueTag::Function, closure_new_c(L, searcher_preload)));
  searchers->set_int(L, 2, TValue::obj(ValueTag::Function, closure_new_c(L, searcher_lua)));

  loaded->set(L, TValue::obj(ValueTag::String, L->intern("_G")),
              TValue::obj(ValueTag::Table, L->globals));
  loaded->set(L, TValue::obj(ValueTag::String, L->intern("package")),
              TValue::obj(ValueTag::Table, pkg));
  set_field_value(L, pkg, "loaded", TValue::obj(ValueTag::Table, loaded));
  set_field_value(L, pkg, "preload", TValue::obj(ValueTag::Table, preload));
  set_field_value(L, pkg, "searchers", TValue::obj(ValueTag::Table, searchers));
  set_package_path_field(L, pkg, "path", "LUA_PATH", k_path_default);
  set_package_path_field(L, pkg, "cpath", "LUA_CPATH", k_cpath_default);
  set_field(L, pkg, "require", pkg_require);
  set_field(L, pkg, "loadlib", pkg_loadlib);
  set_field(L, pkg, "searchpath", pkg_searchpath);
  set_field_value(L, pkg, "config", TValue::obj(ValueTag::String, L->intern(k_config)));

  set_global_value(L, "require", TValue::obj(ValueTag::Function, closure_new_c(L, pkg_require)));
  set_global_value(L, "package", TValue::obj(ValueTag::Table, pkg));
}

} // namespace luatier
