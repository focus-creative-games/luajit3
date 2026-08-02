# Frame, Error, and Coroutine Model

**Status:** Phase 0 freeze  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §10, §18–19

## Principle

Lua execution state is owned by explicit VM structures. The host C++ stack is not the sole source of truth for Lua frames, locals, or unwind state.

## CallFrame

Logical fields:

```text
CallFrame {
  callee            // Closure* or CFunction
  proto_or_c        // Proto* or native target tag
  base              // index into thread value stack
  top               // exclusive top relative to thread stack
  expected_results  // LUA_MULTRET or count
  saved_pc          // caller bytecode PC (or native return stub id)
  frame_kind
  flags             // tail, protected, vararg, etc.
  prev              // previous frame index
}
```

### Frame kinds

| Kind | Role |
|------|------|
| `InterpLua` | Interpreter activation |
| `BaselineJit` | Baseline native frame |
| `OptJit` | Optimizing native frame |
| `CApi` | Native C API / host call |
| `Continue` | Resume/continuation |
| `Protected` | `pcall`/`xpcall` boundary |

## Value stack

Each `Thread` (coroutine) has:

- growable `TValue` stack
- frame stack (or frame list)
- open upvalue list
- status: `Fresh`, `Running`, `Suspended`, `Dead`, `Error`
- pending yield results / error object

## Multiple returns

- Fast path: small number of results written to caller registers
- `LUA_MULTRET` / `C=0` CALL: results occupy contiguous stack slots; caller top adjusted
- Overflow of native ABI returns never truncates Lua multi-ret silently

## Tail calls

`TAILCALL` replaces the current Lua frame in place:

1. Evaluate args in current frame
2. Close upvalues for abandoned locals as required
3. Reuse (or shrink/grow) base for callee
4. Do not grow logical Lua call depth for the abandoned frame

## Protected calls

`pcall`/`xpcall` push a `Protected` boundary recording:

- restore stack top / frame depth
- optional message handler (`xpcall`)
- jump buffer or explicit unwind token for C boundaries

Internal VM prefer structured unwind (frame walk + status codes). Host `setjmp`/`longjmp` (or SEH equivalents) only at selected C API boundaries.

### Unwind obligations

1. Preserve Lua stack-trace material (PC + protos)
2. Close open upvalues for unwound frames
3. Stop at protected boundary; return status to caller
4. Interact safely with finalizers (no recursive unprotected failure loops)

## Coroutines

- `coroutine.create` allocates a `Thread` with fresh stacks
- `resume` runs until return, yield, or error
- `yield` suspends, packing values for resumers
- Yield across C API frames: follow Lua rules (error unless yieldable C)
- Optimized JIT frames: may deopt before unsupported yield boundaries ([deopt-safepoint.md](deopt-safepoint.md))

## Debug hooks

Hooks poll at:

- instruction/line boundaries (interpreter)
- call/return
- safepoints in JIT code

Enabling certain hooks may invalidate optimizing code and force interpreter/baseline.
