#pragma once

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lj3 {

struct Lj3Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] inline void panic(const std::string& msg) { throw Lj3Error(msg); }

template <typename T>
inline T* lj3_alloc(std::size_t n = 1) {
  auto* p = static_cast<T*>(std::malloc(sizeof(T) * n));
  if (!p)
    panic("out of memory");
  return p;
}

inline void lj3_free(void* p) { std::free(p); }

} // namespace lj3
