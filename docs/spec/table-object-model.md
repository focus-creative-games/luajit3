# Table and Object Model

**Status:** Phase 0 freeze  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §8–9

## TValue

Explicit tagged value (no NaN-boxing in v0.1):

```cpp
struct TValue {
  uint64_t payload;
  uint32_t type;  // ValueTag
  uint32_t aux;   // spare / immediate hints
};
```

### ValueTag

`Nil`, `Bool`, `Int`, `Float`, `String`, `Table`, `Function`, `Userdata`, `Thread`, `LightUserdata`, `Internal`.

## Strings

- cached length + hash
- short strings always interned
- long strings not necessarily interned
- metamethod names / identifiers strongly interned

## Tables

Logical array part + hash part.

Metadata for optimization (maintain from Phase 1):

- metatable pointer
- `structure_version` / epoch (bump on shape-changing ops)
- array density hints
- optimization class flags: plain, dense-array, record-like, megamorphic, weak, metamethod-observed

## Functions

- `Proto`: immutable bytecode + constants + debug
- `Closure`: proto or C function + upvalue array

## Upvalues

- Open: points at stack slot
- Closed: owns heap `TValue`
- Per-thread open-upvalue list

## Userdata / threads

As in [frame-model.md](frame-model.md) and [gc.md](gc.md).
