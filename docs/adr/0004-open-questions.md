# ADR 0004: Closing PROPOSAL §31 open questions (initial)

## Status

Accepted for v0.1 defaults

| # | Question | Decision |
|---|----------|----------|
| 1 | Compatibility matrix | See `docs/spec/compatibility.md` — Lua 5.3.6 first |
| 2 | LBC encoding | ABC/ABx/AsBx `uint32_t`, see `docs/spec/lbc.md` |
| 3 | TValue layout | Explicit tagged `{payload,type,aux}` |
| 4 | Table shape/version | `structure_version` bump on structural change |
| 5 | Debug under opt | Prefer interpreter/baseline when heavy hooks enabled |
| 6 | Yield in optimized code | Conservative deopt (Phase 3+) |
| 7 | Code cache eviction | In-memory only; no persistence in v0.1 |
| 8 | Optimizing backend | Self-hosted MIR→LIR; no heavy external JIT deps initially |
| 9 | Long-string intern | Short ≤40 interned; long not by default |
| 10 | Native runtime API | Deferred to Phase 4 after C API subset stabilizes |
