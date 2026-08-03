#include "lib/libs.hpp"

#include "lib/lib_util.hpp"
#include "runtime/closure.hpp"
#include "runtime/upvalue.hpp"
#include "runtime/userdata.hpp"

#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace luatier {
using namespace lib;

constexpr int kMaxLenNum = 200;
constexpr int kMaxArgLine = 250;
constexpr size_t kIoBufferSize = 8192;

struct LFile {
  FILE* fp = nullptr;
  bool close_on_close = true;
  bool is_std = false;
  bool can_read = true;
  bool can_write = true;
};

static void set_file_mode(LFile* lf, const std::string& mode) {
  char m = mode[0];
  bool plus = mode.find('+') != std::string::npos;
  lf->can_read = (m == 'r' || m == 'a' || plus);
  lf->can_write = (m == 'w' || m == 'a' || plus);
}

static const char* kFileMeta = "LUATIER_FILE*";

static Table* file_metatable(State* L) {
  TValue mt = L->registry->get(TValue::obj(ValueTag::String, L->intern(kFileMeta)));
  if (!mt.is_table()) {
    Table* t = new_lib(L, 8);
    set_field_value(L, t, "__name", TValue::obj(ValueTag::String, L->intern("FILE*")));
    L->registry->set(L, TValue::obj(ValueTag::String, L->intern(kFileMeta)),
                     TValue::obj(ValueTag::Table, t));
    return t;
  }
  return mt.as_table();
}

static LFile* lfile_ptr(Userdata* u) {
  return reinterpret_cast<LFile*>(userdata_data(u));
}

static LFile* check_file(State* L, int idx) {
  if (idx > L->gettop())
    panic("(FILE* expected, got no value)");
  if (!L->at(idx)->is_userdata())
    arg_type_error(L, idx, "FILE*");
  Userdata* u = L->at(idx)->as_userdata();
  Table* mt = file_metatable(L);
  if (u->metatable != mt)
    arg_type_error(L, idx, "FILE*");
  return lfile_ptr(u);
}

static FILE* tofile(LFile* f) {
  if (!f->fp)
    panic("attempt to use a closed file");
  return f->fp;
}

static Userdata* new_file_ud(State* L, FILE* fp, bool close_on_close, bool is_std,
                             bool can_read = true, bool can_write = true) {
  Table* mt = file_metatable(L);
  Userdata* u = userdata_new(L, sizeof(LFile), mt);
  auto* lf = lfile_ptr(u);
  lf->fp = fp;
  lf->close_on_close = close_on_close;
  lf->is_std = is_std;
  lf->can_read = can_read;
  lf->can_write = can_write;
  return u;
}

static UpVal* make_closed_upval(State* L, const TValue& v) {
  auto* uv = L->gc.create<UpVal>(GcKind::UpVal);
  uv->open = false;
  uv->closed = v;
  uv->thread = nullptr;
  uv->stack_index = -1;
  return uv;
}

static Closure* current_c_closure(State* L) {
  Thread* th = L->current;
  if (th->frames.empty())
    return nullptr;
  return th->frames.back().cl;
}

static int file_result_fail(State* L, const std::string& filename) {
  int en = errno;
  L->settop(0);
  L->push(TValue::nil());
  if (!filename.empty())
    push_string(L, filename + ": " + std::strerror(en));
  else
    push_string(L, std::strerror(en));
  L->push(TValue::integer(en));
  return 3;
}

static char locale_decpoint() {
  struct lconv* lc = std::localeconv();
  return (lc && lc->decimal_point && lc->decimal_point[0]) ? lc->decimal_point[0] : '.';
}

// Registry keys for current default files (PUC IO_INPUT / IO_OUTPUT).
static const char* kIoInput = "LUATIER_IO_INPUT";
static const char* kIoOutput = "LUATIER_IO_OUTPUT";

static TValue get_default_file(State* L, const char* key) {
  return L->registry->get(TValue::obj(ValueTag::String, L->intern(key)));
}

static void set_default_file(State* L, const char* key, const TValue& f) {
  L->registry->set(L, TValue::obj(ValueTag::String, L->intern(key)), f);
}

