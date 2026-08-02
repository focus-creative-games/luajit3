#include "lib/libs.hpp"

#include "lib/ldump.hpp"
#include "lib/lib_util.hpp"
#include "runtime/closure.hpp"
#include "vm/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace lj3 {
using namespace lib;

static int str_len(State* L) {
  size_t n = check_string(L, 1)->len;
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(n)));
  return 1;
}

static int str_sub(State* L) {
  std::string_view s = check_string(L, 1)->view();
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
  std::string s(check_string(L, 1)->view());
  int64_t n = check_int(L, 2);
  std::string sep = L->gettop() >= 3 ? std::string(check_string(L, 3)->view()) : "";
  if (n <= 0) {
    L->settop(0);
    push_string(L, "");
    return 1;
  }
  std::string out;
  for (int64_t i = 0; i < n; ++i) {
    if (i)
      out += sep;
    out += s;
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
  int64_t i = L->gettop() >= 2 ? check_int(L, 2) : 1;
  int64_t j = L->gettop() >= 3 ? check_int(L, 3) : static_cast<int64_t>(s.size());
  if (i < 0)
    i = static_cast<int64_t>(s.size()) + i + 1;
  if (j < 0)
    j = static_cast<int64_t>(s.size()) + j + 1;
  i = std::max<int64_t>(1, i);
  j = std::min<int64_t>(static_cast<int64_t>(s.size()), j);
  L->settop(0);
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
  std::string q = "\"";
  for (unsigned char c : s) {
    if (c == '\\' || c == '\"' || c == '\n' || c == '\r') {
      q += '\\';
      if (c == '\n')
        q += 'n';
      else if (c == '\r')
        q += 'r';
      else
        q += static_cast<char>(c);
    } else if (std::isprint(c)) {
      q += static_cast<char>(c);
    } else {
      q += '\\';
      if (c >= 100)
        q += static_cast<char>('0' + (c / 100));
      if (c >= 10)
        q += static_cast<char>('0' + ((c / 10) % 10));
      q += static_cast<char>('0' + (c % 10));
    }
  }
  q += '"';
  return q;
}

static bool format_is_flag(char c) {
  return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0';
}

static bool format_is_conv(char c) {
  return c == 'd' || c == 'i' || c == 'u' || c == 'o' || c == 'x' || c == 'X' || c == 'e' ||
         c == 'E' || c == 'f' || c == 'F' || c == 'g' || c == 'G' || c == 'a' || c == 'A' ||
         c == 'c' || c == 's' || c == 'p' || c == 'q';
}

static std::string format_build(const char* spec_start, const char* spec_end, State* L, int& argi) {
  std::string fmt = "%";
  for (const char* q = spec_start + 1; q < spec_end - 1; ++q) {
    if (*q == '*') {
      int64_t v = check_int(L, argi++);
      fmt += std::to_string(v);
    } else {
      fmt += *q;
    }
  }
  fmt += spec_end[-1];
  return fmt;
}

static std::string format_one(const char*& p, const char* end, State* L, int& argi) {
  const char* spec_start = p;
  ++p;
  if (p >= end)
    return "%";
  if (*p == '%') {
    ++p;
    return "%";
  }

  while (p < end && format_is_flag(*p))
    ++p;
  if (p < end && *p == '*')
    ++p;
  else
    while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
      ++p;
  if (p < end && *p == '.') {
    ++p;
    if (p < end && *p == '*')
      ++p;
    else
      while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
        ++p;
  }
  while (p < end && (*p == 'h' || *p == 'l' || *p == 'L'))
    ++p;

  if (p >= end || !format_is_conv(*p))
    return std::string(spec_start, p - spec_start);

  ++p;
  const char* spec_end = p;
  char conv = spec_end[-1];

  if (conv == 'q') {
    std::string s = value_to_string(*L->at(argi++));
    return format_quote(s);
  }

  std::string fmt = format_build(spec_start, spec_end, L, argi);
  const TValue* val = L->at(argi++);
  char buf[512];

  switch (conv) {
  case 's': {
    std::string s = value_to_string(*val);
    std::snprintf(buf, sizeof(buf), fmt.c_str(), s.c_str());
    return buf;
  }
  case 'c': {
    int ch = val->is_number() ? static_cast<int>(val->to_number()) & 0xFF : 0;
    std::snprintf(buf, sizeof(buf), fmt.c_str(), ch);
    return buf;
  }
  case 'd':
  case 'i':
    std::snprintf(buf, sizeof(buf), fmt.c_str(),
                  static_cast<long long>(val->is_number() ? static_cast<int64_t>(val->to_number()) : 0));
    return buf;
  case 'u':
  case 'o':
  case 'x':
  case 'X':
    std::snprintf(buf, sizeof(buf), fmt.c_str(),
                  static_cast<unsigned long long>(val->is_number()
                                                      ? static_cast<uint64_t>(val->to_number())
                                                      : 0));
    return buf;
  case 'f':
  case 'F':
  case 'e':
  case 'E':
  case 'g':
  case 'G':
  case 'a':
  case 'A':
    std::snprintf(buf, sizeof(buf), fmt.c_str(), val->is_number() ? val->to_number() : 0.0);
    return buf;
  case 'p':
    std::snprintf(buf, sizeof(buf), fmt.c_str(), static_cast<void*>(nullptr));
    return buf;
  default:
    return std::string(spec_start, spec_end - spec_start);
  }
}

static int str_format(State* L) {
  std::string_view fmt = check_string(L, 1)->view();
  int argi = 2;
  std::string out;
  const char* p = fmt.data();
  const char* end = p + fmt.size();
  while (p < end) {
    if (*p == '%') {
      out += format_one(p, end, L, argi);
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

struct MatchState {
  std::string_view s;
  std::string_view p;
  size_t match_start = 0;
  size_t match_end = 0;
  std::vector<std::pair<size_t, size_t>> captures;
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

// Advance past one pattern item; returns whether char `c` matches. `pi` is the
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
    size_t j = pi + 1;
    bool negate = j < ms.p.size() && ms.p[j] == '^';
    if (negate)
      ++j;
    bool found = false;
    if (si < ms.s.size()) {
      char c = ms.s[si];
      while (j < ms.p.size() && ms.p[j] != ']') {
        if (ms.p[j] == '%' && j + 1 < ms.p.size()) {
          if (match_class(c, ms.p[j + 1]))
            found = true;
          j += 2;
          continue;
        }
        char lo = ms.p[j++];
        char hi = lo;
        if (j < ms.p.size() && ms.p[j] == '-' && j + 1 < ms.p.size() && ms.p[j + 1] != ']') {
          hi = ms.p[j + 1];
          j += 2;
        }
        if (lo <= c && c <= hi)
          found = true;
      }
    }
    if (j >= ms.p.size() || ms.p[j] != ']')
      return false;
    *out_pi = j + 1;
    return si < ms.s.size() && (negate ? !found : found);
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
  return false;
}

static bool match_capture(MatchState& ms, size_t si, int cap) {
  if (cap < 0 || cap >= static_cast<int>(ms.captures.size()))
    return false;
  auto [a, b] = ms.captures[static_cast<size_t>(cap)];
  if (b == kCapUnfinished || b == kCapPosition)
    return false;
  size_t len = b - a;
  if (si + len > ms.s.size())
    return false;
  return ms.s.substr(si, len) == ms.s.substr(a, len);
}

static bool match_balance(MatchState& ms, size_t si, size_t pi) {
  if (pi + 1 >= ms.p.size())
    return false;
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
      return false;
    char spec = ms.p[pi + 1];
    if (spec == 'b')
      return match_balance(ms, si, pi + 2);
    if (spec == 'f') {
      if (pi + 2 >= ms.p.size() || ms.p[pi + 2] != '[')
        return false;
      size_t ep = 0;
      // frontier: previous char not in class, current in class
      bool prev = si > 0 && single_match(ms, si - 1, pi + 2, &ep);
      bool cur = single_match(ms, si, pi + 2, &ep);
      if (!cur || prev)
        return false;
      pi = ep;
      goto init;
    }
    if (std::isdigit(static_cast<unsigned char>(spec))) {
      int cap = spec - '0';
      if (!match_capture(ms, si, cap))
        return false;
      auto [a, b] = ms.captures[static_cast<size_t>(cap)];
      si += b - a;
      pi += 2;
      goto init;
    }
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
  if (b == kCapUnfinished || a > b)
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
  MatchState ms{s, p, 0, 0, {}};
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
  MatchState ms{s, p, 0, 0, {}};
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
  for (size_t i = 0; i < repl.size(); ++i) {
    if (repl[i] == '%' && i + 1 < repl.size()) {
      char c = repl[++i];
      if (c == '%') {
        out += '%';
      } else if (c == '0') {
        out.append(ms.s.substr(ms.match_start, ms.match_end - ms.match_start));
      } else if (c >= '1' && c <= '9') {
        size_t idx = static_cast<size_t>(c - '1');
        if (idx < ms.captures.size())
          out += capture_string(ms, idx);
      } else {
        out += '%';
        out += c;
      }
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
  int64_t n = L->gettop() >= 4 ? check_int(L, 4) : -1;
  // Keep repl alive across settop by not clearing until end; stash on stack bottom.
  std::string out;
  size_t i = 0;
  int count = 0;
  bool anchor = !pat.empty() && pat[0] == '^';
  while (i <= s.size() && (n < 0 || count < n)) {
    if (i == s.size() && count > 0)
      break;
    MatchState ms{s, pat, 0, 0, {}};
    if (!match_search(ms, i, anchor))
      break;
    out.append(s.substr(i, ms.match_start - i));
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
      if (!r.is_nil())
        out += value_to_string(r);
      L->settop(base);
    } else if (repl_v.is_table()) {
      std::string key = ms.captures.empty()
                            ? std::string(ms.s.substr(ms.match_start, ms.match_end - ms.match_start))
                            : capture_string(ms, 0);
      TValue v = repl_v.as_table()->get(TValue::obj(ValueTag::String, L->intern(key)));
      if (!v.is_nil())
        out += value_to_string(v);
    } else {
      out += gsub_expand_repl(std::string(repl_v.is_string() ? repl_v.as_string()->view()
                                                             : value_to_string(repl_v)),
                              ms);
    }
    size_t next = ms.match_end;
    if (next <= i)
      next = i + 1;
    i = next;
    ++count;
    if (anchor)
      break;
  }
  if (i < s.size())
    out.append(s.substr(i));
  L->settop(0);
  push_string(L, out);
  L->push(TValue::integer(count));
  return 2;
}

static int gmatch_iter(State* L) {
  // args: state(table {s, pat, pos}), _control (ignored; pos lives in state[3])
  if (!L->at(1)->is_table()) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  Table* st = L->at(1)->as_table();
  TValue sv = st->get_int(1);
  TValue pv = st->get_int(2);
  TValue iv = st->get_int(3);
  if (!sv.is_string() || !pv.is_string()) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  std::string s(sv.as_string()->view());
  std::string_view pat = pv.as_string()->view();
  int64_t i = iv.is_number() ? static_cast<int64_t>(iv.to_number()) : 0;
  if (i < 0)
    i = 0;
  MatchState ms{s, pat, 0, 0, {}};
  if (!match_search(ms, static_cast<size_t>(i), false)) {
    L->settop(0);
    L->push(TValue::nil());
    return 1;
  }
  size_t next = ms.match_end;
  if (next == ms.match_start)
    next = ms.match_start + 1;
  st->set(L, TValue::integer(3), TValue::integer(static_cast<int64_t>(next)));
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

static int str_gmatch(State* L) {
  std::string s(check_string(L, 1)->view());
  std::string_view pat = check_string(L, 2)->view();
  Table* st = L->gc.create<Table>(GcKind::Table);
  st->set(L, TValue::integer(1), TValue::obj(ValueTag::String, L->intern(s)));
  st->set(L, TValue::integer(2), TValue::obj(ValueTag::String, L->intern(std::string(pat))));
  st->set(L, TValue::integer(3), TValue::integer(0));
  L->settop(0);
  L->push(TValue::obj(ValueTag::Function, closure_new_c(L, gmatch_iter)));
  L->push(TValue::obj(ValueTag::Table, st));
  L->push(TValue::integer(0));
  return 3;
}

static int str_packsize(State* L) {
  std::string_view fmt = check_string(L, 1)->view();
  size_t sz = 0;
  for (char c : fmt) {
    if (c == 'j' || c == 'J')
      sz += 8;
    else if (c == 'n')
      sz += sizeof(double);
    else if (c == 'i' || c == 'I')
      sz += 4;
    else if (c == 'c')
      sz += 1;
  }
  L->settop(0);
  L->push(TValue::integer(static_cast<int64_t>(sz)));
  return 1;
}

static int str_pack(State* L) {
  std::string_view fmt = check_string(L, 1)->view();
  std::string out;
  int arg = 2;
  for (char c : fmt) {
    if (c == 'j' || c == 'J') {
      int64_t v = check_int(L, arg++);
      for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    } else if (c == 'n') {
      double v = check_number(L, arg++);
      uint64_t bits;
      std::memcpy(&bits, &v, sizeof(v));
      for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    }
  }
  L->settop(0);
  push_string(L, out);
  return 1;
}

static int str_unpack(State* L) {
  std::string_view fmt = check_string(L, 1)->view();
  std::string_view data = check_string(L, 2)->view();
  size_t pos = L->gettop() >= 3 ? static_cast<size_t>(check_int(L, 3) - 1) : 0;
  L->settop(0);
  for (char c : fmt) {
    if (c == 'j' || c == 'J') {
      int64_t v = 0;
      for (int i = 0; i < 8 && pos < data.size(); ++i)
        v |= static_cast<int64_t>(static_cast<unsigned char>(data[pos++])) << (i * 8);
      L->push(TValue::integer(v));
    } else if (c == 'n') {
      uint64_t bits = 0;
      for (int i = 0; i < 8 && pos < data.size(); ++i)
        bits |= static_cast<uint64_t>(static_cast<unsigned char>(data[pos++])) << (i * 8);
      double v;
      std::memcpy(&v, &bits, sizeof(v));
      L->push(TValue::number(v));
    }
  }
  L->push(TValue::integer(static_cast<int64_t>(pos + 1)));
  return L->gettop();
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
}

} // namespace lj3
