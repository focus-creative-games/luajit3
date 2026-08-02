#include "runtime/value.hpp"

#include "runtime/string.hpp"

#include <sstream>
#include <string>

namespace lj3 {

bool try_to_number(const TValue& v, TValue* out) {
  if (v.is_number()) {
    *out = v;
    return true;
  }
  if (!v.is_string())
    return false;
  try {
    std::string s(v.as_string()->view());
    size_t idx = 0;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
      long long n = std::stoll(s, &idx, 0);
      if (idx == s.size()) {
        *out = TValue::integer(static_cast<int64_t>(n));
        return true;
      }
    }
    double d = std::stod(s, &idx);
    if (idx == s.size()) {
      *out = TValue::number(d);
      return true;
    }
  } catch (...) {
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
    std::ostringstream os;
    os << v.as_float();
    return os.str();
  }
  case ValueTag::String:
    return std::string(v.as_string()->view());
  case ValueTag::Table:
    return "table";
  case ValueTag::Function:
    return "function";
  case ValueTag::Thread:
    return "thread";
  case ValueTag::Userdata:
    return "userdata";
  default:
    return "value";
  }
}

} // namespace lj3