static FILE* getiofile(State* L, const char* key, const char* which) {
  TValue cur = get_default_file(L, key);
  if (!cur.is_userdata())
    panic(std::string("standard ") + which + " file is closed");
  LFile* f = lfile_ptr(cur.as_userdata());
  if (!f->fp)
    panic(std::string("standard ") + which + " file is closed");
  return f->fp;
}

/* { READ } */

struct RN {
  FILE* f = nullptr;
  int c = EOF;
  int n = 0;
  char buff[kMaxLenNum + 1];
};

static int rn_nextc(RN* rn) {
  if (rn->n >= kMaxLenNum) {
    rn->buff[0] = '\0';
    return 0;
  }
  rn->buff[rn->n++] = static_cast<char>(rn->c);
  rn->c = std::fgetc(rn->f);
  return 1;
}

static int rn_test2(RN* rn, const char* set) {
  if (rn->c == set[0] || (set[1] && rn->c == set[1]))
    return rn_nextc(rn);
  return 0;
}

static int rn_readdigits(RN* rn, int hex) {
  int count = 0;
  while (((hex ? std::isxdigit(static_cast<unsigned char>(rn->c))
               : std::isdigit(static_cast<unsigned char>(rn->c))) != 0) &&
         rn_nextc(rn))
    ++count;
  return count;
}

static bool read_number(State* L, FILE* fp) {
  RN rn;
  int count = 0;
  int hex = 0;
  char decp[2];
  rn.f = fp;
  rn.n = 0;
  decp[0] = locale_decpoint();
  decp[1] = '.';
  do {
    rn.c = std::fgetc(rn.f);
  } while (std::isspace(static_cast<unsigned char>(rn.c)));
  rn_test2(&rn, "-+");
  if (rn_test2(&rn, "00")) {
    if (rn_test2(&rn, "xX"))
      hex = 1;
    else
      count = 1;
  }
  count += rn_readdigits(&rn, hex);
  if (rn_test2(&rn, decp))
    count += rn_readdigits(&rn, hex);
  if (count > 0 && rn_test2(&rn, hex ? "pP" : "eE")) {
    rn_test2(&rn, "-+");
    rn_readdigits(&rn, 0);
  }
  if (rn.c != EOF)
    std::ungetc(rn.c, rn.f);
  rn.buff[rn.n] = '\0';
  TValue n;
  if (try_to_number(TValue::obj(ValueTag::String, L->intern(rn.buff)), &n)) {
    L->push(n);
    return true;
  }
  return false;
}

static bool test_eof(State* L, FILE* fp) {
  int c = std::fgetc(fp);
  if (c != EOF)
    std::ungetc(c, fp);
  L->push(TValue::obj(ValueTag::String, L->intern("")));
  return c != EOF;
}

static bool read_line(State* L, FILE* fp, bool chop) {
  std::string out;
  int c = 0;
  while (true) {
    c = std::fgetc(fp);
    if (c == EOF || c == '\n')
      break;
    out.push_back(static_cast<char>(c));
  }
  if (!chop && c == '\n')
    out.push_back('\n');
  if (c == '\n' || !out.empty()) {
    push_string(L, out);
    return true;
  }
  return false;
}

static void read_all(State* L, FILE* fp) {
  std::string out;
  char buf[kIoBufferSize];
  size_t nr = 0;
  do {
    nr = std::fread(buf, 1, sizeof(buf), fp);
    out.append(buf, nr);
  } while (nr == sizeof(buf));
  push_string(L, out);
}

static bool read_chars(State* L, FILE* fp, size_t n) {
  std::string data(n, '\0');
  size_t nr = std::fread(data.data(), 1, n, fp);
  data.resize(nr);
  push_string(L, data);
  return nr > 0;
}

