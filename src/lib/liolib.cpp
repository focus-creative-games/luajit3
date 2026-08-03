#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "runtime/userdata.hpp"

#include <cstdio>
#include <cstring>

namespace luatier {
using namespace lib;

struct LFile {
  FILE* fp = nullptr;
  bool close_on_close = true;
  bool is_std = false;
};

static const char* kFileMeta = "LUATIER_FILE*";

static Table* file_metatable(State* L) {
  TValue mt = L->registry->get(TValue::obj(ValueTag::String, L->intern(kFileMeta)));
  if (!mt.is_table()) {
    Table* t = new_lib(L, 8);
    L->registry->set(L, TValue::obj(ValueTag::String, L->intern(kFileMeta)),
                     TValue::obj(ValueTag::Table, t));
    return t;
  }
  return mt.as_table();
}

static LFile* check_file(State* L, int idx) {
  if (!L->at(idx)->is_userdata())
    panic("file expected");
  return reinterpret_cast<LFile*>(userdata_data(L->at(idx)->as_userdata()));
}

static Userdata* new_file_ud(State* L, FILE* fp, bool close_on_close, bool is_std) {
  Table* mt = file_metatable(L);
  Userdata* u = userdata_new(L, sizeof(LFile), mt);
  auto* lf = reinterpret_cast<LFile*>(userdata_data(u));
  lf->fp = fp;
  lf->close_on_close = close_on_close;
  lf->is_std = is_std;
  return u;
}

static int file_read(State* L) {
  LFile* f = check_file(L, 1);
  if (!f->fp)
    panic("attempt to use a closed file");
  std::string mode = L->gettop() >= 2 ? std::string(check_string(L, 2)->view()) : "l";
  L->settop(0);
  if (mode == "a") {
    std::string rest;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f->fp))
      rest += buf;
    push_string(L, rest);
    return 1;
  }
  if (mode == "l" || mode.empty()) {
    char buf[8192];
    if (!std::fgets(buf, sizeof(buf), f->fp)) {
      if (std::feof(f->fp))
        L->push(TValue::nil());
      else
        push_string(L, "");
      return 1;
    }
    std::string line(buf);
    if (!line.empty() && line.back() == '\n')
      line.pop_back();
    push_string(L, line);
    return 1;
  }
  if (!mode.empty() && mode[0] == '*') {
    int64_t n = std::stoll(mode.substr(1));
    std::string data(static_cast<size_t>(n), '\0');
    size_t got = std::fread(data.data(), 1, static_cast<size_t>(n), f->fp);
    data.resize(got);
    if (got == 0)
      L->push(TValue::nil());
    else
      push_string(L, data);
    return 1;
  }
  panic("invalid format");
}

static int file_write(State* L) {
  LFile* f = check_file(L, 1);
  if (!f->fp)
    panic("attempt to use a closed file");
  for (int i = 2; i <= L->gettop(); ++i) {
    std::string s = value_to_string(*L->at(i));
    std::fwrite(s.data(), 1, s.size(), f->fp);
  }
  Userdata* u = L->at(1)->as_userdata();
  L->settop(0);
  L->push(TValue::obj(ValueTag::Userdata, u));
  return 1;
}

static int file_close(State* L) {
  LFile* f = check_file(L, 1);
  if (f->fp && f->close_on_close && !f->is_std)
    std::fclose(f->fp);
  f->fp = nullptr;
  L->settop(0);
  L->push(TValue::boolean(true));
  return 1;
}

static int file_flush(State* L) {
  LFile* f = check_file(L, 1);
  if (f->fp)
    std::fflush(f->fp);
  return 0;
}

static int file_seek(State* L) {
  LFile* f = check_file(L, 1);
  if (!f->fp)
    panic("attempt to use a closed file");
  const char* whence = L->gettop() >= 2 ? check_string(L, 2)->view().data() : "cur";
  int64_t offset = L->gettop() >= 3 ? check_int(L, 3) : 0;
  int w = SEEK_CUR;
  if (std::strcmp(whence, "set") == 0)
    w = SEEK_SET;
  else if (std::strcmp(whence, "end") == 0)
    w = SEEK_END;
  if (std::fseek(f->fp, static_cast<long>(offset), w) != 0)
    panic("seek failed");
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(std::ftell(f->fp))));
  return 1;
}

static int file_lines_iter(State* L) {
  if (!L->at(1)->is_userdata()) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  LFile* f = check_file(L, 1);
  if (!f->fp) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  char buf[8192];
  if (!std::fgets(buf, sizeof(buf), f->fp)) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  std::string line(buf);
  if (!line.empty() && line.back() == '\n')
    line.pop_back();
  L->settop(0);
  push_string(L, line);
  return 1;
}

static int file_lines(State* L) {
  Userdata* u = L->at(1)->as_userdata();
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, closure_new_c(L, file_lines_iter)));
  L->push(TValue::obj(ValueTag::Userdata, u));
  return 1;
}

static int io_open(State* L) {
  std::string filename = std::string(check_string(L, 1)->view());
  std::string mode = L->gettop() >= 2 ? std::string(check_string(L, 2)->view()) : "r";
  FILE* fp = std::fopen(filename.c_str(), mode.c_str());
  if (!fp) {
    L->settop(0);
    L->push(TValue::nil());
    push_string(L, "cannot open " + filename);
    return 2;
  }
  L->settop(0);
  L->push(TValue::obj(ValueTag::Userdata, new_file_ud(L, fp, true, false)));
  return 1;
}

static int io_write_impl(State* L, FILE* fp) {
  for (int i = 1; i <= L->gettop(); ++i) {
    std::string s = value_to_string(*L->at(i));
    std::fwrite(s.data(), 1, s.size(), fp);
  }
  return static_cast<int>(L->gettop());
}

static int io_write(State* L) {
  io_write_impl(L, stdout);
  L->settop(0);
  L->push(TValue::obj(ValueTag::Userdata, new_file_ud(L, stdout, false, true)));
  return 1;
}

static int io_read(State* L) {
  int n = L->gettop();
  Userdata* u = new_file_ud(L, stdin, false, true);
  L->push(TValue::obj(ValueTag::Userdata, u));
  for (int i = n; i >= 1; --i)
    *L->at(i + 1) = *L->at(i);
  *L->at(1) = TValue::obj(ValueTag::Userdata, u);
  return file_read(L);
}

void open_io_lib(State* L) {
  Table* mt = file_metatable(L);
  set_field(L, mt, "read", file_read);
  set_field(L, mt, "write", file_write);
  set_field(L, mt, "close", file_close);
  set_field(L, mt, "flush", file_flush);
  set_field(L, mt, "seek", file_seek);
  set_field(L, mt, "lines", file_lines);
  // Lua file objects look up methods via metatable.__index = metatable.
  set_field_value(L, mt, "__index", TValue::obj(ValueTag::Table, mt));

  Table* io = new_lib(L, 8);
  set_field(L, io, "open", io_open);
  set_field(L, io, "read", io_read);
  set_field(L, io, "write", io_write);
  set_field_value(L, io, "stdin", TValue::obj(ValueTag::Userdata, new_file_ud(L, stdin, false, true)));
  set_field_value(L, io, "stdout", TValue::obj(ValueTag::Userdata, new_file_ud(L, stdout, false, true)));
  set_field_value(L, io, "stderr", TValue::obj(ValueTag::Userdata, new_file_ud(L, stderr, false, true)));
  set_global_value(L, "io", TValue::obj(ValueTag::Table, io));
}

} // namespace luatier
