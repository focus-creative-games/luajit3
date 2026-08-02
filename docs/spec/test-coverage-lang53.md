# Lua 5.3 Language Test Coverage Matrix

**Reference:** [manual/Lua-5.3.html](manual/Lua-5.3.html)  
**Suite entry:** `tests/test_lang53.cpp` → `tests/lang53/*.cpp`  
**Scope:** Language core + language-facing base library (not full stdlib).

Status legend: **Done** | **GAP** (documented latitude / out of Phase-1 scope) | **OUT** (explicitly excluded)

## A. Types & coercion (§2.1 / §3.4.3) — `test_types_coercion.cpp`

| Case family | Status |
|-------------|--------|
| `type()` for nil/boolean/number/string/table/function/thread | Done |
| int vs float still `type` number; arithmetic int/float paths | Done |
| string↔number coercion in arith (not wired → error); invalid coercion errors | Done |
| NaN inequality; inf observable via compare | Done |
| `//` toward -∞ boundaries | Done |

## B. Operators (§3.4.1–3.4.8) — `test_ops.cpp`

| Case family | Status |
|-------------|--------|
| Arithmetic suite + unary `-`; `^` right-assoc | Done |
| Bitwise `& \| ~ << >>` unary `~`; non-integer error | Done |
| Relational + cross-type equality; tables not equal by default | Done |
| `and`/`or`/`not` values + short-circuit | Done |
| `..` right-assoc; `#` string/sequence; hole border smoke | Done |
| Precedence cross-checks | Done |

## C. Statements (§3.3) — `test_statements.cpp`

| Case family | Status |
|-------------|--------|
| `do`/`local` block scope | Done |
| Assignment adjustment (pad/drop); table field multi-assign | Done |
| if / while / repeat | Done |
| repeat-until visibility of body locals | GAP |
| Numeric for +/− step, empty range, break | Done |
| Generic for ipairs/pairs/custom; multi vars | Done |
| Call-as-statement discards results | Done |
| `return f()` vs `return (f())` | Done |

## D. Tables (§3.4.9) — `test_tables_meta.cpp`

| Case family | Status |
|-------------|--------|
| Array/hash/mixed; `a=` / `[k]=` / list form | Done |
| Constructor trailing multret `{f()}` / `{1,f()}` | Done |
| `#t`, rawget/rawset/rawlen/rawequal | Done |

## E. Functions / multret / closures (§3.4.10–12) — `test_functions.cpp`

| Case family | Status |
|-------------|--------|
| Params default nil; vararg + `select` | Done |
| Closures / nested upvalues | Done |
| Method call `obj:method` | Done |
| Multret in assign, args, constructors, `()` truncate | Done |

## F. Goto / visibility (§3.5) — `test_visibility_goto.cpp`

| Case family | Status |
|-------------|--------|
| Forward/backward goto | Done |
| Duplicate label error | Done |
| Jump into local scope rejected by sema | GAP |
| Closure capture after local declaration | Done |

## G. Metatables (§2.4) — `test_tables_meta.cpp`

| Case family | Status |
|-------------|--------|
| `__index`/`__newindex` table+function | Done |
| Arith / `__len` / `__concat` / cmp / `__call` | Done |
| `__metatable` protect; raw* bypass | Done |
| Registry metatables for non-table types | GAP |
| Weak tables / `__gc` | GAP |

## H. Errors (§2.3) — `test_errors_coro.cpp`

| Case family | Status |
|-------------|--------|
| `error` + `pcall` false,msg | Done |
| `pcall` success multret | Done |
| `xpcall` + message handler | Done |
| `assert` failure | Done |

## I. Coroutines (§2.6) — `test_errors_coro.cpp`

| Case family | Status |
|-------------|--------|
| create/resume args+results | Done |
| yield/resume multivalue | Done |
| status suspended/dead; running | Done |
| yield from main / resume dead → error | Done |
| status `normal` (nested resume) | Done |

## J. Base library edges — `test_baselib.cpp`

| Case family | Status |
|-------------|--------|
| tonumber/tostring basics | Done |
| next key-set completeness | Done |
| select negative index | Done |

## OUT / GAP (not Phase-1 language acceptance)

| Item | Status |
|------|--------|
| Full `math`/`string`/`table`/`io`/`os`/`utf8`/`package` | OUT |
| Debug library / hooks | OUT |
| String escapes `\x` / `\u{}` / `\z` | GAP |
| To-be-closed variables (5.4+) | OUT |
| Official PUC-Rio test suite submodule | OUT (see test-plan.md) |

## How to run

```bat
cmake --build build --config Debug --target lj3_tests
build\Debug\lj3_tests.exe
```
