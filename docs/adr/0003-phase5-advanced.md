# ADR 0003: Phase 5 advanced features gating

## Status

Accepted

## Decision

Phase 5 features are implemented as **opt-in stubs** behind compile-time/docs gates until Phase 1–3 acceptance criteria pass:

1. Generational GC
2. Background compilation
3. AOT bytecode / versioned native cache
4. Limited FFI sequence
5. Selective compaction / PGO

No Phase 5 feature is enabled by default.
