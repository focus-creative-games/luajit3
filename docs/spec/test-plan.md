# Test and Validation Plan

**Status:** Phase 0 freeze  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §26

## Layers

### A. Unit / component tests

- lexer tokens and source spans
- parser AST shapes
- semantic errors (goto, label, break)
- LBC encoder/verifier
- table insert/get/rehash
- GC mark/sweep stress on synthetic graphs

### B. Conformance

- Vendor or submodule official Lua test suites (5.3.x first)
- Track pass rate in CI; Phase 1 gate = broad pass without JIT

### C. Differential testing

Run the same chunk under:

1. reference Lua (PUC-Rio)
2. LuaJIT3 interpreter
3. baseline JIT (Phase 2+)
4. optimizing JIT (Phase 3+)

Compare stdout, exit status, error messages (normalized), and selected debug traces.

Harness: `tests/differential/` + CMake `ctest` fixture.

### D. Fuzzing

Targets:

- parser / lexer
- lowering
- bytecode verifier
- table + metatable interactions
- coroutine × error × GC
- JIT invalidation / deopt (Phase 2+)

### E. Stress knobs

| Env / flag | Effect |
|------------|--------|
| `LJ3_STRESS_GC_EVERY_SAFEPOINT` | GC at every safepoint |
| `LJ3_STRESS_FORCE_DEOPT` | frequent deopt (Phase 2+) |
| `LJ3_STRESS_DISABLE_IC` | random/generic IC path |
| `LJ3_STRESS_TIER_THRASH` | aggressive tier up/down |
| `LJ3_STRESS_POISON_ALLOC` | allocator poison in Debug |

## Benchmarks (post Phase 1)

- microbenchmarks under `bench/micro`
- table-heavy / alloc-heavy / coroutine-heavy
- real-world scripts as available

Performance never gates Phase 1. Phase 2+ require no semantic regressions vs interpreter.

## CI matrix

- Windows x86-64 (MSVC or clang-cl)
- Linux x86-64 (gcc or clang)
- Debug + Release
- ASan/UBSan on Linux Debug when available
