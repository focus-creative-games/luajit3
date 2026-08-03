#include "lib/libs.hpp"

#include "lib/ldump.hpp"
#include "lib/lib_util.hpp"
#include "runtime/closure.hpp"
#include "vm/interpreter.hpp"
#include "vm/meta.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace luatier {
using namespace lib;

static int str_len(State* L) {
  size_t n = check_string_self(L, 1)->len;
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(n)));
  return 1;
}

static int str_sub(State* L) {
  std::string_view s = check_string_self(L, 1)->view();
  int64_t i = check_int(L, 2);
  int64_t j = L->gettop() >= 3 ? check_int(L, 3) : static_cast<int64_t>(s.size());
  if (i < 0)
    i = static_cast<int64_t>(s.size()) + i + 1;
  if (j < 0)
    j = static_cast<int64_t>(s.size()) + j + 1;
  i = std::max<int64_t>(1, i);
  j = std::min<int64_t>(static_cast<int64_t>(s.size()), j);
  L->settop(0);
  if (i > j)
    push_string(L, "");
  else
    push_string(L, s.substr(static_cast<size_t>(i - 1), static_cast<size_t>(j - i + 1)));
  return 1;
}

static int str_rep(State* L) {
  std::string_view s = check_string(L, 1)->view();
  int64_t n = check_int(L, 2);
  std::string_view sep = L->gettop() >= 3 ? check_string(L, 3)->view() : "";
  if (n <= 0) {
    L->settop(0);
    push_string(L, "");
    return 1;
  }
  // PUC MAXSIZE (== INT_MAX on LP64). Same overflow guard as luaB_rep.
  constexpr size_t kMaxSize = static_cast<size_t>((std::numeric_limits<int>::max)());
  size_t l = s.size();
  size_t lsep = sep.size();
  if (l + lsep < l || l + lsep > kMaxSize / static_cast<size_t>(n))
    panic("resulting string too large");
  size_t total = static_cast<size_t>(n) * l + (n > 0 ? static_cast<size_t>(n - 1) * lsep : 0);
  std::string out;
  out.reserve(total);
  for (int64_t i = 0; i < n; ++i) {
    if (i)
      out.append(sep);
    out.append(s);
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int str_reverse(State* L) {
  std::string s(check_string(L, 1)->view());
  std::reverse(s.begin(), s.end());
  L->settop(0);
  push_string(L, s);
  return 1;
}

static int str_lower(State* L) {
  std::string s(check_string(L, 1)->view());
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  L->settop(0);
  push_string(L, s);
  return 1;
}

static int str_upper(State* L) {
  std::string s(check_string(L, 1)->view());
  for (char& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  L->settop(0);
  push_string(L, s);
  return 1;
}

static int str_byte(State* L) {
  std::string_view s = check_string(L, 1)->view();
  const int64_t len = static_cast<int64_t>(s.size());
  auto posrelat = [len](int64_t pos) -> int64_t {
    if (pos < 0)
      pos += len + 1;
    return pos;
  };
  // PUC: j defaults to i (not #s) when the 3rd argument is omitted.
  int64_t i = posrelat(L->gettop() >= 2 ? check_int(L, 2) : 1);
  int64_t j = posrelat(L->gettop() >= 3 ? check_int(L, 3) : i);
  if (i < 1)
    i = 1;
  if (j > len)
    j = len;
  L->settop(0);
  if (i > j)
    return 0;
  for (int64_t k = i; k <= j; ++k)
    L->push(TValue::integer(static_cast<unsigned char>(s[static_cast<size_t>(k - 1)])));
  return static_cast<int>(j - i + 1);
}

static int str_char(State* L) {
  std::string out;
  for (int i = 1; i <= L->gettop(); ++i) {
    int64_t c = check_int(L, i);
    out.push_back(static_cast<char>(c & 0xFF));
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static std::string format_quote(const std::string& s) {
  // PUC addquoted: ", \, and newline → backslash + that byte (newline stays a real LF).
  std::string q = "\"";
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"' || c == '\\' || c == '\n') {
      q += '\\';
      q += static_cast<char>(c);
    } else if (std::iscntrl(c)) {
      // Match Lua 5.3: prefer \ddd; keep digit after escapes unambiguous.
      char buff[8];
      if (i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))
        std::snprintf(buff, sizeof(buff), "\\%03d", static_cast<int>(c));
      else
        std::snprintf(buff, sizeof(buff), "\\%d", static_cast<int>(c));
      q += buff;
    } else {
      q += static_cast<char>(c);
    }
  }
  q += '"';
  return q;
}

// PUC num2straux / lua_number2strx: portable %a/%A (MSVC sprintf is too verbose / loses -0).
static double format_adddigit(char* buff, int n, double x) {
  double dd = std::floor(x);
  int d = static_cast<int>(dd);
  buff[n] = static_cast<char>(d < 10 ? d + '0' : d - 10 + 'a');
  return x - dd;
}

static std::string format_hexfloat(double x, bool upper) {
  char buff[128];
  int n = 0;
  if (x != x || x == HUGE_VAL || x == -HUGE_VAL) {
    n = std::snprintf(buff, sizeof(buff), "%.14g", x);
  } else if (x == 0) {
    // MSVC "%.14g" drops the sign of -0; use signbit explicitly.
    n = std::snprintf(buff, sizeof(buff), "%s0x0p+0", std::signbit(x) ? "-" : "");
  } else {
    int e = 0;
    double m = std::frexp(x, &e);
    if (m < 0) {
      buff[n++] = '-';
      m = -m;
    }
    // DBL_MANT_DIG=53 → L_NBFD = ((53-1)%4)+1 = 1
    constexpr int L_NBFD = ((std::numeric_limits<double>::digits - 1) % 4) + 1;
    buff[n++] = '0';
    buff[n++] = 'x';
    m = format_adddigit(buff, n++, m * static_cast<double>(1 << L_NBFD));
    e -= L_NBFD;
    if (m > 0) {
      buff[n++] = '.';
      do {
        m = format_adddigit(buff, n++, m * 16.0);
      } while (m > 0 && n < 120);
    }
    n += std::snprintf(buff + n, sizeof(buff) - static_cast<size_t>(n), "p%+d", e);
  }
  if (n < 0)
    n = 0;
  std::string out(buff, static_cast<size_t>(n));
  if (upper) {
    for (char& c : out)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

// PUC scanformat: flags "-+ #0", width/precision at most 2 digits each.
static const char* format_scan(const char* strfrmt, const char* end, std::string& form) {
  static constexpr const char* FLAGS = "-+ #0";
  const char* p = strfrmt;
  while (p < end && std::strchr(FLAGS, *p) != nullptr)
    ++p;
  if (static_cast<size_t>(p - strfrmt) >= std::strlen(FLAGS) + 1)
    panic("invalid format (repeated flags)");
  if (p < end && std::isdigit(static_cast<unsigned char>(*p)))
    ++p;
  if (p < end && std::isdigit(static_cast<unsigned char>(*p)))
    ++p;
  if (p < end && *p == '.') {
    ++p;
    if (p < end && std::isdigit(static_cast<unsigned char>(*p)))
      ++p;
    if (p < end && std::isdigit(static_cast<unsigned char>(*p)))
      ++p;
  }
  if (p < end && std::isdigit(static_cast<unsigned char>(*p)))
    panic("invalid format (width or precision too long)");
  form.assign("%");
  form.append(strfrmt, static_cast<size_t>((p - strfrmt) + 1));
  return p;
}

static std::string format_one(const char*& p, const char* end, State* L, int& argi, int top) {
  ++p;
  if (p >= end)
    return "%";
  if (*p == '%') {
    ++p;
    return "%";
  }

  if (argi > top)
    panic("no value");

  std::string form;
  p = format_scan(p, end, form);
  if (p >= end)
    panic("invalid format");
  char conv = *p++;

  auto snformat = [&](auto arg) -> std::string {
    int need = std::snprintf(nullptr, 0, form.c_str(), arg);
    if (need < 0)
      return {};
    std::string out(static_cast<size_t>(need) + 1, '\0');
    std::snprintf(out.data(), out.size(), form.c_str(), arg);
    out.resize(static_cast<size_t>(need));
    return out;
  };

  if (conv == 'q') {
    const TValue* qv = L->at(argi++);
    if (qv->is_string())
      return format_quote(std::string(qv->as_string()->view()));
    if (qv->is_int()) {
      int64_t n = qv->as_int();
      if (n == std::numeric_limits<int64_t>::min()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(n));
        return buf;
      }
      return std::to_string(n);
    }
    if (qv->is_float())
      return format_hexfloat(qv->as_float(), false);
    if (qv->is_nil())
      return "nil";
    if (qv->is_bool())
      return qv->payload ? "true" : "false";
    panic("value has no literal form");
  }

  const TValue* val = L->at(argi++);

  switch (conv) {
  case 's': {
    std::string s;
    if (val->is_string()) {
      s = std::string(val->as_string()->view());
    } else {
      TValue mm = get_metamethod(L, *val, "__tostring");
      if (mm.is_function()) {
        int save_top = L->gettop();
        L->push(mm);
        L->push(*val);
        call_closure(L, mm.as_closure(), 1, 1);
        if (!L->at(L->gettop())->is_string())
          panic("'__tostring' must return a string");
        s = std::string(L->at(L->gettop())->as_string()->view());
        L->settop(save_top);
      } else {
        TValue name = get_metamethod(L, *val, "__name");
        if (name.is_string()) {
          char buf[128];
          void* ptr = val->is_table() ? static_cast<void*>(val->as_table())
                                      : reinterpret_cast<void*>(static_cast<uintptr_t>(val->payload));
          std::snprintf(buf, sizeof(buf), "%s: %p",
                        std::string(name.as_string()->view()).c_str(), ptr);
          s = buf;
        } else {
          s = value_to_string(*val);
        }
      }
    }
    if (form == "%s")
      return s;
    if (s.find('\0') != std::string::npos)
      panic("string contains zeros");
    // PUC: no precision and long string → keep entire string (avoid %s truncation).
    if (form.find('.') == std::string::npos && s.size() >= 100)
      return s;
    return snformat(s.c_str());
  }
  case 'c': {
    int64_t n = 0;
    if (val->is_int())
      n = val->as_int();
    else if (val->is_number())
      n = static_cast<int64_t>(val->to_number());
    else
      panic("number expected");
    return snformat(static_cast<int>(n));
  }
  case 'd':
  case 'i': {
    std::string f64 = form;
    f64.insert(f64.size() - 1, "ll");
    int64_t n = 0;
    if (val->is_int())
      n = val->as_int();
    else if (val->is_number())
      n = static_cast<int64_t>(val->to_number());
    else
      panic("number has no integer representation");
    int need = std::snprintf(nullptr, 0, f64.c_str(), static_cast<long long>(n));
    std::string out(static_cast<size_t>(need) + 1, '\0');
    std::snprintf(out.data(), out.size(), f64.c_str(), static_cast<long long>(n));
    out.resize(static_cast<size_t>(need));
    return out;
  }
  case 'u':
  case 'o':
  case 'x':
  case 'X': {
    std::string f64 = form;
    f64.insert(f64.size() - 1, "ll");
    uint64_t n = 0;
    if (val->is_int())
      n = static_cast<uint64_t>(val->as_int());
    else if (val->is_number())
      n = static_cast<uint64_t>(val->to_number());
    else
      panic("number has no integer representation");
    int need = std::snprintf(nullptr, 0, f64.c_str(), static_cast<unsigned long long>(n));
    std::string out(static_cast<size_t>(need) + 1, '\0');
    std::snprintf(out.data(), out.size(), f64.c_str(), static_cast<unsigned long long>(n));
    out.resize(static_cast<size_t>(need));
    return out;
  }
  case 'a':
  case 'A':
    if (form != "%a" && form != "%A")
      panic("modifiers for format '%a'/'%A' not implemented");
    if (!val->is_number())
      panic("number expected");
    return format_hexfloat(val->to_number(), conv == 'A');
  case 'f':
  case 'e':
  case 'E':
  case 'g':
  case 'G':
    if (!val->is_number())
      panic("number expected");
    return snformat(val->to_number());
  case 'p': {
    void* ptr = nullptr;
    if (val->is_table())
      ptr = val->as_table();
    else if (val->is_function())
      ptr = val->as_closure();
    else if (val->is_userdata())
      ptr = val->as_userdata();
    else if (val->is_thread())
      ptr = val->as_thread();
    else if (val->is_string())
      ptr = val->as_string();
    return snformat(ptr);
  }
  default:
    panic(std::string("invalid option '%") + conv + "' to 'format'");
  }
}

static int str_format(State* L) {
  std::string_view fmt = check_string(L, 1)->view();
  int top = L->gettop();
  int argi = 2;
  std::string out;
  const char* p = fmt.data();
  const char* end = p + fmt.size();
  while (p < end) {
    if (*p == '%') {
      out += format_one(p, end, L, argi, top);
    } else {
      out += *p++;
    }
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int str_dump(State* L) {
  if (!L->at(1)->is_function())
    panic("string.dump: function expected");
  Closure* cl = L->at(1)->as_closure();
  if (cl->is_c)
    panic("unable to dump given function");
  bool strip = L->gettop() >= 2 && L->at(2)->is_truthy();
  std::string blob = dump_proto(cl->proto, strip);
  L->settop(0);
  push_string(L, blob);
  return 1;
}

// --- Lua-style pattern engine (simplified, PUC-compatible core) ---

static constexpr size_t kCapUnfinished = static_cast<size_t>(-1);
static constexpr size_t kCapPosition = static_cast<size_t>(-2);

// PUC MAXCCALLS: recursion budget for match(); prevents C-stack overflow.
constexpr int kMaxMatchDepth = 200;

struct MatchState {
  std::string_view s;
  std::string_view p;
  size_t match_start = 0;
  size_t match_end = 0;
  int matchdepth = kMaxMatchDepth;
  std::vector<std::pair<size_t, size_t>> captures;
};

struct MatchDepthGuard {
  MatchState& ms;
  explicit MatchDepthGuard(MatchState& m) : ms(m) {
    if (ms.matchdepth-- == 0)
      panic("pattern too complex");
  }
  ~MatchDepthGuard() { ++ms.matchdepth; }
  MatchDepthGuard(const MatchDepthGuard&) = delete;
  MatchDepthGuard& operator=(const MatchDepthGuard&) = delete;
};

static bool match_class(char ch, char clas) {
  bool res = false;
  switch (std::tolower(static_cast<unsigned char>(clas))) {
  case 'a': res = std::isalpha(static_cast<unsigned char>(ch)) != 0; break;
  case 'c': res = std::iscntrl(static_cast<unsigned char>(ch)) != 0; break;
  case 'd': res = std::isdigit(static_cast<unsigned char>(ch)) != 0; break;
  case 'g': res = std::isgraph(static_cast<unsigned char>(ch)) != 0; break;
  case 'l': res = std::islower(static_cast<unsigned char>(ch)) != 0; break;
  case 'p': res = std::ispunct(static_cast<unsigned char>(ch)) != 0; break;
  case 's': res = std::isspace(static_cast<unsigned char>(ch)) != 0; break;
  case 'u': res = std::isupper(static_cast<unsigned char>(ch)) != 0; break;
  case 'w': res = std::isalnum(static_cast<unsigned char>(ch)) != 0; break;
  case 'x': res = std::isxdigit(static_cast<unsigned char>(ch)) != 0; break;
  case 'z': res = ch == '\0'; break;
  default: return ch == clas;
  }
  return std::islower(static_cast<unsigned char>(clas)) ? res : !res;
}

// Bracket class at `pi` (`[` ...); sets *out_ep to index after `]`.
// `c` may be the virtual '\0' used at string frontiers (PUC src_init/src_end).
static bool match_bracket_class(MatchState& ms, char c, size_t pi, size_t* out_ep) {
  if (pi >= ms.p.size() || ms.p[pi] != '[')
    return false;
  size_t j = pi + 1;
  bool negate = j < ms.p.size() && ms.p[j] == '^';
  if (negate)
    ++j;
  // PUC: first ']' after '[' / '[^' is a literal member, not the closer.
  bool found = false;
  size_t k = j;
  do {
    if (k >= ms.p.size())
      return false;
    if (ms.p[k] == '%' && k + 1 < ms.p.size()) {
      if (match_class(c, ms.p[k + 1]))
        found = true;
      k += 2;
    } else {
      char lo = ms.p[k++];
      char hi = lo;
      if (k < ms.p.size() && ms.p[k] == '-' && k + 1 < ms.p.size() && ms.p[k + 1] != ']') {
        hi = ms.p[k + 1];
        k += 2;
      }
      if (static_cast<unsigned char>(lo) <= static_cast<unsigned char>(c) &&
          static_cast<unsigned char>(c) <= static_cast<unsigned char>(hi))
        found = true;
    }
  } while (k < ms.p.size() && ms.p[k] != ']');
  if (k >= ms.p.size() || ms.p[k] != ']')
    panic("malformed pattern (missing ']')");
  *out_ep = k + 1;
  return negate ? !found : found;
}

// Advance past one pattern item; returns whether char at `si` matches. `pi` is the
// start of the item; on return `*out_pi` is the index after the item.
static bool single_match(MatchState& ms, size_t si, size_t pi, size_t* out_pi) {
  if (pi >= ms.p.size())
    return false;
  char p0 = ms.p[pi];
  if (p0 == '.') {
    *out_pi = pi + 1;
    return si < ms.s.size();
  }
  if (p0 == '%') {
    if (pi + 1 >= ms.p.size())
      return false;
    *out_pi = pi + 2;
    if (si >= ms.s.size())
      return false;
    return match_class(ms.s[si], ms.p[pi + 1]);
  }
  if (p0 == '[') {
    if (si >= ms.s.size()) {
      // Still need out_pi for callers that only care about class end.
      size_t ep = 0;
      match_bracket_class(ms, '\0', pi, &ep);
      *out_pi = ep ? ep : pi;
      return false;
    }
    return match_bracket_class(ms, ms.s[si], pi, out_pi);
  }
  *out_pi = pi + 1;
  return si < ms.s.size() && ms.s[si] == p0;
}

static bool match(MatchState& ms, size_t si, size_t pi);

static bool max_expand(MatchState& ms, size_t si, size_t pi, size_t ep) {
  size_t i = 0;
  size_t out = ep;
  while (single_match(ms, si + i, pi, &out))
    ++i;
  while (true) {
    if (match(ms, si + i, ep + 1))
      return true;
    if (i == 0)
      return false;
    --i;
  }
}

static bool min_expand(MatchState& ms, size_t si, size_t pi, size_t ep) {
  for (;;) {
    if (match(ms, si, ep + 1))
      return true;
    size_t out = ep;
    if (!single_match(ms, si, pi, &out))
      return false;
    ++si;
  }
}

static size_t class_end(MatchState& ms, size_t pi) {
  switch (ms.p[pi++]) {
  case '%':
    if (pi >= ms.p.size())
      panic("malformed pattern (ends with '%')");
    return pi + 1;
  case '[':
    if (pi < ms.p.size() && ms.p[pi] == '^')
      ++pi;
    do {
      if (pi >= ms.p.size())
        panic("malformed pattern (missing ']')");
      if (ms.p[pi] == '%' && pi + 1 < ms.p.size())
        pi += 2;
      else
        ++pi;
    } while (pi < ms.p.size() && ms.p[pi] != ']');
    if (pi >= ms.p.size() || ms.p[pi] != ']')
      panic("malformed pattern (missing ']')");
    return pi + 1;
  default:
    return pi;
  }
}

static bool start_capture(MatchState& ms, size_t si, size_t pi, bool position) {
  ms.captures.push_back({si, position ? kCapPosition : kCapUnfinished});
  bool ok = match(ms, si, pi);
  if (!ok)
    ms.captures.pop_back();
  return ok;
}

static bool end_capture(MatchState& ms, size_t si, size_t pi) {
  for (int i = static_cast<int>(ms.captures.size()) - 1; i >= 0; --i) {
    if (ms.captures[static_cast<size_t>(i)].second == kCapUnfinished) {
      ms.captures[static_cast<size_t>(i)].second = si;
      bool ok = match(ms, si, pi);
      if (!ok)
        ms.captures[static_cast<size_t>(i)].second = kCapUnfinished;
      return ok;
    }
  }
  panic("invalid pattern capture");
}

static bool match_capture(MatchState& ms, size_t si, int cap) {
  if (cap < 0 || cap >= static_cast<int>(ms.captures.size()))
    panic("invalid capture index %" + std::to_string(cap + 1));
  auto [a, b] = ms.captures[static_cast<size_t>(cap)];
  if (b == kCapUnfinished || b == kCapPosition)
    panic("invalid capture index %" + std::to_string(cap + 1));
  size_t len = b - a;
  if (si + len > ms.s.size())
    return false;
  return ms.s.substr(si, len) == ms.s.substr(a, len);
}

static bool match_balance(MatchState& ms, size_t si, size_t pi) {
  if (pi + 1 >= ms.p.size())
    panic("malformed pattern (missing arguments to '%b')");
  char open = ms.p[pi];
  char close = ms.p[pi + 1];
  if (si >= ms.s.size() || ms.s[si] != open)
    return false;
  int depth = 1;
  size_t j = si + 1;
  while (j < ms.s.size()) {
    if (ms.s[j] == close) {
      if (--depth == 0)
        return match(ms, j + 1, pi + 2);
    } else if (ms.s[j] == open) {
      ++depth;
    }
    ++j;
  }
  return false;
}

static bool match(MatchState& ms, size_t si, size_t pi) {
  MatchDepthGuard depth(ms);
  init:
  if (pi >= ms.p.size()) {
    ms.match_end = si;
    return true;
  }
  switch (ms.p[pi]) {
  case '(':
    if (pi + 1 < ms.p.size() && ms.p[pi + 1] == ')')
      return start_capture(ms, si, pi + 2, true);
    return start_capture(ms, si, pi + 1, false);
  case ')':
    return end_capture(ms, si, pi + 1);
  case '$':
    if (pi + 1 == ms.p.size())
      return (si >= ms.s.size()) ? (ms.match_end = si, true) : false;
    break;
  case '%': {
    if (pi + 1 >= ms.p.size())
      panic("malformed pattern (ends with '%')");
    char spec = ms.p[pi + 1];
    if (spec == 'b')
      return match_balance(ms, si, pi + 2);
    if (spec == 'f') {
      if (pi + 2 >= ms.p.size() || ms.p[pi + 2] != '[')
        panic("missing '[' after '%f' in pattern");
      // PUC: virtual '\0' before src_init and at src_end.
      char previous = (si == 0) ? '\0' : ms.s[si - 1];
      char current = (si >= ms.s.size()) ? '\0' : ms.s[si];
      size_t ep = 0;
      bool prev_in = match_bracket_class(ms, previous, pi + 2, &ep);
      bool cur_in = match_bracket_class(ms, current, pi + 2, &ep);
      if (prev_in || !cur_in)
        return false;
      pi = ep;
      goto init;
    }
    if (spec >= '1' && spec <= '9') {
      // PUC: %1 is captures[0] (l - '1').
      int cap = spec - '1';
      if (!match_capture(ms, si, cap))
        return false;
      auto [a, b] = ms.captures[static_cast<size_t>(cap)];
      si += b - a;
      pi += 2;
      goto init;
    }
    if (spec == '0')
      panic("invalid capture index %0");
    break;
  }
  default:
    break;
  }

  size_t ep = class_end(ms, pi);
  char nextp = (ep < ms.p.size()) ? ms.p[ep] : '\0';
  if (nextp == '*')
    return max_expand(ms, si, pi, ep);
  if (nextp == '+') {
    size_t out = 0;
    if (!single_match(ms, si, pi, &out))
      return false;
    return max_expand(ms, si + 1, pi, ep);
  }
  if (nextp == '-')
    return min_expand(ms, si, pi, ep);
  if (nextp == '?') {
    size_t out = 0;
    if (single_match(ms, si, pi, &out) && match(ms, si + 1, ep + 1))
      return true;
    return match(ms, si, ep + 1);
  }

  size_t out = 0;
  if (!single_match(ms, si, pi, &out))
    return false;
  si = si + 1;
  pi = ep;
  goto init;
}

static bool match_search(MatchState& ms, size_t start, bool anchor) {
  size_t pi = 0;
  if (!ms.p.empty() && ms.p[0] == '^') {
    anchor = true;
    pi = 1;
  }
  size_t last = anchor ? start : ms.s.size();
  for (size_t i = start; i <= last; ++i) {
    ms.captures.clear();
    ms.match_start = i;
    if (match(ms, i, pi))
      return true;
    if (anchor)
      break;
  }
  return false;
}

static std::string capture_string(const MatchState& ms, size_t idx) {
  auto [a, b] = ms.captures[idx];
  if (b == kCapPosition)
    return std::to_string(static_cast<long long>(a + 1));
  if (b == kCapUnfinished)
    panic("unfinished capture");
  if (a > b)
    return "";
  return std::string(ms.s.substr(a, b - a));
}

static int str_find(State* L) {
  std::string_view s = check_string(L, 1)->view();
  std::string_view p = check_string(L, 2)->view();
  int64_t init = L->gettop() >= 3 ? check_int(L, 3) : 1;
  bool plain = L->gettop() >= 4 && L->at(4)->is_truthy();
  if (init < 0)
    init = static_cast<int64_t>(s.size()) + init + 1;
  init = std::max<int64_t>(1, init);
  size_t start = static_cast<size_t>(init - 1);
  L->settop(0);
  if (plain) {
    size_t pos = s.find(p, start);
    if (pos == std::string_view::npos)
      L->push(TValue::nil());
    else {
      L->push(TValue::integer(static_cast<int64_t>(pos + 1)));
      L->push(TValue::integer(static_cast<int64_t>(pos + p.size())));
    }
    return L->gettop();
  }
  MatchState ms;
  ms.s = s;
  ms.p = p;
  if (match_search(ms, start, false)) {
    L->push(TValue::integer(static_cast<int64_t>(ms.match_start + 1)));
    L->push(TValue::integer(static_cast<int64_t>(ms.match_end)));
    for (size_t i = 0; i < ms.captures.size(); ++i) {
      auto [a, b] = ms.captures[i];
      if (b == kCapPosition)
        L->push(TValue::integer(static_cast<int64_t>(a + 1)));
      else
        push_string(L, capture_string(ms, i));
    }
    return L->gettop();
  }
  L->push(TValue::nil());
  return 1;
}

static int str_match(State* L) {
  std::string_view s = check_string(L, 1)->view();
  std::string_view p = check_string(L, 2)->view();
  int64_t init = L->gettop() >= 3 ? check_int(L, 3) : 1;
  if (init < 0)
    init = static_cast<int64_t>(s.size()) + init + 1;
  init = std::max<int64_t>(1, init);
  MatchState ms;
  ms.s = s;
  ms.p = p;
  L->settop(0);
  if (match_search(ms, static_cast<size_t>(init - 1), false)) {
    if (ms.captures.empty()) {
      push_string(L, std::string(ms.s.substr(ms.match_start, ms.match_end - ms.match_start)));
      return 1;
    }
    for (size_t i = 0; i < ms.captures.size(); ++i) {
      auto [a, b] = ms.captures[i];
      if (b == kCapPosition)
        L->push(TValue::integer(static_cast<int64_t>(a + 1)));
      else
        push_string(L, capture_string(ms, i));
    }
    return L->gettop();
  }
  L->push(TValue::nil());
  return 1;
}

static std::string gsub_expand_repl(const std::string& repl, const MatchState& ms) {
  std::string out;
  auto whole = [&]() {
    return std::string(ms.s.substr(ms.match_start, ms.match_end - ms.match_start));
  };
  for (size_t i = 0; i < repl.size(); ++i) {
    if (repl[i] == '%' && i + 1 < repl.size()) {
      char c = repl[++i];
      if (c == '%') {
        out += '%';
      } else if (c == '0') {
        out += whole();
      } else if (c >= '1' && c <= '9') {
        size_t idx = static_cast<size_t>(c - '1');
        // PUC push_onecapture: with no captures, %1 is the whole match.
        if (idx < ms.captures.size())
          out += capture_string(ms, idx);
        else if (idx == 0)
          out += whole();
        else
          panic("invalid capture index %" + std::to_string(idx + 1));
      } else {
        panic("invalid use of '%' in replacement string");
      }
    } else if (repl[i] == '%' && i + 1 >= repl.size()) {
      panic("invalid use of '%' in replacement string");
    } else {
      out += repl[i];
    }
  }
  return out;
}

static int str_gsub(State* L) {
  std::string s(check_string(L, 1)->view());
  std::string_view pat = check_string(L, 2)->view();
  TValue repl_v = *L->at(3);
  int64_t max_s = L->gettop() >= 4 ? check_int(L, 4) : static_cast<int64_t>(s.size()) + 1;
  std::string out;
  size_t src = 0;
  // PUC 5.3.3+: reject a subsequent empty match that ends at the same point.
  size_t lastmatch = static_cast<size_t>(-1);
  int count = 0;
  bool anchor = !pat.empty() && pat[0] == '^';
  std::string_view pat_body = pat;
  if (anchor && !pat_body.empty())
    pat_body.remove_prefix(1);

  auto apply_repl = [&](MatchState& ms) {
    if (repl_v.is_function()) {
      int base = L->gettop();
      L->push(repl_v);
      int nargs = 0;
      if (ms.captures.empty()) {
        push_string(L, std::string(ms.s.substr(ms.match_start, ms.match_end - ms.match_start)));
        nargs = 1;
      } else {
        for (size_t c = 0; c < ms.captures.size(); ++c) {
          auto [a, b] = ms.captures[c];
          if (b == kCapPosition)
            L->push(TValue::integer(static_cast<int64_t>(a + 1)));
          else
            push_string(L, capture_string(ms, c));
        }
        nargs = static_cast<int>(ms.captures.size());
      }
      call_closure(L, repl_v.as_closure(), nargs, 1);
      TValue r = *L->at(base + 1);
      // PUC: nil/false keeps the original match text.
      if (r.is_nil() || (r.is_bool() && !r.payload))
        out.append(ms.s.substr(ms.match_start, ms.match_end - ms.match_start));
      else
        out += value_to_string(r);
      L->settop(base);
    } else if (repl_v.is_table()) {
      // PUC add_value: key is whole match, or first capture (position → integer).
      TValue key;
      if (ms.captures.empty()) {
        key = TValue::obj(ValueTag::String,
                          L->intern(std::string(ms.s.substr(ms.match_start, ms.match_end - ms.match_start))));
      } else {
        auto [a, b] = ms.captures[0];
        if (b == kCapPosition)
          key = TValue::integer(static_cast<int64_t>(a + 1));
        else
          key = TValue::obj(ValueTag::String, L->intern(capture_string(ms, 0)));
      }
      TValue v = meta_index(L, repl_v, key);
      if (v.is_nil() || (v.is_bool() && !v.payload)) {
        out.append(ms.s.substr(ms.match_start, ms.match_end - ms.match_start));
      } else if (v.is_string() || v.is_number()) {
        out += value_to_string(v);
      } else {
        const char* tn = "nil";
        if (v.is_bool())
          tn = "boolean";
        else if (v.is_table())
          tn = "table";
        else if (v.is_function())
          tn = "function";
        else if (v.is_userdata())
          tn = "userdata";
        else if (v.is_thread())
          tn = "thread";
        panic(std::string("invalid replacement value (a ") + tn + ")");
      }
    } else {
      out += gsub_expand_repl(std::string(repl_v.is_string() ? repl_v.as_string()->view()
                                                             : value_to_string(repl_v)),
                              ms);
    }
  };

  while (count < max_s) {
    MatchState ms;
    ms.s = s;
    ms.p = pat_body;
    bool matched = match(ms, src, 0);
    // Anchor patterns only try once at the original start (handled by match on '^' skip).
    if (matched)
      ms.match_start = src;
    size_t e = matched ? ms.match_end : static_cast<size_t>(-2);
    if (matched && e != lastmatch) {
      ++count;
      apply_repl(ms);
      src = lastmatch = e;
    } else if (src < s.size()) {
      out += s[src++];
    } else {
      break;
    }
    if (anchor)
      break;
  }
  if (src < s.size())
    out.append(s.substr(src));
  L->settop(0);
  push_string(L, out);
  L->push(TValue::integer(count));
  return 2;
}

static UpVal* make_closed_upval(State* L, const TValue& v) {
  auto* uv = L->gc.create<UpVal>(GcKind::UpVal);
  uv->open = false;
  uv->closed = v;
  uv->thread = nullptr;
  uv->stack_index = -1;
  return uv;
}

// PUC-style gmatch: upvalues (s, pattern, src_pos, lastmatch_end).
static int gmatch_iter(State* L) {
  Closure* cl = L->current->frames.empty() ? nullptr : L->current->frames.back().cl;
  if (!cl || cl->upvals.size() < 4 || !cl->upvals[0] || !cl->upvals[1] || !cl->upvals[2] ||
      !cl->upvals[3]) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  TValue sv = cl->upvals[0]->get();
  TValue pv = cl->upvals[1]->get();
  TValue iv = cl->upvals[2]->get();
  TValue lv = cl->upvals[3]->get();
  if (!sv.is_string() || !pv.is_string()) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  std::string s(sv.as_string()->view());
  std::string_view pat = pv.as_string()->view();
  size_t src = iv.is_number() ? static_cast<size_t>(iv.to_number()) : 0;
  // lastmatch_end as size_t; -1 sentinel stored as integer -1.
  size_t lastmatch =
      (lv.is_int() && lv.as_int() < 0) ? static_cast<size_t>(-1) : static_cast<size_t>(lv.to_number());

  for (; src <= s.size(); ++src) {
    MatchState ms;
    ms.s = s;
    ms.p = pat;
    ms.match_start = src;
    if (!match(ms, src, 0))
      continue;
    size_t e = ms.match_end;
    if (e == lastmatch)
      continue;
    cl->upvals[2]->set(L, TValue::integer(static_cast<int64_t>(e)));
    cl->upvals[3]->set(L, TValue::integer(static_cast<int64_t>(e)));
    L->settop(0);
    if (ms.captures.empty()) {
      push_string(L, std::string(ms.s.substr(ms.match_start, ms.match_end - ms.match_start)));
    } else {
      for (size_t c = 0; c < ms.captures.size(); ++c) {
        auto [a, b] = ms.captures[c];
        if (b == kCapPosition)
          L->push(TValue::integer(static_cast<int64_t>(a + 1)));
        else
          push_string(L, capture_string(ms, c));
      }
    }
    return L->gettop();
  }
  L->settop(0);
  L->push(TValue::nil());
  return 1;
}

static int str_gmatch(State* L) {
  check_string(L, 1);
  check_string(L, 2);
  L->settop(2);
  Closure* cl = closure_new_c(L, gmatch_iter);
  cl->upvals.resize(4);
  cl->upvals[0] = make_closed_upval(L, *L->at(1));
  cl->upvals[1] = make_closed_upval(L, *L->at(2));
  cl->upvals[2] = make_closed_upval(L, TValue::integer(0));           // src
  cl->upvals[3] = make_closed_upval(L, TValue::integer(static_cast<int64_t>(-1))); // lastmatch
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, cl));
  return 1;
}

// --- string.pack / packsize / unpack (Lua 5.3) ---

constexpr int kMaxIntSize = 16;
constexpr char kPackPad = '\0';

struct PackHeader {
  bool islittle = true;
  int maxalign = 1;
};

enum class PackOpt { Int, UInt, Float, Char, String, ZStr, Padding, PadAlign, Nop };

static bool native_little() {
  const union {
    uint16_t u;
    uint8_t c[2];
  } x = {1};
  return x.c[0] == 1;
}

static int pack_maxalign() {
  struct CD {
    char c;
    union {
      double d;
      void* p;
      int64_t i;
    } u;
  };
  return static_cast<int>(offsetof(CD, u));
}

static int getnum(const char*& fmt, int df) {
  if (*fmt < '0' || *fmt > '9')
    return df;
  int a = 0;
  do {
    a = a * 10 + (*fmt++ - '0');
  } while (*fmt >= '0' && *fmt <= '9' && a <= (std::numeric_limits<int>::max() - 9) / 10);
  return a;
}

static int getnumlimit(const char*& fmt, int df) {
  int sz = getnum(fmt, df);
  if (sz > kMaxIntSize || sz <= 0)
    panic("integral size (" + std::to_string(sz) + ") out of limits [1,16]");
  return sz;
}

static PackOpt getoption(PackHeader& h, const char*& fmt, int& size) {
  int opt = static_cast<unsigned char>(*fmt++);
  size = 0;
  switch (opt) {
  case 'b': size = 1; return PackOpt::Int;
  case 'B': size = 1; return PackOpt::UInt;
  case 'h': size = static_cast<int>(sizeof(short)); return PackOpt::Int;
  case 'H': size = static_cast<int>(sizeof(short)); return PackOpt::UInt;
  case 'l': size = static_cast<int>(sizeof(long)); return PackOpt::Int;
  case 'L': size = static_cast<int>(sizeof(long)); return PackOpt::UInt;
  case 'j': size = static_cast<int>(sizeof(int64_t)); return PackOpt::Int;
  case 'J': size = static_cast<int>(sizeof(int64_t)); return PackOpt::UInt;
  case 'T': size = static_cast<int>(sizeof(size_t)); return PackOpt::UInt;
  case 'f': size = static_cast<int>(sizeof(float)); return PackOpt::Float;
  case 'd': size = static_cast<int>(sizeof(double)); return PackOpt::Float;
  case 'n': size = static_cast<int>(sizeof(double)); return PackOpt::Float;
  case 'i': size = getnumlimit(fmt, static_cast<int>(sizeof(int))); return PackOpt::Int;
  case 'I': size = getnumlimit(fmt, static_cast<int>(sizeof(int))); return PackOpt::UInt;
  case 's': size = getnumlimit(fmt, static_cast<int>(sizeof(size_t))); return PackOpt::String;
  case 'c':
    size = getnum(fmt, -1);
    if (size == -1)
      panic("missing size for format option 'c'");
    return PackOpt::Char;
  case 'z': return PackOpt::ZStr;
  case 'x': size = 1; return PackOpt::Padding;
  case 'X': return PackOpt::PadAlign;
  case ' ':
  case '\n':
  case '\t':
  case '\v':
  case '\f':
  case '\r':
    break; // ignore whitespace
  case '<': h.islittle = true; break;
  case '>': h.islittle = false; break;
  case '=': h.islittle = native_little(); break;
  case '!': h.maxalign = getnumlimit(fmt, pack_maxalign()); break;
  default: panic(std::string("invalid format option '") + static_cast<char>(opt) + "'");
  }
  return PackOpt::Nop;
}

static PackOpt getdetails(PackHeader& h, size_t totalsize, const char*& fmt, int& psize,
                          int& ntoalign) {
  PackOpt opt = getoption(h, fmt, psize);
  int align = psize;
  if (opt == PackOpt::PadAlign) {
    if (*fmt == '\0')
      panic("invalid next option for option 'X'");
    PackOpt next = getoption(h, fmt, align);
    if (next == PackOpt::Char || align == 0)
      panic("invalid next option for option 'X'");
  }
  if (align <= 1 || opt == PackOpt::Char) {
    ntoalign = 0;
  } else {
    if (align > h.maxalign)
      align = h.maxalign;
    if ((align & (align - 1)) != 0)
      panic("format asks for alignment not power of 2");
    ntoalign = (align - static_cast<int>(totalsize & static_cast<size_t>(align - 1))) & (align - 1);
  }
  return opt;
}

static void packint(std::string& out, uint64_t n, bool islittle, int size, bool neg) {
  size_t base = out.size();
  out.append(static_cast<size_t>(size), '\0');
  out[base + (islittle ? 0 : static_cast<size_t>(size - 1))] = static_cast<char>(n & 0xFF);
  for (int i = 1; i < size; ++i) {
    n >>= 8;
    out[base + (islittle ? static_cast<size_t>(i) : static_cast<size_t>(size - 1 - i))] =
        static_cast<char>(n & 0xFF);
  }
  if (neg && size > static_cast<int>(sizeof(int64_t))) {
    for (int i = static_cast<int>(sizeof(int64_t)); i < size; ++i)
      out[base + (islittle ? static_cast<size_t>(i) : static_cast<size_t>(size - 1 - i))] =
          static_cast<char>(0xFF);
  }
}

static void copy_endian(char* dest, const char* src, int size, bool islittle) {
  if (islittle == native_little()) {
    std::memcpy(dest, src, static_cast<size_t>(size));
  } else {
    for (int i = 0; i < size; ++i)
      dest[i] = src[size - 1 - i];
  }
}

static int64_t unpackint(const char* str, bool islittle, int size, bool issigned) {
  uint64_t res = 0;
  int limit = (size <= static_cast<int>(sizeof(int64_t))) ? size : static_cast<int>(sizeof(int64_t));
  for (int i = limit - 1; i >= 0; --i) {
    res <<= 8;
    res |= static_cast<uint64_t>(static_cast<unsigned char>(str[islittle ? i : size - 1 - i]));
  }
  if (size < static_cast<int>(sizeof(int64_t))) {
    if (issigned) {
      uint64_t mask = uint64_t{1} << (size * 8 - 1);
      res = (res ^ mask) - mask;
    }
  } else if (size > static_cast<int>(sizeof(int64_t))) {
    int mask = (!issigned || static_cast<int64_t>(res) >= 0) ? 0 : 0xFF;
    for (int i = limit; i < size; ++i) {
      if (static_cast<unsigned char>(str[islittle ? i : size - 1 - i]) != mask)
        panic(std::to_string(size) + "-byte integer does not fit into Lua Integer");
    }
  }
  return static_cast<int64_t>(res);
}

static int str_pack(State* L) {
  const char* fmt = check_string(L, 1)->view().data();
  PackHeader h;
  h.islittle = native_little();
  h.maxalign = 1;
  std::string out;
  size_t totalsize = 0;
  int arg = 1;
  while (*fmt != '\0') {
    int size = 0, ntoalign = 0;
    PackOpt opt = getdetails(h, totalsize, fmt, size, ntoalign);
    totalsize += static_cast<size_t>(ntoalign + size);
    while (ntoalign-- > 0)
      out.push_back(kPackPad);
    ++arg;
    switch (opt) {
    case PackOpt::Int: {
      int64_t n = check_int(L, arg);
      if (size < static_cast<int>(sizeof(int64_t))) {
        int64_t lim = int64_t{1} << (size * 8 - 1);
        if (n < -lim || n >= lim)
          panic("integer overflow");
      }
      packint(out, static_cast<uint64_t>(n), h.islittle, size, n < 0);
      break;
    }
    case PackOpt::UInt: {
      int64_t n = check_int(L, arg);
      if (size < static_cast<int>(sizeof(int64_t))) {
        uint64_t umax = (uint64_t{1} << (size * 8)) - 1;
        if (static_cast<uint64_t>(n) > umax)
          panic("unsigned overflow");
      }
      packint(out, static_cast<uint64_t>(n), h.islittle, size, false);
      break;
    }
    case PackOpt::Float: {
      double n = check_number(L, arg);
      char buff[16]{};
      if (size == static_cast<int>(sizeof(float))) {
        float f = static_cast<float>(n);
        std::memcpy(buff, &f, sizeof(f));
      } else {
        std::memcpy(buff, &n, sizeof(n));
      }
      size_t base = out.size();
      out.append(static_cast<size_t>(size), '\0');
      copy_endian(out.data() + base, buff, size, h.islittle);
      break;
    }
    case PackOpt::Char: {
      std::string_view s = check_string(L, arg)->view();
      if (s.size() > static_cast<size_t>(size))
        panic("string longer than given size");
      out.append(s);
      out.append(static_cast<size_t>(size) - s.size(), kPackPad);
      break;
    }
    case PackOpt::String: {
      std::string_view s = check_string(L, arg)->view();
      if (size < static_cast<int>(sizeof(size_t)) &&
          s.size() >= (size_t{1} << (size * 8)))
        panic("string length does not fit in given size");
      packint(out, static_cast<uint64_t>(s.size()), h.islittle, size, false);
      out.append(s);
      totalsize += s.size();
      break;
    }
    case PackOpt::ZStr: {
      std::string_view s = check_string(L, arg)->view();
      if (s.find('\0') != std::string_view::npos)
        panic("string contains zeros");
      out.append(s);
      out.push_back('\0');
      totalsize += s.size() + 1;
      break;
    }
    case PackOpt::Padding:
      out.push_back(kPackPad);
      // fallthrough
    case PackOpt::PadAlign:
    case PackOpt::Nop:
      --arg;
      break;
    }
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int str_packsize(State* L) {
  const char* fmt = check_string(L, 1)->view().data();
  PackHeader h;
  h.islittle = native_little();
  h.maxalign = 1;
  size_t totalsize = 0;
  // PUC MAXSIZE: largest size_t that also fits in a Lua integer / int (use INT_MAX).
  constexpr size_t kMaxSize = static_cast<size_t>(std::numeric_limits<int>::max());
  while (*fmt != '\0') {
    int size = 0, ntoalign = 0;
    PackOpt opt = getdetails(h, totalsize, fmt, size, ntoalign);
    size += ntoalign;
    if (static_cast<size_t>(size) > kMaxSize || totalsize > kMaxSize - static_cast<size_t>(size))
      panic("format result too large");
    totalsize += static_cast<size_t>(size);
    if (opt == PackOpt::String || opt == PackOpt::ZStr)
      panic("variable-length format");
  }
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(totalsize)));
  return 1;
}

static int str_unpack(State* L) {
  const char* fmt = check_string(L, 1)->view().data();
  std::string_view data = check_string(L, 2)->view();
  // PUC Lua 5.3 posrelat: pos==0 and out-of-range negatives become 0, then
  // (size_t)0-1 underflows so the "initial position out of string" check fires.
  int64_t ipos = L->gettop() >= 3 ? check_int(L, 3) : 1;
  if (ipos < 0) {
    if (static_cast<size_t>(-ipos) > data.size())
      ipos = 0;
    else
      ipos = static_cast<int64_t>(data.size()) + ipos + 1;
  }
  size_t pos = static_cast<size_t>(ipos) - 1;
  if (pos > data.size())
    panic("initial position out of string");
  PackHeader h;
  h.islittle = native_little();
  h.maxalign = 1;
  L->settop(0);
  int n = 0;
  while (*fmt != '\0') {
    int size = 0, ntoalign = 0;
    PackOpt opt = getdetails(h, pos, fmt, size, ntoalign);
    if (pos + static_cast<size_t>(ntoalign) + static_cast<size_t>(size) > data.size())
      panic("data string too short");
    pos += static_cast<size_t>(ntoalign);
    ++n;
    switch (opt) {
    case PackOpt::Int:
    case PackOpt::UInt: {
      int64_t res = unpackint(data.data() + pos, h.islittle, size, opt == PackOpt::Int);
      L->push(TValue::integer(res));
      break;
    }
    case PackOpt::Float: {
      char buff[16]{};
      copy_endian(buff, data.data() + pos, size, h.islittle);
      if (size == static_cast<int>(sizeof(float))) {
        float f;
        std::memcpy(&f, buff, sizeof(f));
        L->push(TValue::number(static_cast<double>(f)));
      } else {
        double d;
        std::memcpy(&d, buff, sizeof(d));
        L->push(TValue::number(d));
      }
      break;
    }
    case PackOpt::Char:
      push_string(L, data.substr(pos, static_cast<size_t>(size)));
      break;
    case PackOpt::String: {
      size_t len = static_cast<size_t>(unpackint(data.data() + pos, h.islittle, size, false));
      if (pos + static_cast<size_t>(size) + len > data.size())
        panic("data string too short");
      push_string(L, data.substr(pos + static_cast<size_t>(size), len));
      pos += len;
      break;
    }
    case PackOpt::ZStr: {
      size_t len = std::strlen(data.data() + pos);
      push_string(L, data.substr(pos, len));
      pos += len + 1;
      break;
    }
    case PackOpt::PadAlign:
    case PackOpt::Padding:
    case PackOpt::Nop:
      --n;
      break;
    }
    pos += static_cast<size_t>(size);
  }
  L->push(TValue::integer(static_cast<int64_t>(pos + 1)));
  return n + 1;
}

void open_string_lib(State* L) {
  Table* str = new_lib(L, 32);
  set_field(L, str, "len", str_len);
  set_field(L, str, "sub", str_sub);
  set_field(L, str, "rep", str_rep);
  set_field(L, str, "reverse", str_reverse);
  set_field(L, str, "lower", str_lower);
  set_field(L, str, "upper", str_upper);
  set_field(L, str, "byte", str_byte);
  set_field(L, str, "char", str_char);
  set_field(L, str, "format", str_format);
  set_field(L, str, "dump", str_dump);
  set_field(L, str, "find", str_find);
  set_field(L, str, "match", str_match);
  set_field(L, str, "gsub", str_gsub);
  set_field(L, str, "gmatch", str_gmatch);
  set_field(L, str, "pack", str_pack);
  set_field(L, str, "unpack", str_unpack);
  set_field(L, str, "packsize", str_packsize);
  set_global_value(L, "string", TValue::obj(ValueTag::Table, str));

  // PUC createmetatable: string methods via ("..."):sub(...)
  Table* mt = table_new(L, 0, 1);
  mt->set(L, TValue::obj(ValueTag::String, L->intern("__index")),
          TValue::obj(ValueTag::Table, str));
  L->type_mt[static_cast<size_t>(ValueTag::String)] = mt;
}

} // namespace luatier
