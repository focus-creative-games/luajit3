#include "lib/libs.hpp"

#include "lib/lib_util.hpp"

#include <chrono>
#include <cmath>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define LUATIER_UNLINK _unlink
#define LUATIER_RENAME rename
#else
#include <unistd.h>
#define LUATIER_UNLINK unlink
#define LUATIER_RENAME rename
#endif

namespace luatier {
using namespace lib;

static int os_clock(State* L) {
  using clock = std::chrono::steady_clock;
  static auto start = clock::now();
  double t = std::chrono::duration<double>(clock::now() - start).count();
  L->settop(0);
  L->push(TValue::number(t));
  return 1;
}

static int os_difftime(State* L) {
  double t2 = check_number(L, 1);
  double t1 = check_number(L, 2);
  L->settop(0);
  L->push(TValue::number(t2 - t1));
  return 1;
}

#if defined(_WIN32)
static const char kStrftimeOptions[] =
    "aAbBcdHIjmMpSUwWxXyYzZ%||#c#x#d#H#I#j#m#M#S#U#w#W#y#Y";
#else
static const char kStrftimeOptions[] =
    "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%||EcECExEXEyEYOdOeOHOIOmOMOSOuOUOVOwOWOy";
#endif

static constexpr int kMaxDateField = INT_MAX / 2;
static constexpr size_t kSizeTimeFmt = 250;

static void set_date_field(State* L, Table* t, const char* key, int value) {
  t->set(L, TValue::obj(ValueTag::String, L->intern(key)), TValue::integer(value));
}

static void set_date_bool_field(State* L, Table* t, const char* key, int value) {
  if (value < 0)
    return;
  t->set(L, TValue::obj(ValueTag::String, L->intern(key)), TValue::boolean(value != 0));
}

static void set_all_date_fields(State* L, Table* t, std::tm* stm) {
  set_date_field(L, t, "sec", stm->tm_sec);
  set_date_field(L, t, "min", stm->tm_min);
  set_date_field(L, t, "hour", stm->tm_hour);
  set_date_field(L, t, "day", stm->tm_mday);
  set_date_field(L, t, "month", stm->tm_mon + 1);
  set_date_field(L, t, "year", stm->tm_year + 1900);
  set_date_field(L, t, "wday", stm->tm_wday + 1);
  set_date_field(L, t, "yday", stm->tm_yday + 1);
  set_date_bool_field(L, t, "isdst", stm->tm_isdst);
}

static const char* check_strftime_option(const char* conv, size_t convlen, char* cc) {
  const char* option = kStrftimeOptions;
  int oplen = 1;
  cc[0] = '%';
  for (; *option != '\0' && static_cast<size_t>(oplen) <= convlen; option += oplen) {
    if (*option == '|')
      ++oplen;
    else if (std::memcmp(conv, option, static_cast<size_t>(oplen)) == 0) {
      std::memcpy(cc + 1, conv, static_cast<size_t>(oplen));
      cc[oplen + 1] = '\0';
      return conv + oplen;
    }
  }
  std::string bad = "%";
  bad.append(conv, convlen);
  panic("invalid conversion specifier '" + bad + "'");
}

static int64_t check_time(State* L, int idx) {
  int64_t t = check_int(L, idx);
  if (static_cast<std::time_t>(t) != t)
    panic("value out-of-bound");
  return t;
}

static int os_date(State* L) {
  std::string fmt = L->gettop() >= 1 ? std::string(check_string(L, 1)->view()) : "%c";
  std::time_t t = L->gettop() >= 2 ? static_cast<std::time_t>(check_time(L, 2))
                                     : std::time(nullptr);
  const char* s = fmt.data();
  size_t slen = fmt.size();
  const char* se = s + slen;
  std::tm tmr{};
  std::tm* stm = nullptr;
  if (slen > 0 && s[0] == '!') {
    stm = gmtime(&t);
    ++s;
  } else {
    stm = localtime(&t);
  }
  if (!stm)
    panic("time result cannot be represented in this installation");
  tmr = *stm;
  stm = &tmr;
  if (std::strcmp(s, "*t") == 0) {
    Table* tab = table_new(L, 0, 9);
    set_all_date_fields(L, tab, stm);
    L->settop(0);
    L->push(TValue::obj(ValueTag::Table, tab));
    return 1;
  }
  std::string out;
  out.reserve(fmt.size() + 64);
  while (s < se) {
    if (*s != '%') {
      out.push_back(*s++);
      continue;
    }
    ++s;
    char cc[8];
    s = check_strftime_option(s, static_cast<size_t>(se - s), cc);
    char buff[kSizeTimeFmt];
    size_t reslen = std::strftime(buff, sizeof(buff), cc, stm);
    out.append(buff, reslen);
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int get_date_bool_field(State* L, Table* t, const char* key) {
  TValue v = t->get(TValue::obj(ValueTag::String, L->intern(key)));
  if (v.is_nil())
    return -1;
  return v.is_truthy() ? 1 : 0;
}

static int get_date_field(State* L, Table* t, const char* key, int def, int delta) {
  TValue v = t->get(TValue::obj(ValueTag::String, L->intern(key)));
  if (v.is_nil()) {
    if (def < 0)
      panic(std::string("field '") + key + "' missing in date table");
    return def - delta;
  }
  if (!v.is_number())
    panic(std::string("field '") + key + "' is not an integer");
  if (v.is_float()) {
    double d = v.as_float();
    if (std::floor(d) != d)
      panic(std::string("field '") + key + "' is not an integer");
  }
  int64_t res = v.is_int() ? v.as_int() : static_cast<int64_t>(v.as_float());
  if (res < -kMaxDateField || res > kMaxDateField)
    panic(std::string("field '") + key + "' is out-of-bound");
  return static_cast<int>(res) - delta;
}

static int os_time(State* L) {
  std::time_t t;
  if (L->gettop() < 1 || L->at(1)->is_nil()) {
    t = std::time(nullptr);
  } else if (L->at(1)->is_table()) {
    Table* tab = L->at(1)->as_table();
    std::tm ts{};
    ts.tm_sec = get_date_field(L, tab, "sec", 0, 0);
    ts.tm_min = get_date_field(L, tab, "min", 0, 0);
    ts.tm_hour = get_date_field(L, tab, "hour", 12, 0);
    ts.tm_mday = get_date_field(L, tab, "day", -1, 0);
    ts.tm_mon = get_date_field(L, tab, "month", -1, 1);
    ts.tm_year = get_date_field(L, tab, "year", -1, 1900);
    ts.tm_isdst = get_date_bool_field(L, tab, "isdst");
    t = std::mktime(&ts);
    set_all_date_fields(L, tab, &ts);
  } else if (L->at(1)->is_number()) {
    t = static_cast<std::time_t>(check_time(L, 1));
  } else {
    arg_type_error(L, 1, "table or number");
  }
  if (t == static_cast<std::time_t>(-1))
    panic("time result cannot be represented in this installation");
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(t)));
  return 1;
}

static int os_setlocale(State* L) {
  const char* loc = nullptr;
  int category = LC_ALL;
  if (L->gettop() >= 1 && !L->at(1)->is_nil())
    loc = check_string(L, 1)->view().data();
  if (L->gettop() >= 2) {
    const char* cat = check_string(L, 2)->view().data();
    if (std::strcmp(cat, "all") == 0)
      category = LC_ALL;
    else if (std::strcmp(cat, "collate") == 0)
      category = LC_COLLATE;
    else if (std::strcmp(cat, "ctype") == 0)
      category = LC_CTYPE;
    else if (std::strcmp(cat, "monetary") == 0)
      category = LC_MONETARY;
    else if (std::strcmp(cat, "numeric") == 0)
      category = LC_NUMERIC;
    else if (std::strcmp(cat, "time") == 0)
      category = LC_TIME;
    else
      panic("bad argument #2 to 'setlocale' (invalid category)");
  }
  const char* result = std::setlocale(category, loc);
  L->settop(0);
  if (result)
    push_string(L, result);
  else
    L->push(TValue::nil());
  return 1;
}

static int os_getenv(State* L) {
  const char* name = check_string(L, 1)->view().data();
  const char* v = std::getenv(name);
  L->settop(0);
  if (v)
    push_string(L, v);
  else
    L->push(TValue::nil());
  return 1;
}

static int os_remove(State* L) {
  std::string path = std::string(check_string(L, 1)->view());
  L->settop(0);
  if (LUATIER_UNLINK(path.c_str()) == 0)
    L->push(TValue::boolean(true));
  else {
    L->push(TValue::nil());
    push_string(L, "remove failed");
    return 2;
  }
  return 1;
}

static int os_rename(State* L) {
  std::string from = std::string(check_string(L, 1)->view());
  std::string to = std::string(check_string(L, 2)->view());
  L->settop(0);
  if (LUATIER_RENAME(from.c_str(), to.c_str()) == 0)
    L->push(TValue::boolean(true));
  else {
    L->push(TValue::nil());
    push_string(L, "rename failed");
    return 2;
  }
  return 1;
}

static int os_exit(State* L) {
  int code = EXIT_SUCCESS;
  if (L->gettop() >= 1 && !L->at(1)->is_nil()) {
    if (L->at(1)->is_bool())
      code = L->at(1)->is_truthy() ? EXIT_SUCCESS : EXIT_FAILURE;
    else
      code = static_cast<int>(check_int(L, 1));
  }
  // Second arg true: close Lua state before exit (PUC). We don't have a full
  // lua_close path for the process; process exit is enough for the suite.
  (void)(L->gettop() >= 2 && L->at(2)->is_truthy());
  std::exit(code);
}

static int os_tmpname(State* L) {
  // Prefer unique non-existent names (PUC tmpnam). Avoid '.' in the name:
  // require/-l treat '.' as a module separator (breaks main.lua -l tests).
  static int counter = 0;
  for (int attempt = 0; attempt < 128; ++attempt) {
#if defined(_WIN32)
    std::string name = "luatiertmp_" + std::to_string(++counter) + "_" +
                       std::to_string(static_cast<long long>(std::time(nullptr))) + "_" +
                       std::to_string(attempt);
#else
    std::string name = "/tmp/luatiertmp_" + std::to_string(++counter) + "_" +
                       std::to_string(static_cast<long long>(std::time(nullptr))) + "_" +
                       std::to_string(attempt);
#endif
    FILE* probe = std::fopen(name.c_str(), "r");
    if (probe) {
      std::fclose(probe);
      continue;
    }
    L->settop(0);
    push_string(L, name);
    return 1;
  }
  panic("unable to generate a unique filename");
}

static int os_execute(State* L) {
  if (L->gettop() < 1 || L->at(1)->is_nil()) {
    int st = std::system(nullptr);
    L->settop(0);
    L->push(TValue::boolean(st != 0));
    return 1;
  }
  const char* cmd = check_string(L, 1)->view().data();
  errno = 0;
  int status = std::system(cmd);
  return exec_result(L, status);
}

void open_os_lib(State* L) {
  Table* os = new_lib(L, 16);
  set_field(L, os, "clock", os_clock);
  set_field(L, os, "time", os_time);
  set_field(L, os, "difftime", os_difftime);
  set_field(L, os, "date", os_date);
  set_field(L, os, "setlocale", os_setlocale);
  set_field(L, os, "getenv", os_getenv);
  set_field(L, os, "remove", os_remove);
  set_field(L, os, "rename", os_rename);
  set_field(L, os, "exit", os_exit);
  set_field(L, os, "tmpname", os_tmpname);
  set_field(L, os, "execute", os_execute);
  set_global_value(L, "os", TValue::obj(ValueTag::Table, os));
}

} // namespace luatier
