# GC and Memory Management

**Status:** Phase 0 freeze  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §20–21

## Strategy (v1)

**Precise, non-moving, incremental mark-sweep** collector.

Roadmap extensions (Phase 5+):

- v2: generational extension
- v3: optional selective compaction if justified

Non-moving simplifies JIT embedded references, deopt metadata, C API pointers, and debugging.

## Object header

Every heap object begins with:

```text
GcHeader {
  next           // gray/all list link
  type           // object kind
  mark           // white0/white1/gray/black bits
  flags          // finalized, weak, fixed, etc.
}
```

## Allocation

- Segregated size classes / arenas by object kind where useful
- Fast bump or freelist path in the nursery of each class
- Allocation may debit GC debt and trigger incremental work / safepoint

## Colors and phases

Tri-color abstraction with flip-white for atomic sweep prep:

1. **Pause / restart** — flip white, mark roots gray
2. **Propagate** — incremental gray→black, scan children
3. **Atomic** — finish marking, remark barriers, weak processing
4. **Sweep** — reclaim white objects incrementally
5. **Finalize** — run finalizers for dead userdata/tables with `__gc`

## Roots

Must scan at least:

- all thread value stacks and frame stacks
- open upvalues
- globals / registry
- interned short-string table
- pending error / yield state
- JIT metadata and code-embedded object references (Phase 2+)
- API anchored references (`luaL_ref`, native handles)

## Write barriers

- Incremental: barrier on black → white stores
- Generational (future): old → young stores

API: `gc_barrier(obj, value)` / `gc_barrier_back(table)` for table rehash cases.

## Weak references

Support weak keys, weak values, and ephemeron-style marking consistent with Lua semantics. Exact timing within an incremental cycle is an allowed latitude if final observable rules match the language.

## Finalizers

- Objects with `__gc` are kept alive until finalizer runs
- Resurrection is allowed once; document behavior in stress tests
- Finalizers run in a controlled phase; errors in finalizers are reported without aborting the VM process by default

## Safepoint integration

GC work is polled at:

- interpreter loop / `CHECKGC` / `SAFEPPOINT`
- allocation slow paths
- call boundaries
- JIT safepoint polls

Stress mode: `LUATIER_STRESS_GC_EVERY_SAFEPOINT=1` forces a full incremental step (or full collect in debug) at every safepoint.

## Precision

No conservative stack scanning of the host C++ stack for Lua heap objects. Host code must keep Lua values in anchored API stack slots or explicit root lists.
