# LBC — LuaTier Portable Register Bytecode

**Status:** Phase 0 freeze (v0.1)  
**Related:** [PROPOSAL.md](../../PROPOSAL.md) §7

LBC is the execution contract between frontend lowering, interpreter, and JIT tiers.

## Design goals

- Architecture-independent
- Register-based (explicit dataflow)
- Rich enough for full Lua 5.3+ semantics
- Stable enough for deopt PC mapping and tooling dumps

## Instruction encoding

Each instruction is a little-endian `uint32_t`:

```
 bits  0..7   opcode (OpCode)
 bits  8..15  A
 bits 16..23  B
 bits 24..31  C
```

Formats:

| Format | Fields | Meaning |
|--------|--------|---------|
| ABC | A, B, C | three unsigned 8-bit operands |
| ABx | A, Bx | Bx = `(C<<8)\|B` unsigned 16-bit |
| AsBx | A, sBx | signed Bx = ABx − 32768 |
| Ax | Ax | Ax = full 24-bit immediate (rare) |

Registers are frame-local slots `0 .. maxstack-1`. Constants are indexed into the prototype constant pool (`K`).

Signed PC-relative jumps use `AsBx` where the displacement is in **instructions**.

## OpCode catalog (v0.1)

### Data movement

| Op | Format | Semantics |
|----|--------|-----------|
| `MOVE` | ABC | `R[A] = R[B]` |
| `LOADNIL` | ABC | `R[A] .. R[A+B] = nil` |
| `LOADBOOL` | ABC | `R[A] = (B != 0)`; if `C` then PC++ |
| `LOADINT` | AsBx | `R[A] = sBx` as integer (small immediates) |
| `LOADFLOAT` | ABx | `R[A] = number_from_K(Bx)` (float const) |
| `LOADK` | ABx | `R[A] = K[Bx]` |
| `LOADKX` | ABC | `R[A] = K[EXTRAARG.Ax]` (followed by `EXTRAARG`) |
| `EXTRAARG` | Ax | Ax payload for previous op |

### Arithmetic / bitwise / logic

| Op | Format | Semantics |
|----|--------|-----------|
| `ADD` `SUB` `MUL` `DIV` `IDIV` `MOD` `POW` | ABC | `R[A] = RK(B) op RK(C)` |
| `BAND` `BOR` `BXOR` `SHL` `SHR` | ABC | bitwise |
| `UNM` `BNOT` `NOT` `LEN` | ABC | `R[A] = op R[B]` |
| `CONCAT` | ABC | `R[A] = concat R[B]..R[C]` |

`RK(x)`: if `x` has high bit set (x ≥ 256), use `K[x-256]`, else `R[x]`. For v0.1 encoder, prefer explicit `LOADK` + register ops when simplifying; RK form remains valid.

### Comparisons / tests

| Op | Format | Semantics |
|----|--------|-----------|
| `EQ` `LT` `LE` | ABC | if `(RK(B) op RK(C)) != A` then PC++ |
| `TEST` | ABC | if `not (R[A] iff C)` then PC++ |
| `TESTSET` | ABC | if `(R[B] iff C)` then `R[A]=R[B]` else PC++ |

Comparison ops are followed by a jump instruction (lowered as `JMP`) when the condition continues.

### Tables

| Op | Format | Semantics |
|----|--------|-----------|
| `NEWTABLE` | ABC | `R[A] = {}` with array/hash size hints B/C (log2 or 0) |
| `GETTABLE` | ABC | `R[A] = R[B][RK(C)]` |
| `SETTABLE` | ABC | `R[A][RK(B)] = RK(C)` |
| `GETFIELD` | ABC | `R[A] = R[B][Kstring(C)]` |
| `SETFIELD` | ABC | `R[A][Kstring(B)] = RK(C)` |
| `GETI` | ABC | `R[A] = R[B][C]` integer key immediate |
| `SETI` | ABC | `R[A][B] = RK(C)` |
| `GETTABUP` | ABC | `R[A] = UpValue[B][RK(C)]` |
| `SETTABUP` | ABC | `UpValue[A][RK(B)] = RK(C)` |
| `SETLIST` | ABC | set list from `R[A+1]..` into table `R[A]` |

### Functions / upvalues / calls

| Op | Format | Semantics |
|----|--------|-----------|
| `CLOSURE` | ABx | `R[A] = closure(Proto[Bx])` + capture upvalues |
| `GETUPVAL` | ABC | `R[A] = UpValue[B]` |
| `SETUPVAL` | ABC | `UpValue[B] = R[A]` |
| `CALL` | ABC | call `R[A]` with `B-1` args (B=0: varargs to top); `C-1` results (C=0: all) |
| `TAILCALL` | ABC | frame-replacing call |
| `RETURN` | ABC | return `R[A] .. R[A+B-2]` (B=0: to top; B=1: none) |
| `VARARG` | ABC | load varargs into `R[A]..` |
| `SELF` | ABC | `R[A+1]=R[B]; R[A]=R[B][RK(C)]` |

### Control flow

| Op | Format | Semantics |
|----|--------|-----------|
| `JMP` | AsBx | PC += sBx; may close upvalues ≥ R[A] if A != 0 (A=0 means no close; A=1 closes from 0 — see encoder: A is first register to close, 0 = none via sentinel encoding: **A==0 means no upvalue close**, else close from `A-1`) |
| `FORPREP` | AsBx | numeric for prep; PC += sBx |
| `FORLOOP` | AsBx | numeric for loop; PC += sBx if continue |
| `TFORCALL` | ABC | generic for call |
| `TFORLOOP` | AsBx | generic for loop |

### Runtime / GC / meta markers

| Op | Format | Semantics |
|----|--------|-----------|
| `CHECKGC` | ABC | allocation/GC poll (A unused or debt hint) |
| `SAFEPPOINT` | ABC | explicit safepoint / hook poll |

Metamethod dispatch is primarily via runtime slow paths from table/arith ops. Optional future `META_*` helpers may be added with a spec revision.

## Function prototype metadata

Each `Proto` contains:

| Field | Purpose |
|-------|---------|
| `code[]` | instruction stream |
| `constants[]` | nil/bool/int/float/string |
| `protos[]` | nested prototypes |
| `upvalues[]` | `{instack, index, name}` |
| `maxstack` | register count |
| `numparams` | fixed parameters |
| `is_vararg` | vararg flag |
| `lineinfo[]` | PC → source line |
| `locvars[]` | name + startpc/endpc |
| `source` | chunk name |
| `deopt_slots` | reserved descriptors for JIT value maps (may be empty in Phase 1) |
| `feedback_desc[]` | optional IC slot descriptors (Phase 2+) |

## Verifier (Phase 1+)

Before execution, optional verifier checks:

- register indices `< maxstack`
- constant indices in range
- jump targets in range
- `CLOSURE` proto index in range
- stack height discipline for CALL/RETURN/VARARG

## Versioning

- Magic: `LBC1` in any future on-disk form
- Spec version field: `0x00010000` (v0.1.0)
- In-memory only for Phase 1; disk format optional later
