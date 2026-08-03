#pragma once

#include "runtime/value.hpp"

#include <cstddef>
#include <vector>

namespace luatier {

enum class GcColor : uint8_t { White0 = 0, White1 = 1, Gray = 2, Black = 3 };

enum GcFlags : uint16_t {
  GcFlagFinalized = 1u << 0,
};

struct GcObject {
  GcObject* next = nullptr;
  GcObject* gray_next = nullptr;
  uint8_t kind = 0;
  uint8_t mark = static_cast<uint8_t>(GcColor::White0);
  uint16_t flags = 0;
};

enum class GcKind : uint8_t {
  String = 1,
  Table,
  Proto,
  Closure,
  UpVal,
  Userdata,
  Thread,
};

struct State;

class GC {
public:
  explicit GC(State* L);
  ~GC();

  void link(GcObject* obj, GcKind kind, size_t size_bytes);

  template <typename T>
  T* create(GcKind kind) {
    auto* obj = new T();
    link(obj, kind, sizeof(T));
    return obj;
  }

  void mark_object(GcObject* o);
  void mark_value(const TValue& v);
  void barrier(GcObject* parent, const TValue& child);
  void step();
  void full_gc();
  void maybe_step();
  void safepoint();
  void set_running(bool running);
  bool is_running() const;

  enum class Phase { Pause, Propagate, Sweep };
  Phase phase() const { return phase_; }

  bool stress_every_safepoint = false;
  bool running_finalizer_ = false;
  int64_t debt_ = 0;
  int64_t threshold_ = 1024 * 64;

private:
  bool collecting_ = false;
  uint8_t current_white() const { return white_; }
  void mark_roots();
  void propagate_one();
  void sweep();
  void clear_weak_tables(uint8_t mask = 3);
  void converge_ephemerons();
  void run_finalizers();

  std::vector<GcObject*> finalize_;

  State* L_;
  GcObject* all_ = nullptr;
  GcObject* gray_ = nullptr;
  uint8_t white_ = static_cast<uint8_t>(GcColor::White0);
  Phase phase_ = Phase::Pause;
  bool gc_isrunning_ = true;
};

} // namespace luatier
