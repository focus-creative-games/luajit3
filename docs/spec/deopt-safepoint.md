# Deoptimization and Safepoints

**Status:** Phase 0 freeze  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §16, §21

Even though Phase 1 has no JIT, prototypes and tooling reserve metadata slots so baseline/optimizing tiers cannot ship without deopt.

## Hard rule

No native JIT code path may be merged without:

1. bytecode PC mapping
2. stack maps for GC-live references
3. value maps for deopt materialization
4. frame reconstruction into interpreter state

## Safepoints

Required poll points:

- interpreter loop boundaries
- function call boundaries
- backward branches
- allocation slow paths
- JIT poll sites
- selected runtime helper entry/exit

At a safepoint the runtime may:

- run incremental GC
- honor invalidation requests
- run debug hooks
- deoptimize the current optimized frame

## Stack maps

For each safepoint / deopt exit in native code:

```text
StackMap {
  pc                 // LBC PC (or synthetic)
  frame_id
  slots[]            // {location, kind: ref|int|float|const}
}
```

Locations: physical register, stack spill slot, or constant rematerialization recipe.

## Deopt metadata

```text
DeoptPoint {
  bytecode_pc
  inlined_frames[]   // optional Phase 3+
  value_map
  materializations   // for elided values
  target             // always interpreter reconstruction
}
```

## Guard classes

- value tag / type
- int vs float kind
- table shape / version
- metatable pointer / version
- callee identity
- environment version

## Deopt triggers

- guard failure
- unsupported runtime event
- debug hook activation that forbids optimized code
- explicit invalidation (metatable/shape/env)
- GC/runtime state requiring fallback
- unsupported yield boundary

## Correctness

After deopt, execution continues in the interpreter **as if** optimized code had never violated Lua semantics. Side effects already performed remain; speculation may only retract uncommitted assumptions.

## Debug policy

- Full debug / heavy hooks: prefer interpreter or baseline
- Source-correlated stack traces are mandatory across tiers
