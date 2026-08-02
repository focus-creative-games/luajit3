#include "lib/libs.hpp"

#include "lib/lib_util.hpp"

#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

#ifdef _WIN32
#include <direct.h>
#define LJ3_UNLINK _unlink
#define LJ3_RENAME rename
#else
#include <unistd.h>
#define LJ3_UNLINK unlink
#define LJ3_RENAME rename
#endif

namespace lj3 {
using namespace lib;

static int os_clock(State* L) {
  using clock = std::chrono::steady_clock;
  static auto start = clock::now();
  double t = std::chrono::duration<double>(clock::now() - start).count();
  L->settop(0);
  L->push(TValue::number(t));
  return 1;
}

static int os_time(State* L) {
  std::time_t t = std::time(nullptr);
  if (L->gettop() >= 1 && L->at(1)->is_table()) {
    std::tm tm{};
    tm.tm_year = static_cast<int>(L->at(1)->as_table()->get(TValue::obj(ValueTag::String, L->intern("year"))).to_number()) - 1900;
    tm.tm_mon = static_cast<int>(L->at(1)->as_table()->get(TValue::obj(ValueTag::String, L->intern("month"))).to_number()) - 1;
    tm.tm_mday = static_cast<int>(L->at(1)->as_table()->get(TValue::obj(ValueTag::String, L->intern("day"))).to_number());
    tm.tm_hour = static_cast<int>(L->at(1)->as_table()->get(TValue::obj(ValueTag::String, L->intern("hour"))).to_number());
    tm.tm_min = static_cast<int>(L->at(1)->as_table()->get(TValue::obj(ValueTag::String, L->intern("min"))).to_number());
    tm.tm_sec = static_cast<int>(L->at(1)->as_table()->get(TValue::obj(ValueTag::String, L->intern("sec"))).to_number());
    t = std::mktime(&tm);
  } else if (L->gettop() >= 1 && L->at(1)->is_number())
    t = static_cast<std::time_t>(L->at(1)->to_number());
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(t)));
  return 1;
}

static int os_difftime(State* L) {
  double t2 = check_number(L, 1);
  double t1 = check_number(L, 2);
  L->settop(0);
  L->push(TValue::number(t2 - t1));
  return 1;
}

static int os_date(State* L) {
  std::string fmt = L->gettop() >= 1 ? std::string(opt_string(L, 1, "%c")) : "%c";
  std::time_t t = L->gettop() >= 2 ? static_cast<std::time_t>(check_int(L, 2))
                                   : std::time(nullptr);
  if (fmt == "*t") {
    std::tm* tm = std::localtime(&t);
    Table* tab = table_new(L, 0, 16);
    tab->set(L, TValue::obj(ValueTag::String, L->intern("year")), TValue::integer(tm->tm_year + 1900));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("month")), TValue::integer(tm->tm_mon + 1));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("day")), TValue::integer(tm->tm_mday));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("hour")), TValue::integer(tm->tm_hour));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("min")), TValue::integer(tm->tm_min));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("sec")), TValue::integer(tm->tm_sec));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("wday")), TValue::integer(tm->tm_wday + 1));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("yday")), TValue::integer(tm->tm_yday + 1));
    tab->set(L, TValue::obj(ValueTag::String, L->intern("isdst")), TValue::boolean(tm->tm_isdst != 0));
    L->settop(0);
    L->push(TValue::obj(ValueTag::Table, tab));
    return 1;
  }
  char buf[256];
  if (std::strftime(buf, sizeof(buf), fmt.c_str(), std::localtime(&t)) == 0)
    panic("invalid date format");
  L->settop(0);
  push_string(L, buf);
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
  if (LJ3_UNLINK(path.c_str()) == 0)
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
  if (LJ3_RENAME(from.c_str(), to.c_str()) == 0)
    L->push(TValue::boolean(true));
  else {
    L->push(TValue::nil());
    push_string(L, "rename failed");
    return 2;
  }
  return 1;
}

static int os_exit(State* L) {
  int code = L->gettop() >= 1 ? static_cast<int>(check_int(L, 1)) : 0;
  std::exit(code);
}

static int os_tmpname(State* L) {
  static int counter = 0;
  std::string name = "lj3tmp_" + std::to_string(++counter) + ".tmp";
  L->settop(0);
  push_string(L, name);
  return 1;
}

static int os_execute(State* L) {
  if (L->gettop() < 1 || L->at(1)->is_nil()) {
    L->settop(0);
    L->push(TValue::boolean(true));
    return 1;
  }
  const char* cmd = check_string(L, 1)->view().data();
  int status = std::system(cmd);
  L->settop(0);
  if (status == -1) {
    L->push(TValue::nil());
    push_string(L, "execute failed");
    return 2;
  }
  L->push(TValue::boolean(status == 0));
  return 1;
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

} // namespace lj3
