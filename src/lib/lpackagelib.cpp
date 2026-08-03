#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "vm/interpreter.hpp"

#include <fstream>
#include <sstream>

#ifndef LUA_MULTRET
#define LUA_MULTRET (-1)
#endif

namespace lj3 {
using namespace lib;

static Table* package_table(State* L) {
  TValue p = L->globals->get(TValue::obj(ValueTag::String, L->intern("package")));
  if (!p.is_table())
    panic("package table missing");
  return p.as_table();
}

static int searcher_preload(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  Table* pkg = package_table(L);
  Table* preload = pkg->get(TValue::obj(ValueTag::String, L->intern("preload"))).as_table();
  TValue f = preload->get(TValue::obj(ValueTag::String, L->intern(name)));
  L->settop(0);
  if (f.is_function())
    L->push(f);
  else
    push_string(L, "no field package.preload['" + name + "']");
  return 1;
}

static std::string path_template(std::string_view path, std::string_view name) {
  std::string result;
  size_t start = 0;
  while (start < path.size()) {
    size_t semi = path.find(';', start);
    if (semi == std::string_view::npos)
      semi = path.size();
    std::string tmpl(path.substr(start, semi - start));
    std::string s;
    for (size_t i = 0; i < tmpl.size(); ++i) {
      if (tmpl[i] == '?')
        s.append(name);
      else
        s.push_back(tmpl[i]);
    }
    if (!result.empty())
      result += ';';
    result += s;
    start = semi + 1;
  }
  return result;
}

static std::string modname_to_path(std::string name) {
  for (char& c : name) {
    if (c == '.')
#ifdef _WIN32
      c = '\\';
#else
      c = '/';
#endif
  }
  return name;
}

static int searcher_lua(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  Table* pkg = package_table(L);
  TValue pathv = pkg->get(TValue::obj(ValueTag::String, L->intern("path")));
  std::string path = pathv.is_string() ? std::string(pathv.as_string()->view()) : "?.lua;./?.lua";
  std::string tried;
  std::string expanded = path_template(path, modname_to_path(name));
  size_t start = 0;
  while (start < expanded.size()) {
    size_t semi = expanded.find(';', start);
    if (semi == std::string::npos)
      semi = expanded.size();
    std::string filename(expanded.substr(start, semi - start));
    start = semi + 1;
    if (filename.empty())
      continue;
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
      if (!tried.empty())
        tried += "\n\t";
      tried += "no file '" + filename + "'";
      continue;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string source = ss.str();
    L->settop(0);
    int st = L->load_string(source, "@" + filename);
    if (st == 0)
      return 1;
    push_string(L, value_to_string(*L->at(1)));
    return 1;
  }
  L->settop(0);
  push_string(L, tried.empty() ? "module '" + name + "' not found" : tried);
  return 1;
}

static int pkg_require(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  Table* pkg = package_table(L);
  Table* loaded = pkg->get(TValue::obj(ValueTag::String, L->intern("loaded"))).as_table();
  TValue got = loaded->get(TValue::obj(ValueTag::String, L->intern(name)));
  if (!got.is_nil()) {
    L->settop(0);
    L->push(got);
    return 1;
  }

  Table* searchers = pkg->get(TValue::obj(ValueTag::String, L->intern("searchers"))).as_table();
  if (!searchers)
    panic("package.searchers missing");

  std::string err;
  for (int64_t i = 1;; ++i) {
    TValue sfn = searchers->get_int(i);
    if (sfn.is_nil())
      break;
    L->settop(0);
    L->push(sfn);
    push_string(L, name);
    call_closure(L, sfn.as_closure(), 1, 2);
    TValue loader = L->gettop() >= 1 ? *L->at(1) : TValue::nil();
    if (loader.is_function()) {
      L->settop(0);
      L->push(loader);
      push_string(L, name);
      call_closure(L, loader.as_closure(), 1, 1);
      TValue mod = L->gettop() >= 1 ? *L->at(1) : TValue::boolean(true);
      if (mod.is_nil())
        mod = TValue::boolean(true);
      loaded->set(L, TValue::obj(ValueTag::String, L->intern(name)), mod);
      L->settop(0);
      L->push(mod);
      return 1;
    }
    if (loader.is_string()) {
      if (!err.empty())
        err += "\n";
      err += loader.as_string()->view();
    }
  }
  panic("module '" + name + "' not found:" + (err.empty() ? "" : "\n" + err));
}

static int pkg_loadlib(State* L) {
  (void)check_string(L, 1);
  (void)check_string(L, 2);
  panic("package.loadlib is not implemented (dynamic C modules require native loader support)");
}

static int pkg_searchpath(State* L) {
  std::string name = std::string(check_string(L, 1)->view());
  std::string path = std::string(check_string(L, 2)->view());
  std::string sep = L->gettop() >= 3 ? std::string(check_string(L, 3)->view()) : ".";
  std::string rep = L->gettop() >= 4 ? std::string(check_string(L, 4)->view()) : "/";
  std::string mod = name;
  size_t pos = 0;
  while ((pos = mod.find(sep, pos)) != std::string::npos) {
    mod.replace(pos, sep.size(), rep);
    pos += rep.size();
  }
  std::string expanded = path_template(path, mod);
  L->settop(0);
  push_string(L, expanded);
  return 1;
}

void open_package_lib(State* L) {
  Table* pkg = new_lib(L, 16);
  Table* loaded = new_lib(L, 8);
  Table* preload = new_lib(L, 8);
  Table* searchers = table_new(L, 4, 0);

  searchers->set_int(L, 1, TValue::obj(ValueTag::Function, closure_new_c(L, searcher_preload)));
  searchers->set_int(L, 2, TValue::obj(ValueTag::Function, closure_new_c(L, searcher_lua)));

  // PUC: package.loaded._G / package so traceback can name globals (pcall, etc.).
  loaded->set(L, TValue::obj(ValueTag::String, L->intern("_G")),
              TValue::obj(ValueTag::Table, L->globals));
  loaded->set(L, TValue::obj(ValueTag::String, L->intern("package")),
              TValue::obj(ValueTag::Table, pkg));
  set_field_value(L, pkg, "loaded", TValue::obj(ValueTag::Table, loaded));
  set_field_value(L, pkg, "preload", TValue::obj(ValueTag::Table, preload));
  set_field_value(L, pkg, "searchers", TValue::obj(ValueTag::Table, searchers));
  set_field_value(L, pkg, "path",
                  TValue::obj(ValueTag::String, L->intern("./?.lua;./?/init.lua;?.lua;?/init.lua")));
#ifdef _WIN32
  set_field_value(L, pkg, "cpath",
                  TValue::obj(ValueTag::String, L->intern("./?.dll;./?/init.dll")));
#else
  set_field_value(L, pkg, "cpath",
                  TValue::obj(ValueTag::String, L->intern("./?.so;./?/init.so")));
#endif
  set_field(L, pkg, "require", pkg_require);
  set_field(L, pkg, "loadlib", pkg_loadlib);
  set_field(L, pkg, "searchpath", pkg_searchpath);
  set_field_value(L, pkg, "config", TValue::obj(ValueTag::String, L->intern("/;\n?\n!\n-")));

  set_global_value(L, "require", TValue::obj(ValueTag::Function, closure_new_c(L, pkg_require)));
  set_global_value(L, "package", TValue::obj(ValueTag::Table, pkg));
}

} // namespace lj3
