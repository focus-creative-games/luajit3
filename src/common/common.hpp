#pragma once

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace luatier {

// Max nested C calls / resume depth (PUC LUAI_MAXCCALLS). Prevents native
// stack overflow on recursive coroutine.wrap / C callbacks.
#ifndef LUAI_MAXCCALLS
#define LUAI_MAXCCALLS 200
#endif

struct LuatierError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] inline void panic(const std::string& msg) { throw LuatierError(msg); }

template <typename T>
inline T* luatier_alloc(std::size_t n = 1) {
  auto* p = static_cast<T*>(std::malloc(sizeof(T) * n));
  if (!p)
    panic("out of memory");
  return p;
}

inline void luatier_free(void* p) { std::free(p); }

} // namespace luatier