static bool read_format(State* L, FILE* fp, const TValue& fmt) {
  if (fmt.is_number()) {
    int64_t n = fmt.is_int() ? fmt.as_int() : static_cast<int64_t>(fmt.as_float());
    if (n < 0)
      panic("invalid format");
    if (n == 0)
      return test_eof(L, fp);
    return read_chars(L, fp, static_cast<size_t>(n));
  }
  if (!fmt.is_string())
    panic("invalid format");
  std::string mode(fmt.as_string()->view());
  if (!mode.empty() && mode[0] == '*')
    mode = mode.substr(1);
  if (mode == "a" || mode == "all") {
    read_all(L, fp);
    return true;
  }
  if (mode == "l" || mode.empty())
    return read_line(L, fp, true);
  if (mode == "L")
    return read_line(L, fp, false);
  if (mode == "n")
    return read_number(L, fp);
  try {
    int64_t n = std::stoll(mode);
    if (n < 0)
      panic("invalid format");
    if (n == 0)
      return test_eof(L, fp);
    return read_chars(L, fp, static_cast<size_t>(n));
  } catch (...) {
    panic("invalid format");
  }
}

static int g_read(State* L, FILE* fp, const std::vector<TValue>& fmts, LFile* lf) {
  if (lf && !lf->can_read) {
    errno = EBADF;
    return file_result_fail(L, "");
  }
  std::clearerr(fp);
  L->settop(0);
  if (fmts.empty()) {
    if (!read_line(L, fp, true)) {
      if (std::ferror(fp))
        return file_result_fail(L, "");
      L->push(TValue::nil());
    }
    return 1;
  }
  bool success = true;
  size_t pushed = 0;
  for (size_t i = 0; i < fmts.size() && success; ++i) {
    int top_before = L->gettop();
    success = read_format(L, fp, fmts[i]);
    ++pushed;
    if (std::ferror(fp))
      return file_result_fail(L, "");
    if (!success) {
      if (L->gettop() > top_before)
        (void)L->pop();
      L->push(TValue::nil());
    }
  }
  return static_cast<int>(pushed);
}

static int file_read(State* L) {
  LFile* f = check_file(L, 1);
  tofile(f);
  std::vector<TValue> fmts;
  for (int i = 2; i <= L->gettop(); ++i)
    fmts.push_back(*L->at(i));
  return g_read(L, f->fp, fmts, f);
}

static int io_readline(State* L) {
  Closure* cl = current_c_closure(L);
  if (!cl || cl->upvals.size() < 3 || !cl->upvals[0])
    panic("file is already closed");
  TValue filev = cl->upvals[0]->get();
  if (!filev.is_userdata())
    panic("file is already closed");
  LFile* lf = lfile_ptr(filev.as_userdata());
  if (!lf->fp)
    panic("file is already closed");

  int nfmts = static_cast<int>(cl->upvals[1]->get().to_number());
  bool toclose = cl->upvals[2]->get().is_truthy();

  std::vector<TValue> fmts;
  fmts.reserve(static_cast<size_t>(nfmts));
  for (int i = 0; i < nfmts; ++i) {
    if (static_cast<size_t>(3 + i) >= cl->upvals.size() || !cl->upvals[static_cast<size_t>(3 + i)])
      panic("file is already closed");
    fmts.push_back(cl->upvals[static_cast<size_t>(3 + i)]->get());
  }

  int nresults = g_read(L, lf->fp, fmts, lf);
  if (nresults > 0 && L->gettop() >= 1 && !L->at(1)->is_nil())
    return nresults;
  if (nresults > 1) {
    std::string msg = L->at(2)->is_string() ? std::string(L->at(2)->as_string()->view()) : "?";
    panic(msg);
  }
  if (toclose && L->gettop() >= 1 && L->at(1)->is_nil()) {
    if (lf->fp && lf->close_on_close && !lf->is_std) {
      std::fclose(lf->fp);
      lf->fp = nullptr;
    }
  }
  L->settop(0);
  return 0;
}

static void aux_lines(State* L, int toclose) {
  int n = L->gettop() - 1;
  if (n > kMaxArgLine)
    panic("too many arguments");
  Closure* cl = closure_new_c(L, io_readline);
  cl->upvals.resize(static_cast<size_t>(3 + (n > 0 ? n : 0)));
  cl->upvals[0] = make_closed_upval(L, *L->at(1));
  cl->upvals[1] = make_closed_upval(L, TValue::integer(n));
  cl->upvals[2] = make_closed_upval(L, TValue::boolean(toclose != 0));
  for (int i = 0; i < n; ++i)
    cl->upvals[static_cast<size_t>(3 + i)] = make_closed_upval(L, *L->at(2 + i));
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, cl));
}

