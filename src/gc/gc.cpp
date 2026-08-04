#include "gc/gc.hpp"

#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/upvalue.hpp"
#include "runtime/userdata.hpp"
#include "vm/interpreter.hpp"
#include "vm/meta.hpp"
#include "vm/state.hpp"

#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace luatier {

GC::GC(State* L) : L_(L) {
  const char* stress = std::getenv("LUATIER_STRESS_GC_EVERY_SAFEPOINT");
  stress_every_safepoint = stress && stress[0] == '1';
}

GC::~GC() {
  GcObject* o = all_;
  while (o) {
    GcObject* n = o->next;
    switch (static_cast<GcKind>(o->kind)) {
    case GcKind::String:
      std::free(o);
      break;
    case GcKind::Table:
      delete static_cast<Table*>(o);
      break;
    case GcKind::Proto:
      delete static_cast<Proto*>(o);
      break;
    case GcKind::Closure:
      delete static_cast<Closure*>(o);
      break;
    case GcKind::UpVal:
      delete static_cast<UpVal*>(o);
      break;
    case GcKind::Thread:
      delete static_cast<Thread*>(o);
      break;
    case GcKind::Userdata:
      delete static_cast<Userdata*>(o);
      break;
    default:
      std::free(o);
      break;
    }
    o = n;
  }
}

void GC::link(GcObject* obj, GcKind kind, size_t size_bytes) {
  obj->kind = static_cast<uint8_t>(kind);
  obj->mark = current_white();
  obj->next = all_;
  all_ = obj;
  debt_ += static_cast<int64_t>(size_bytes);
  // Do not collect here: the new object is not rooted yet.
}

void GC::mark_object(GcObject* o) {
  if (!o)
    return;
  if (o->mark == static_cast<uint8_t>(GcColor::Black) ||
      o->mark == static_cast<uint8_t>(GcColor::Gray))
    return;
  o->mark = static_cast<uint8_t>(GcColor::Gray);
  o->gray_next = gray_;
  gray_ = o;
}

void GC::mark_value(const TValue& v) {
  switch (v.tag()) {
  case ValueTag::String:
  case ValueTag::Table:
  case ValueTag::Function:
  case ValueTag::Userdata:
  case ValueTag::Thread:
    mark_object(v.as_gc());
    break;
  default:
    break;
  }
}

void GC::barrier(GcObject* parent, const TValue& child) {
  if (!parent)
    return;
  if (parent->mark == static_cast<uint8_t>(GcColor::Black))
    mark_value(child);
}

static int lua_frame_high_water(const Thread* th) {
  int limit = 0;
  for (auto& fr : th->frames) {
    if (fr.proto)
      limit = std::max(limit, fr.base + fr.proto->maxstack);
    else if (fr.cl && fr.cl->proto)
      limit = std::max(limit, fr.base + fr.cl->proto->maxstack);
  }
  return limit;
}

static void mark_thread_stack(GC* gc, Thread* th, bool strict_c_top) {
  // For the *current* thread inside a C call (stack_base != 0), mark only
  // below top — same as PUC. Dead temps above the C window must not pin a
  // weakly-held __gc (gc.lua: __gc x weak tables).
  // Suspended threads (e.g. main while a coroutine runs) still need their
  // full Lua register windows: resume leaves main->top at the C window, but
  // locals such as `threads`/`fn` remain live below ci->top.
  int limit = th->top;
  if (!(strict_c_top && th->stack_base != 0))
    limit = std::max(limit, lua_frame_high_water(th));
  limit = std::min(limit, static_cast<int>(th->stack.size()));
  for (int i = 0; i < limit; ++i)
    gc->mark_value(th->stack[static_cast<size_t>(i)]);
}

static void clear_dead_stack(Thread* th, bool strict_c_top) {
  int start = th->top;
  if (!(strict_c_top && th->stack_base != 0))
    start = std::max(start, lua_frame_high_water(th));
  int n = static_cast<int>(th->stack.size());
  for (int i = start; i < n; ++i)
    th->stack[static_cast<size_t>(i)] = TValue::nil();
}

