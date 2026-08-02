# Test and Validation Plan

**Status:** Phase 1 active — full Lua 5.3 + official suite  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §26, [puc53-status.md](puc53-status.md)

## Layers

### A. Unit / component tests

- lexer tokens and source spans
- parser AST shapes
- semantic errors (goto, label, break)
- LBC encoder/verifier
- table insert/get/rehash
- GC mark/sweep stress on synthetic graphs
- `tests/lang53/*` language oracles

### B. Conformance (Phase 1 hard gate)

- Vendor: `tests/lua-5.3.4-tests/` (sha256 `b80771238271c72565e5a1183292ef31bd7166414cd0d43a8eb79845fa7f599f`)
- **Gate:** `luajit3 -e"_U=true" all.lua` prints `final OK`
- Track per-file pass rate in [puc53-status.md](puc53-status.md)
- Scripts: `scripts/run_puc_tests.sh`, `scripts/run_puc_tests.ps1`
- After basic gate: chase complete suite (no `_U`) with `package.loadlib` + suite `libs/`

### C. Differential testing

Run the same chunk under:

1. reference Lua (PUC-Rio)
2. LuaJIT3 interpreter
3. baseline JIT (Phase 2+)
4. optimizing JIT (Phase 3+)

Compare stdout, exit status, error messages (normalized), and selected debug traces.

Harness: expand beyond `tests/test_differential.cpp` oracles as needed.

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
- Official basic suite (`_U=true`) once the interpreter can host `all.lua`
