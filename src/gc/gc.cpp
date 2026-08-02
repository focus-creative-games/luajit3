#include "gc/gc.hpp"

#include "runtime/closure.hpp"
#include "runtime/string.hpp"
#include "runtime/table.hpp"
#include "runtime/upvalue.hpp"
#include "vm/state.hpp"

#include <cstdlib>
#include <cstring>

namespace lj3 {

GC::GC(State* L) : L_(L) {
  const char* stress = std::getenv("LJ3_STRESS_GC_EVERY_SAFEPOINT");
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
  maybe_step();
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

void GC::mark_roots() {
  if (L_->globals)
    mark_object(L_->globals);
  if (L_->registry)
    mark_object(L_->registry);
  for (auto* th : {L_->main, L_->current}) {
    if (!th)
      continue;
    mark_object(th);
    for (auto& v : th->stack)
      mark_value(v);
    for (auto& fr : th->frames) {
      if (fr.cl)
        mark_object(fr.cl);
      if (fr.proto)
        mark_object(fr.proto);
    }
    for (UpVal* uv = th->open_upvals; uv; uv = uv->next_open)
      mark_object(uv);
  }
  for (auto& kv : L_->strings.short_intern)
    mark_object(kv.second);
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
    for (auto& v : t->array)
      mark_value(v);
    for (auto& n : t->hash) {
      if (n.used) {
        mark_value(n.key);
        mark_value(n.value);
      }
    }
    break;
  }
  case GcKind::Proto: {
    auto* p = static_cast<Proto*>(o);
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
    break;
  }
  case GcKind::Thread: {
    auto* th = static_cast<Thread*>(o);
    for (auto& v : th->stack)
      mark_value(v);
    break;
  }
  default:
    break;
  }
}

void GC::sweep() {
  GcObject** p = &all_;
  uint8_t other = white_ ^ 1;
  while (*p) {
    GcObject* o = *p;
    if (o->mark == white_) {
      *p = o->next;
      switch (static_cast<GcKind>(o->kind)) {
      case GcKind::String:
        // remove from intern table if present
        if (L_) {
          auto* s = static_cast<LjString*>(o);
          auto it = L_->strings.short_intern.find(std::string(s->view()));
          if (it != L_->strings.short_intern.end() && it->second == s)
            L_->strings.short_intern.erase(it);
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
  phase_ = Phase::Pause;
  step();
  while (phase_ != Phase::Pause)
    step();
}

void GC::maybe_step() {
  if (debt_ >= threshold_)
    step();
}

void GC::safepoint() {
  if (stress_every_safepoint)
    full_gc();
  else
    maybe_step();
}

} // namespace lj3
