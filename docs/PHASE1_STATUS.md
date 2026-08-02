# Phase 1 Status — Lua 5.3 Language Semantics

**Reference:** [docs/spec/manual/Lua-5.3.html](spec/manual/Lua-5.3.html)  
**Scope:** Pure interpreter (no JIT required for acceptance).  
**Stop point:** Await human acceptance before Phase 2+.

## Implemented (language core)

- Lexer / AST parser / sema (break-in-loop) / LBC lowering
- Values: nil, bool, int, float, string, table, function, thread
- Operators: arithmetic, bitwise, relational, logical (`and`/`or` short-circuit), `..`, `#`
- Statements: if/elseif/else, while, repeat, numeric for, generic for, break, do/end, goto/label
- Functions: closures, upvalues, vararg, multiple returns / assignment unpack
- Tables: array/hash, field/index, constructors
- Metatables: `__index`, `__newindex`, `__add`/arith, `__len`, `__eq`/`__lt`/`__le`, `__concat`, `__call`
- Errors: `error`, `pcall`, `xpcall` (basic)
- Iteration helpers: `next`, `pairs`, `ipairs`
- Coroutines: `create`, `resume`, `yield`, `status`, `running`
- GC: incremental mark-sweep + stress env `LJ3_STRESS_GC_EVERY_SAFEPOINT`

## Base library (language-facing)

`print`, `type`, `assert`, `error`, `tonumber`, `tostring`, `select`, `pcall`, `xpcall`,  
`getmetatable`, `setmetatable`, `rawget`, `rawset`, `rawequal`, `rawlen`,  
`next`, `pairs`, `ipairs`, `coroutine.*`

## Known gaps / latitudes vs full 5.3

- Full debug library / line hooks not complete
- String escape forms `\x` / `\u{}` / `\z` incomplete
- Registry metatables for non-table types (string/number) not wired
- Weak tables / `__gc` finalizers not complete
- `print(f())` multi-result forwarding edge cases may expose extra stack slots
- Standard libraries `math` / `string` / `table` / `io` / `os` / `utf8` / `package` not required for language-core acceptance

## Recent fixes (acceptance blockers)

- CallFrame dangling refs after reentrant meta/C calls → frame-index reload in interpreter
- Generic `for`: iterator explist adjusted to 3 values; `TFORCALL` at `R[A+3]`; `fix_sbx` absolute dest
- `pcall`/`xpcall` unwind leftover Lua frames on error
- Coroutine `top` is **per-thread** (was shared `State::top_`, caused vector OOB on resume after yield)
- Multret lowering: trailing call/vararg in `return`, call args, table lists (`SETLIST`); `(expr)` via `ExprParen` truncates
- Language oracle suite modularized under `tests/lang53/` (~150+ cases); matrix in [test-coverage-lang53.md](spec/test-coverage-lang53.md)

## How to verify

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
build\Debug\lj3_tests.exe
```

Primary suite: `tests/test_lang53.cpp` + `tests/lang53/*.cpp`  
Coverage matrix: [docs/spec/test-coverage-lang53.md](spec/test-coverage-lang53.md) (language-core rows Done or documented GAP; no TODO).
