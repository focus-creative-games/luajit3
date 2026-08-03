# Compatibility Charter

**Status:** Phase 0 freeze  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §4, §23–24

## Layers

### A. Language compatibility (highest priority)

Target: **Lua 5.3.0 and later** language semantics as documented by the official Lua reference manuals for 5.3.x / 5.4.x, with an explicit matrix below.

| Area | Commitment | Notes |
|------|------------|-------|
| Lexical / syntax | Match | Long strings/comments, goto/label rules |
| Integers / floats | Match 5.3+ | Distinct integer and float; coercion rules as Lua |
| Tables / metatables | Match | Including `__index`/`__newindex`/`__len`/arithmetic/bitwise |
| Closures / upvalues | Match | Open/closed semantics |
| Vararg / multi-ret | Match | |
| Tail calls | Match | VM-level frame replace, not host TCO |
| Coroutines | Match | yield/resume across Lua frames |
| `error` / `pcall` / `xpcall` | Match | |
| Weak tables / finalizers | Match with documented timing latitude | See [gc.md](gc.md) |
| Debug library | Match with optimized-code caveats | See [deopt-safepoint.md](deopt-safepoint.md) |
| Bitwise ops | Match 5.3+ | |
| UTF-8 library / `string.pack` | Match when stdlib ported | Phase 1+ |

**Initial language target for conformance:** Lua **5.3.6** reference behavior, plus 5.4 features gated behind feature flags once 5.3 suite is green (`to-be-closed`, new generational GC API options, etc.).

### B. C API semantic compatibility (secondary)

Provide a Lua-compatible C API surface for loading, calling, stack ops, tables, metatables, and protected calls. Semantics follow official Lua C API docs where practical.

Not required initially:

- Exact binary layout of `lua_State`
- Exact auxiliary library ABI of every `luaL_*` helper
- Module ABI identical to PUC-Rio `luaXX.dll` / `.so`

### C. ABI compatibility (non-goal)

LuaTier may define its own:

- `TValue` and object layouts
- Call frames and native calling conventions for JIT code
- Code cache and stack map encodings

No drop-in replacement promise for existing LuaJIT binaries or FFI ABIs.

## Documented latitudes

These may differ from a specific PUC-Rio build timing while remaining language-legal:

1. Finalizer scheduling relative to incremental GC phases ([gc.md](gc.md))
2. Weak table collection timing within an incremental cycle
3. When optimized code is invalidated by debug hooks
4. Performance-only invalidation of ICs / JIT code (must not change observable Lua results except timing)

## Explicit non-goals

- Historical LuaJIT 2.x FFI
- Trace-based compilation model
- Matching undocumented PUC-Rio memory layouts