static int file_lines(State* L) {
  check_file(L, 1);
  aux_lines(L, 0);
  return 1;
}

static int io_lines(State* L) {
  if (L->gettop() < 1)
    L->push(TValue::nil());
  int toclose = 0;
  if (L->at(1)->is_nil()) {
    TValue cur = get_default_file(L, kIoInput);
    if (!cur.is_userdata())
      panic("standard input file is closed");
    LFile* lf = lfile_ptr(cur.as_userdata());
    if (!lf->fp)
      panic("standard input file is closed");
    *L->at(1) = cur;
  } else {
    std::string filename = std::string(check_string(L, 1)->view());
    errno = 0;
    FILE* fp = std::fopen(filename.c_str(), "r");
    if (!fp)
      panic("cannot open file '" + filename + "' (" + std::strerror(errno) + ")");
    Userdata* u = new_file_ud(L, fp, true, false);
    lfile_ptr(u)->can_read = true;
    lfile_ptr(u)->can_write = false;
    *L->at(1) = TValue::obj(ValueTag::Userdata, u);
    toclose = 1;
  }
  aux_lines(L, toclose);
  return 1;
}

/* { WRITE } */

static int g_write(State* L, FILE* fp, int arg, int top, LFile* lf) {
  if (lf && !lf->can_write) {
    errno = EBADF;
    return file_result_fail(L, "");
  }
  for (int i = arg; i <= top; ++i) {
    TValue* v = L->at(i);
    if (!v->is_string() && !v->is_number())
      arg_type_error(L, i, "string");
    std::string s = value_to_string(*v);
    if (std::fwrite(s.data(), 1, s.size(), fp) != s.size()) {
      if (std::ferror(fp))
        return file_result_fail(L, "");
      return file_result_fail(L, "");
    }
  }
  return 1;
}

static int file_write(State* L) {
  LFile* f = check_file(L, 1);
  FILE* fp = tofile(f);
  Userdata* u = L->at(1)->as_userdata();
  int r = g_write(L, fp, 2, L->gettop(), f);
  if (r != 1)
    return r;
  L->settop(0);
  L->push(TValue::obj(ValueTag::Userdata, u));
  return 1;
}

static int io_write(State* L) {
  TValue cur = get_default_file(L, kIoOutput);
  if (!cur.is_userdata())
    panic("standard output file is closed");
  LFile* outf = lfile_ptr(cur.as_userdata());
  if (!outf->fp)
    panic("standard output file is closed");
  int n = L->gettop();
  int r = g_write(L, outf->fp, 1, n, outf);
  if (r != 1)
    return r;
  L->settop(0);
  L->push(get_default_file(L, kIoOutput));
  return 1;
}

static int file_gc(State* L) {
  if (L->gettop() < 1)
    panic("(FILE* expected, got no value)");
  if (!L->at(1)->is_userdata())
    arg_type_error(L, 1, "FILE*");
  LFile* f = lfile_ptr(L->at(1)->as_userdata());
  if (f->fp && f->close_on_close && !f->is_std) {
    std::fclose(f->fp);
    f->fp = nullptr;
  }
  return 0;
}

static int file_tostring(State* L) {
  LFile* f = check_file(L, 1);
  if (!f->fp) {
    push_string(L, "file (closed)");
    return 1;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "file (%p)", static_cast<void*>(f->fp));
  push_string(L, buf);
  return 1;
}

static int file_close(State* L) {
  LFile* f = check_file(L, 1);
  if (f->is_std) {
    L->settop(0);
    L->push(TValue::nil());
    push_string(L, "cannot close standard file");
    return 2;
  }
  if (!f->fp)
    panic("attempt to use a closed file");
  if (f->close_on_close)
    std::fclose(f->fp);
  f->fp = nullptr;
  L->settop(0);
  L->push(TValue::boolean(true));
  return 1;
}