void GC::mark_roots() {
  if (L_->globals)
    mark_object(L_->globals);
  if (L_->registry)
    mark_object(L_->registry);
  for (Table* mt : L_->type_mt) {
    if (mt)
      mark_object(mt);
  }
  for (auto* th : {L_->main, L_->current}) {
    if (!th)
      continue;
    mark_object(th);
    mark_thread_stack(this, th, /*strict_c_top=*/th == L_->current);
    if (th->hook.func)
      mark_object(th->hook.func);
    for (auto& fr : th->frames) {
      if (fr.cl)
        mark_object(fr.cl);
      if (fr.proto)
        mark_object(fr.proto);
      for (auto& v : fr.varargs)
        mark_value(v);
    }
    for (UpVal* uv = th->open_upvals; uv; uv = uv->next_open)
      mark_object(uv);
  }
  // Intern table is a strong root (all live Lua strings are reachable here).
  // A PUC-style weak string table needs tighter atomic/ephemeron integration;
  // keep strings immortal via the intern set until State teardown.
  for (LjString* head : L_->strings.hash) {
    for (LjString* s = head; s; s = s->hnext)
      mark_object(s);
  }
  for (LjString* s : L_->tm_names) {
    if (s)
      mark_object(s);
  }
}

void GC::propagate_one() {
  if (!gray_)
    return;
  GcObject* o = gray_;
  gray_ = o->gray_next;
  o->mark = static_cast<uint8_t>(GcColor::Black);
  switch (static_cast<GcKind>(o->kind)) {
  case GcKind::String:
    break;
  case GcKind::Table: {
    auto* t = static_cast<Table*>(o);
    if (t->metatable)
      mark_object(t->metatable);
    uint8_t weak = t->weak_mode;
    // Array keys are strong integers; mark values unless values are weak.
    for (auto& v : t->array) {
      if (!(weak & 2))
        mark_value(v);
    }
    for (auto& n : t->hash) {
      if (!n.used)
        continue;
      if (!(weak & 1))
        mark_value(n.key);
      // With weak keys, defer value marking to converge_ephemerons() so a
      // value cannot keep its own (or another) weak key alive incorrectly.
      if (!(weak & 2) && !(weak & 1))
        mark_value(n.value);
    }
    break;
  }
  case GcKind::Proto: {
    auto* p = static_cast<Proto*>(o);
    // PUC traverseproto: a white cache must not keep the closure alive.
    if (p->cache && p->cache->mark == white_)
      p->cache = nullptr;
    for (auto& k : p->constants)
      mark_value(k);
    for (auto* ch : p->protos)
      mark_object(ch);
    break;
  }
  case GcKind::Closure: {
    auto* cl = static_cast<Closure*>(o);
    if (cl->proto)
      mark_object(cl->proto);
    for (auto* uv : cl->upvals)
      mark_object(uv);
    break;
  }
  case GcKind::UpVal: {
    auto* uv = static_cast<UpVal*>(o);
    mark_value(uv->get());
    // Open upvalues pin their thread (PUC traverseuv).
    if (uv->open && uv->thread)
      mark_object(uv->thread);
    break;
  }
  case GcKind::Thread: {
    auto* th = static_cast<Thread*>(o);
    mark_thread_stack(this, th, /*strict_c_top=*/th == L_->current);
    if (th->hook.func)
      mark_object(th->hook.func);
    for (auto& fr : th->frames) {
      if (fr.cl)
        mark_object(fr.cl);
      if (fr.proto)
        mark_object(fr.proto);
      for (auto& v : fr.varargs)
        mark_value(v);
    }
    break;
  }
  case GcKind::Userdata: {
    auto* u = static_cast<Userdata*>(o);
    if (u->metatable)
      mark_object(u->metatable);
    mark_value(u->uservalue);
    break;
  }
  default:
    break;
  }
}

static TValue gc_object_value(GcObject* o, GcKind kind) {
  switch (kind) {
  case GcKind::Table:
    return TValue::obj(ValueTag::Table, static_cast<Table*>(o));
  case GcKind::Userdata:
    return TValue::obj(ValueTag::Userdata, static_cast<Userdata*>(o));
  default:
    return TValue::nil();
  }
}

static bool object_has_gc(State* L, GcObject* o, GcKind kind) {
  if (o->flags & GcFlagFinalized)
    return false;
  if (kind != GcKind::Table && kind != GcKind::Userdata)
    return false;
  TValue v = gc_object_value(o, kind);
  TValue mm = get_metamethod(L, v, "__gc");
  return mm.is_function() || mm.is_truthy();
}

void GC::clear_weak_tables(uint8_t mask) {
  if (!L_)
    return;
  for (GcObject* o = all_; o; o = o->next) {
    if (static_cast<GcKind>(o->kind) != GcKind::Table)
      continue;
    auto* t = static_cast<Table*>(o);
    if (t->weak_mode)
      t->clear_weak_entries(white_, mask);
  }
}

