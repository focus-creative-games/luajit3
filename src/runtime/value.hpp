#pragma once

#include "common/common.hpp"

#include <cmath>
#include <cstring>

namespace luatier {

enum class ValueTag : uint32_t {
  Nil = 0,
  Bool,
  Int,
  Float,
  String,
  Table,
  Function,
  Userdata,
  Thread,
  LightUserdata,
  Internal,
};

struct GcObject;
struct LjString;
struct Table;
struct Closure;
struct UpVal;
struct Proto;
struct Thread;
struct Userdata;

struct TValue {
  uint64_t payload = 0;
  uint32_t type = static_cast<uint32_t>(ValueTag::Nil);
  uint32_t aux = 0;

  ValueTag tag() const { return static_cast<ValueTag>(type); }
  bool is_nil() const { return tag() == ValueTag::Nil; }
  bool is_bool() const { return tag() == ValueTag::Bool; }
  bool is_int() const { return tag() == ValueTag::Int; }
  bool is_float() const { return tag() == ValueTag::Float; }
  bool is_number() const { return is_int() || is_float(); }
  bool is_string() const { return tag() == ValueTag::String; }
  bool is_table() const { return tag() == ValueTag::Table; }
  bool is_function() const { return tag() == ValueTag::Function; }
  bool is_userdata() const { return tag() == ValueTag::Userdata; }
  bool is_thread() const { return tag() == ValueTag::Thread; }
  bool is_truthy() const { return !(is_nil() || (is_bool() && payload == 0)); }

  static TValue nil() { return {}; }
  static TValue boolean(bool b) {
    TValue v;
    v.type = static_cast<uint32_t>(ValueTag::Bool);
    v.payload = b ? 1 : 0;
    return v;
  }
  static TValue integer(int64_t n) {
    TValue v;
    v.type = static_cast<uint32_t>(ValueTag::Int);
    std::memcpy(&v.payload, &n, sizeof(n));
    return v;
  }
  static TValue number(double n) {
    TValue v;
    v.type = static_cast<uint32_t>(ValueTag::Float);
    std::memcpy(&v.payload, &n, sizeof(n));
    return v;
  }
  static TValue obj(ValueTag t, GcObject* o) {
    TValue v;
    v.type = static_cast<uint32_t>(t);
    v.payload = reinterpret_cast<uint64_t>(o);
    return v;
  }

  int64_t as_int() const {
    int64_t n;
    std::memcpy(&n, &payload, sizeof(n));
    return n;
  }
  double as_float() const {
    double n;
    std::memcpy(&n, &payload, sizeof(n));
    return n;
  }
  double to_number() const {
    if (is_int())
      return static_cast<double>(as_int());
    if (is_float())
      return as_float();
    panic("value is not a number");
  }
  GcObject* as_gc() const { return reinterpret_cast<GcObject*>(payload); }
  LjString* as_string() const { return reinterpret_cast<LjString*>(payload); }
  Table* as_table() const { return reinterpret_cast<Table*>(payload); }
  Closure* as_closure() const { return reinterpret_cast<Closure*>(payload); }
  Thread* as_thread() const { return reinterpret_cast<Thread*>(payload); }
  Userdata* as_userdata() const { return reinterpret_cast<Userdata*>(payload); }
};

bool values_equal(const TValue& a, const TValue& b);
std::string value_to_string(const TValue& v);

// Parse strings the same way as tonumber; returns false for non-number strings.
bool try_to_number(const TValue& v, TValue* out);

} // namespace luatier