static int io_close(State* L) {
  if (L->gettop() < 1 || L->at(1)->is_nil()) {
    L->settop(0);
    L->push(get_default_file(L, kIoOutput));
  }
  return file_close(L);
}

static int io_type(State* L) {
  check_any(L, 1, "type");
  if (!L->at(1)->is_userdata()) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  Userdata* u = L->at(1)->as_userdata();
  Table* mt = file_metatable(L);
  if (u->metatable != mt) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  LFile* f = lfile_ptr(u);
  L->settop(0);
  push_string(L, f->fp ? "file" : "closed file");
  return 1;
}

static bool check_mode(const std::string& mode) {
  if (mode.empty())
    return false;
  size_t i = 0;
  if (mode[i] != 'r' && mode[i] != 'w' && mode[i] != 'a')
    return false;
  ++i;
  if (i < mode.size() && mode[i] == '+')
    ++i;
  for (; i < mode.size(); ++i) {
    if (mode[i] != 'b')
      return false;
  }
  return true;
}

static int file_flush(State* L) {
  LFile* f = check_file(L, 1);
  FILE* fp = tofile(f);
  if (std::fflush(fp) != 0)
    return file_result_fail(L, "");
  L->settop(0);
  L->push(TValue::boolean(true));
  return 1;
}

static int io_flush(State* L) {
  FILE* fp = getiofile(L, kIoOutput, "output");
  if (std::fflush(fp) != 0)
    return file_result_fail(L, "");
  L->settop(0);
  L->push(TValue::boolean(true));
  return 1;
}

static int file_setvbuf(State* L) {
  LFile* f = check_file(L, 1);
  FILE* fp = tofile(f);
  std::string mode = std::string(check_string(L, 2)->view());
  int op = -1;
  if (mode == "no")
    op = _IONBF;
  else if (mode == "full")
    op = _IOFBF;
  else if (mode == "line")
    op = _IOLBF;
  else
    panic("bad argument #2 to 'setvbuf' (invalid option '" + mode + "')");
  size_t sz = static_cast<size_t>(opt_int(L, 3, BUFSIZ));
  if (std::setvbuf(fp, nullptr, op, sz) != 0)
    return file_result_fail(L, "");
  L->settop(0);
  L->push(TValue::boolean(true));
  return 1;
}

static int file_seek(State* L) {
  LFile* f = check_file(L, 1);
  FILE* fp = tofile(f);
  std::string whence_s = L->gettop() >= 2 ? std::string(check_string(L, 2)->view()) : "cur";
  int64_t offset = L->gettop() >= 3 ? check_int(L, 3) : 0;
  int w = SEEK_CUR;
  if (whence_s == "set")
    w = SEEK_SET;
  else if (whence_s == "end")
    w = SEEK_END;
  else if (whence_s != "cur")
    panic("bad argument #2 to 'seek' (invalid option '" + whence_s + "')");
  errno = 0;
  if (std::fseek(fp, static_cast<long>(offset), w) != 0)
    return file_result_fail(L, "");
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(std::ftell(fp))));
  return 1;
}

static int io_open(State* L) {
  std::string filename = std::string(check_string(L, 1)->view());
  std::string mode = L->gettop() >= 2 ? std::string(check_string(L, 2)->view()) : "r";
  if (!check_mode(mode))
    panic("bad argument #2 to 'open' (invalid mode)");
  errno = 0;
  FILE* fp = std::fopen(filename.c_str(), mode.c_str());
  if (!fp)
    return file_result_fail(L, filename);
  Userdata* u = new_file_ud(L, fp, true, false);
  set_file_mode(lfile_ptr(u), mode);
  L->settop(0);
  L->push(TValue::obj(ValueTag::Userdata, u));
  return 1;
}

static int io_tmpfile(State* L) {
  errno = 0;
  FILE* fp = std::tmpfile();
  if (!fp)
    return file_result_fail(L, "");
  Userdata* u = new_file_ud(L, fp, true, false);
  lfile_ptr(u)->can_read = true;
  lfile_ptr(u)->can_write = true;
  L->settop(0);
  L->push(TValue::obj(ValueTag::Userdata, u));
  return 1;
}

