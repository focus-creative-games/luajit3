#include "runtime/value.hpp"

#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/userdata.hpp"

#include <cctype>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace luatier {

static bool is_nan_or_inf_token(const std::string& s) {
  size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-'))
    ++i;
  if (i + 3 == s.size()) {
    char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    char b = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + 1])));
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + 2])));
    if (a == 'n' && b == 'a' && c == 'n')
      return true;
    if (a == 'i' && b == 'n' && c == 'f')
      return true;
  }
  return false;
}

static char locale_decpoint() {
  const lconv* lc = std::localeconv();
  if (lc && lc->decimal_point && lc->decimal_point[0])
    return lc->decimal_point[0];
  return '.';
}

// PUC l_str2d: accept '.' and the locale decimal point.
static bool str2d(const std::string& s, double* out) {
  if (s.empty() || is_nan_or_inf_token(s))
    return false;
  char* end = nullptr;
  const char* start = s.c_str();
  double d = std::strtod(start, &end);
  if (end && end != start && *end == '\0') {
    *out = d;
    return true;
  }
  char dec = locale_decpoint();
  if (dec != '.' && s.find('.') != std::string::npos) {
    std::string t = s;
    for (char& ch : t) {
      if (ch == '.')
        ch = dec;
    }
    end = nullptr;
    d = std::strtod(t.c_str(), &end);
    if (end && end != t.c_str() && *end == '\0') {
      *out = d;
      return true;
    }
  }
  return false;
}

bool try_to_number(const TValue& v, TValue* out) {
  if (v.is_number()) {
    *out = v;
    return true;
  }
  if (!v.is_string())
    return false;
  std::string s(v.as_string()->view());
  // Trim spaces like PUC luaO_str2num.
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  s = s.substr(a, b - a);
  if (s.empty() || is_nan_or_inf_token(s))
    return false;

  char dec = locale_decpoint();
  const bool has_frac = s.find('.') != std::string::npos || s.find(dec) != std::string::npos ||
                        s.find('e') != std::string::npos || s.find('E') != std::string::npos ||
                        s.find('p') != std::string::npos || s.find('P') != std::string::npos;
  if (!has_frac) {
    size_t hs = 0;
    int sign = 1;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
      if (s[0] == '-')
        sign = -1;
      hs = 1;
    }
    const bool hex = s.size() > hs + 2 && s[hs] == '0' && (s[hs + 1] == 'x' || s[hs + 1] == 'X');
    if (hex) {
      uint64_t u = 0;
      size_t i = hs + 2;
      for (; i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i])); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int d = std::isdigit(c) ? (c - '0') : (std::tolower(c) - 'a' + 10);
        u = (u << 4) + static_cast<uint64_t>(d);
      }
      if (i == s.size()) {
        *out = TValue::integer(static_cast<int64_t>(sign < 0 ? (0u - u) : u));
        return true;
      }
    } else {
      try {
        size_t idx = 0;
        long long n = std::stoll(s, &idx, 10);
        if (idx == s.size()) {
          *out = TValue::integer(static_cast<int64_t>(n));
          return true;
        }
      } catch (...) {
      }
    }
  }
  double d = 0;
  if (str2d(s, &d)) {
    *out = TValue::number(d);
    return true;
  }
  // Hex float: 0x1.5p+3 / 0x.41 (and locale decimal in the mantissa).
  size_t hs = 0;
  int sign = 1;
  if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
    if (s[0] == '-')
      sign = -1;
    hs = 1;
  }
  if (s.size() > hs + 2 && s[hs] == '0' && (s[hs + 1] == 'x' || s[hs + 1] == 'X')) {
    std::string t = s;
    if (dec != '.') {
      for (size_t i = hs + 2; i < t.size(); ++i) {
        if (t[i] == dec)
          t[i] = '.';
      }
    }
    char* end = nullptr;
    d = std::strtod(t.c_str(), &end);
    if (end && end != t.c_str() && *end == '\0') {
      *out = TValue::number(d);
      return true;
    }
    (void)sign;
  }
  return false;
}

bool values_equal(const TValue& a, const TValue& b) {
  if (a.tag() != b.tag()) {
    if (a.is_number() && b.is_number())
      return a.to_number() == b.to_number(); // NaN != NaN via IEEE
    return false;
  }
  switch (a.tag()) {
  case ValueTag::Nil:
    return true;
  case ValueTag::Bool:
  case ValueTag::Int:
  case ValueTag::LightUserdata:
    return a.payload == b.payload;
  case ValueTag::Float: {
    double x = a.as_float(), y = b.as_float();
    return x == y; // IEEE: NaN != NaN
  }
  case ValueTag::String:
    return a.as_string()->view() == b.as_string()->view();
  default:
    return a.payload == b.payload;
  }
}

std::string value_to_string(const TValue& v) {
  switch (v.tag()) {
  case ValueTag::Nil:
    return "nil";
  case ValueTag::Bool:
    return v.payload ? "true" : "false";
  case ValueTag::Int:
    return std::to_string(v.as_int());
  case ValueTag::Float: {
    // PUC LUA_NUMBER_FMT ("%.14g"): enough digits for round-trip, no trailing noise.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.14g", v.as_float());
    return buf;
  }
  case ValueTag::String:
    return std::string(v.as_string()->view());
  case ValueTag::Table:
  case ValueTag::Function:
  case ValueTag::Thread:
  case ValueTag::Userdata:
  case ValueTag::LightUserdata: {
    // PUC luaO_pushfstring / tostring: "type: %p"
    const char* tname = "value";
    const void* p = nullptr;
    switch (v.tag()) {
    case ValueTag::Table:
      tname = "table";
      p = v.as_table();
      break;
    case ValueTag::Function:
      tname = "function";
      p = v.as_closure();
      break;
    case ValueTag::Thread:
      tname = "thread";
      p = v.as_thread();
      break;
    case ValueTag::Userdata:
      tname = "userdata";
      p = v.as_userdata();
      break;
    case ValueTag::LightUserdata:
      tname = "userdata";
      p = reinterpret_cast<void*>(static_cast<uintptr_t>(v.payload));
      break;
    default:
      break;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s: %p", tname, p);
    return buf;
  }
  default:
    return "value";
  }
}

} // namespace luatier
