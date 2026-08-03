# LuaTier

A fully rewritten modern Lua runtime supporting Lua **5.3+**, featuring tiered hybrid execution: bytecode interpreter, offline AOT compiler, and optional tracing JIT.

Built in C++17 from scratch — **not** derived from historical LuaJIT. See [PROPOSAL.md](PROPOSAL.md) and [docs/ROADMAP.md](docs/ROADMAP.md).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/luatier --version
./build/luatier -e "print(1+2)"
```

## Layout

- `docs/spec/` — frozen architecture contracts
- `src/frontend/` — lexer, parser, AST, sema, LBC lowering
- `src/vm/` — bytecode, interpreter, state, builtins
- `src/runtime/` — TValue, string, table, closure, upvalue
- `src/gc/` — incremental mark-sweep
- `src/jit/` — hotness, IC, baseline/opt/deopt scaffolding
- `src/api/` — Lua-compatible C API
- `tests/` — unit + differential oracle tests

## Stress env

- `LUATIER_STRESS_GC_EVERY_SAFEPOINT=1`
- `LUATIER_STRESS_FORCE_DEOPT=1`
- `LUATIER_STRESS_DISABLE_IC=1`
- `LUATIER_JIT_LOG=1`