static int io_read(State* L) {
  TValue cur = get_default_file(L, kIoInput);
  if (!cur.is_userdata())
    panic("standard input file is closed");
  LFile* inf = lfile_ptr(cur.as_userdata());
  if (!inf->fp)
    panic("standard input file is closed");
  std::vector<TValue> fmts;
  for (int i = 1; i <= L->gettop(); ++i)
    fmts.push_back(*L->at(i));
  return g_read(L, inf->fp, fmts, inf);
}

static int io_input(State* L) {
  if (L->gettop() >= 1 && !L->at(1)->is_nil()) {
    if (L->at(1)->is_string()) {
      std::string filename = std::string(L->at(1)->as_string()->view());
      errno = 0;
      FILE* fp = std::fopen(filename.c_str(), "r");
      if (!fp)
        panic("cannot open file '" + filename + "' (" + std::strerror(errno) + ")");
      Userdata* u = new_file_ud(L, fp, true, false);
      lfile_ptr(u)->can_read = true;
      lfile_ptr(u)->can_write = false;
      set_default_file(L, kIoInput, TValue::obj(ValueTag::Userdata, u));
    } else {
      check_file(L, 1);
      set_default_file(L, kIoInput, *L->at(1));
    }
  }
  L->settop(0);
  L->push(get_default_file(L, kIoInput));
  return 1;
}

static int io_output(State* L) {
  if (L->gettop() >= 1 && !L->at(1)->is_nil()) {
    if (L->at(1)->is_string()) {
      std::string filename = std::string(L->at(1)->as_string()->view());
      errno = 0;
      FILE* fp = std::fopen(filename.c_str(), "w");
      if (!fp)
        panic("cannot open file '" + filename + "' (" + std::strerror(errno) + ")");
      Userdata* u = new_file_ud(L, fp, true, false);
      lfile_ptr(u)->can_read = false;
      lfile_ptr(u)->can_write = true;
      set_default_file(L, kIoOutput, TValue::obj(ValueTag::Userdata, u));
    } else {
      check_file(L, 1);
      set_default_file(L, kIoOutput, *L->at(1));
    }
  }
  L->settop(0);
  L->push(get_default_file(L, kIoOutput));
  return 1;
}

void open_io_lib(State* L) {
  Table* mt = file_metatable(L);
  set_field(L, mt, "read", file_read);
  set_field(L, mt, "write", file_write);
  set_field(L, mt, "close", file_close);
  set_field(L, mt, "flush", file_flush);
  set_field(L, mt, "seek", file_seek);
  set_field(L, mt, "setvbuf", file_setvbuf);
  set_field(L, mt, "lines", file_lines);
  set_field(L, mt, "__gc", file_gc);
  set_field(L, mt, "__tostring", file_tostring);
  set_field_value(L, mt, "__index", TValue::obj(ValueTag::Table, mt));

  TValue stdin_ud =
      TValue::obj(ValueTag::Userdata, new_file_ud(L, stdin, false, true, true, false));
  TValue stdout_ud =
      TValue::obj(ValueTag::Userdata, new_file_ud(L, stdout, false, true, false, true));
  TValue stderr_ud =
      TValue::obj(ValueTag::Userdata, new_file_ud(L, stderr, false, true, false, true));
  set_default_file(L, kIoInput, stdin_ud);
  set_default_file(L, kIoOutput, stdout_ud);

  Table* io = new_lib(L, 16);
  set_field(L, io, "open", io_open);
  set_field(L, io, "close", io_close);
  set_field(L, io, "type", io_type);
  set_field(L, io, "read", io_read);
  set_field(L, io, "write", io_write);
  set_field(L, io, "input", io_input);
  set_field(L, io, "output", io_output);
  set_field(L, io, "lines", io_lines);
  set_field(L, io, "flush", io_flush);
  set_field(L, io, "tmpfile", io_tmpfile);
  set_field_value(L, io, "stdin", stdin_ud);
  set_field_value(L, io, "stdout", stdout_ud);
  set_field_value(L, io, "stderr", stderr_ud);
  set_global_value(L, "io", TValue::obj(ValueTag::Table, io));
}

} // namespace luatier
