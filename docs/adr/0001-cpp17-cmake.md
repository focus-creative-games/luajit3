# ADR 0001: C++17 and CMake

## Status

Accepted

## Context

LuaJIT3 is a from-scratch runtime. Implementation language and build system must be fixed before code lands.

## Decision

- Use **C++17** for all core components
- Use **CMake** 3.16+ as the build system
- Produce static library `luajit3` and CLI `luajit3`
- Prefer sanitizer-friendly code; avoid mandatory exceptions in VM hot paths (exceptions allowed at API boundaries if needed)

## Consequences

- Clear embedding story via `extern "C"` Lua-compatible API
- IR/pass infrastructure can use modern C++ containers carefully
- No dependency on C++20 ranges/modules