void GC::converge_ephemerons() {
  // Mark values of weak-key entries whose keys are already marked; repeat
  // until no new marks. Then white keys are truly unreachable via ephemerons.
  auto key_obj = [](const TValue& v) -> GcObject* {
    switch (v.tag()) {
    case ValueTag::String:
    case ValueTag::Table:
    case ValueTag::Function:
    case ValueTag::Userdata:
    case ValueTag::Thread:
      return v.as_gc();
    default:
      return nullptr;
    }
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (GcObject* o = all_; o; o = o->next) {
      if (static_cast<GcKind>(o->kind) != GcKind::Table)
        continue;
      auto* t = static_cast<Table*>(o);
      if (!(t->weak_mode & 1))
        continue; // needs weak keys
      if (t->weak_mode & 2)
        continue; // weak values: values are not marked as strong roots
      if (t->mark == white_)
        continue; // dead table
      for (auto& n : t->hash) {
        if (!n.used || n.key.is_nil())
          continue;
        GcObject* k = key_obj(n.key);
        if (!k || k->mark == white_)
          continue; // key white: do not mark value
        if (n.value.is_nil())
          continue;
        GcObject* before_gray = gray_;
        mark_value(n.value);
        if (gray_ != before_gray)
          changed = true;
      }
    }
    while (gray_)
      propagate_one();
  }
}

static int thread_live_top(Thread* th) {
  int limit = th->top;
  for (auto& fr : th->frames) {
    if (fr.proto)
      limit = std::max(limit, fr.base + fr.proto->maxstack);
    else if (fr.cl && fr.cl->proto)
      limit = std::max(limit, fr.base + fr.cl->proto->maxstack);
  }
  limit = std::max(limit, th->stack_base);
  return limit;
}

void GC::run_finalizers() {
  if (!L_ || finalize_.empty())
    return;
  running_finalizer_ = true;
  std::vector<GcObject*> pending = std::move(finalize_);
  finalize_.clear();
  std::string first_error;
  for (GcObject* o : pending) {
    o->flags |= GcFlagFinalized;
    GcKind kind = static_cast<GcKind>(o->kind);
    TValue obj = gc_object_value(o, kind);
    TValue mm = get_metamethod(L_, obj, "__gc");
    if (mm.is_function()) {
      try {
        // Push above all live Lua registers, not merely L->top (which may be
        // temporarily low between CALL/RETURN adjustments).
        Thread* th = L_->current;
        int live = thread_live_top(th);
        L_->ensure_stack(live + 4);
        th->top = live;
        int saved_top = live;
        L_->push(mm);
        L_->push(obj);
        // PUC CIST_FIN: mark the *carrying* frame (caller of __gc) so
        // debug.getinfo(2) reports namewhat "metamethod" / name "__gc".
        bool marked_fin = false;
        if (!th->frames.empty()) {
          th->frames.back().finalizer = true;
          marked_fin = true;
        }
        try {
          call_closure(L_, mm.as_closure(), 1, 0);
        } catch (...) {
          if (marked_fin && !th->frames.empty())
            th->frames.back().finalizer = false;
          throw;
        }
        if (marked_fin && !th->frames.empty())
          th->frames.back().finalizer = false;
        L_->set_abs_top(saved_top);
      } catch (const LuatierError& e) {
        if (L_->current)
          L_->set_abs_top(thread_live_top(L_->current));
        if (first_error.empty()) {
          std::string msg = e.what();
          // Lua wraps non-string errors; keep a stable marker for the suite.
          if (msg.find("error in __gc") == std::string::npos)
            first_error = "error in __gc (" + msg + ")";
          else
            first_error = msg;
        }
      } catch (...) {
        if (L_->current)
          L_->set_abs_top(thread_live_top(L_->current));
        if (first_error.empty())
          first_error = "error in __gc";
      }
    }
  }
  running_finalizer_ = false;
  if (!first_error.empty()) {
    // Drop any non-string err_obj from error{} inside __gc; the suite expects
    // the wrapped "error in __gc (...)" string from collectgarbage.
    if (L_ && L_->current)
      L_->current->err_obj_set = false;
    panic(first_error);
  }
}

