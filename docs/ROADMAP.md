# LuaJIT3 Roadmap

This roadmap implements [PROPOSAL.md](../PROPOSAL.md) under these constraints:

- **Language:** C++17 + CMake
- **Team:** one expert engineer (serial delivery)
- **Schedule:** no hard deadline; advance only when milestone acceptance criteria pass
- **Priority:** semantic correctness > observability > performance > ecosystem

## Phase gates

| Phase | Focus | Gate |
|-------|-------|------|
| 0 | Spec freeze + build skeleton | Specs consistent; hello CLI + dummy test |
| 1 | Correctness core (no JIT) | Interpreter matches Lua 5.3.0+ on conformance/diff suite |
| 2 | Baseline JIT | Warm speedup; forced deopt still correct |
| 3 | Optimizing JIT | Hot speedup; three-tier differential tests |
| 4 | Ecosystem | Embeddable C API + diagnostics/profiling |
| 5 | Advanced | Generational GC, background compile, cache, FFI — on demand |

## Out of scope before first production milestone

- FFI
- ABI compatibility with historical LuaJIT
- Trace compiler
- Persistent native code cache

See `docs/spec/` for frozen contracts and `docs/adr/` for resolved open questions.
