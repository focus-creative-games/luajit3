# LuaJIT3

From-scratch Lua **5.3+** JIT runtime (C++17). **Not** derived from historical LuaJIT.

See [PROPOSAL.md](PROPOSAL.md) and [docs/ROADMAP.md](docs/ROADMAP.md).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/luajit3 --version
./build/luajit3 -e "print(1+2)"
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

- `LJ3_STRESS_GC_EVERY_SAFEPOINT=1`
- `LJ3_STRESS_FORCE_DEOPT=1`
- `LJ3_STRESS_DISABLE_IC=1`
- `LJ3_JIT_LOG=1`