void GC::sweep() {
  uint8_t other = white_ ^ 1;

  // Lua atomic order (simplified):
  // 0) wipe stack above top (dead temps must not pin objects)
  // 1) clear weak values while finalizable objects are still white
  // 2) collect finalizable list, resurrect them, mark their fields
  // 3) clear weak keys (keys reachable via finalizable objects survive)
  for (auto* th : {L_ ? L_->main : nullptr, L_ ? L_->current : nullptr}) {
    if (th)
      clear_dead_stack(th, /*strict_c_top=*/th == L_->current);
  }

  // Collect finalizable whites first (still white).
  std::vector<GcObject*> to_finalize;
  for (GcObject* o = all_; o; o = o->next) {
    if (o->mark != white_)
      continue;
    GcKind kind = static_cast<GcKind>(o->kind);
    if (!object_has_gc(L_, o, kind))
      continue;
    to_finalize.push_back(o);
  }

  // Clear weak values while finalizable objects are still white so that
  // weak-value tables drop them before resurrection (gc.lua bug51).
  clear_weak_tables(/*weak values*/ 2);
  converge_ephemerons();

  // Resurrect finalizable objects; propagate marks their metatables and (for
  // non-weak-value metatables) __gc closures/upvalues. Do NOT specially
  // keep_alive __gc: a weak-value metatable must be allowed to drop it
  // (gc.lua: __gc x weak tables / os.exit must not run).
  for (GcObject* o : to_finalize) {
    if (o->mark == white_) {
      o->mark = static_cast<uint8_t>(GcColor::Gray);
      o->gray_next = gray_;
      gray_ = o;
    }
  }
  while (gray_)
    propagate_one();

  converge_ephemerons();
  // Clear remaining weak values (e.g. __gc on a weak-value metatable), then
  // weak keys. Resurrected finalizable keys stay marked and are retained.
  clear_weak_tables(/*weak values*/ 2);
  clear_weak_tables(/*weak keys*/ 1);

  for (GcObject* o : to_finalize) {
    o->mark = other;
    finalize_.push_back(o);
  }

  GcObject** p = &all_;
  while (*p) {
    GcObject* o = *p;
    if (o->mark == white_) {
      GcKind kind = static_cast<GcKind>(o->kind);
      *p = o->next;
      switch (kind) {
      case GcKind::String:
        if (L_) {
          auto* s = static_cast<LjString*>(o);
          L_->strings.remove(s);
        }
        std::free(o);
        break;
      case GcKind::Table:
        delete static_cast<Table*>(o);
        break;
      case GcKind::Proto:
        delete static_cast<Proto*>(o);
        break;
      case GcKind::Closure:
        delete static_cast<Closure*>(o);
        break;
      case GcKind::UpVal:
        delete static_cast<UpVal*>(o);
        break;
      case GcKind::Thread:
        delete static_cast<Thread*>(o);
        break;
      case GcKind::Userdata:
        delete static_cast<Userdata*>(o);
        break;
      default:
        std::free(o);
        break;
      }
    } else {
      o->mark = other;
      p = &o->next;
    }
  }
  white_ = other;
  run_finalizers();
}

void GC::step() {
  switch (phase_) {
  case Phase::Pause:
    gray_ = nullptr;
    mark_roots();
    phase_ = Phase::Propagate;
    break;
  case Phase::Propagate:
    if (gray_)
      propagate_one();
    else
      phase_ = Phase::Sweep;
    break;
  case Phase::Sweep:
    sweep();
    debt_ = 0;
    phase_ = Phase::Pause;
    break;
  }
}

void GC::full_gc() {
  if (collecting_)
    return;
  collecting_ = true;
  try {
    while (phase_ != Phase::Pause)
      step();
    gray_ = nullptr;
    phase_ = Phase::Pause;
    step();
    while (phase_ != Phase::Pause)
      step();
  } catch (...) {
    collecting_ = false;
    phase_ = Phase::Pause;
    gray_ = nullptr;
    throw;
  }
  collecting_ = false;
}

void GC::maybe_step() {
  if (!gc_isrunning_ || running_finalizer_ || collecting_)
    return;
  // Prefer full cycles over incomplete incremental marking until barriers are
  // airtight; collect only at strategic safepoints (loop back-edges, calls,
  // NEWTABLE/CLOSURE) so freshly linked objects are rooted.
  // full_gc() owns the collecting_ guard (do not set it here or full_gc no-ops).
  if (debt_ >= threshold_) {
    full_gc();
    if (threshold_ < 1024 * 64)
      threshold_ = 1024 * 64;
  }
}

void GC::safepoint() {
  if (running_finalizer_ || collecting_)
    return;
  if (stress_every_safepoint)
    full_gc();
  else
    maybe_step();
}

void GC::set_running(bool running) { gc_isrunning_ = running; }
bool GC::is_running() const { return gc_isrunning_; }

} // namespace luatier
