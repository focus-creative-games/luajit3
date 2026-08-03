# Phase 1 Status — Full Lua 5.3 (Language + Stdlib)

**Reference:** [docs/spec/manual/Lua-5.3.html](spec/manual/Lua-5.3.html)  
**Scope:** Pure interpreter (no JIT). Full language semantics **and** all standard libraries.  
**Hard gate:** Official [lua-5.3.4-tests](https://www.lua.org/tests/) via  
`luatier -e"_U=true" all.lua` → **`final OK`**.  
**Stop point:** Await human acceptance before Phase 2+.

Status board: [docs/spec/puc53-status.md](spec/puc53-status.md)  
Language oracle matrix: [docs/spec/test-coverage-lang53.md](spec/test-coverage-lang53.md)

## Acceptance criteria

1. Lua 5.3 language features complete (no intentional language OUT/GAP left undocumented as permanent).
2. Standard libraries: `base`, `coroutine`, `package`, `string`, `table`, `math`, `utf8`, `io`, `os`, `debug`.
3. Official basic suite (`_U=true`) green in CI (Linux + Windows).
4. `luatier_tests` (internal oracles) green.
5. Complete suite (no `_U`) pursued in-phase; platform-specific failures documented.

## Out of Phase 1

- Internal/`ltests`/`testC`
- Any JIT tier

## How to verify

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
build\Debug\luatier_tests.exe
scripts\run_puc_tests.ps1
```

```sh
scripts/run_puc_tests.sh
```
