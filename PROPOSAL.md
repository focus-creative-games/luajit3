# New LuaJIT Architecture Proposal v0.1

**Status:** Draft  
**Audience:** Language runtime architects, compiler engineers, GC/runtime engineers, tooling engineers, performance engineers  
**Scope:** A new-from-scratch LuaJIT implementation targeting **Lua 5.3.0 and later** language semantics, with no code or design dependency on existing LuaJIT implementations.

---

# 1. Executive Summary

This document proposes the architecture for a new Just-In-Time compiled Lua runtime, hereafter **New LuaJIT**, designed from first principles to support **Lua 5.3.0+** semantics while achieving high performance, strong correctness guarantees, and long-term maintainability.

The design explicitly avoids inheriting historical constraints from prior LuaJIT implementations. Instead, it adopts a modern, layered execution architecture built around:

- a full parsing and semantic front-end,
- a register-based portable bytecode,
- a high-fidelity interpreter,
- a tiered JIT pipeline,
- explicit deoptimization,
- precise garbage collection integration,
- strong tooling, validation, and observability foundations.

The proposal prioritizes **semantic fidelity first**, **performance second**, and **operational robustness throughout**.

---

# 2. Goals and Non-Goals

## 2.1 Goals

New LuaJIT shall:

1. **Implement Lua 5.3.0+ language semantics**
   - Target language behavior aligned with official Lua releases at or above 5.3.0.
   - Preserve observable behavior for standard language constructs, coroutines, metamethods, errors, and debug-visible execution semantics within documented compatibility limits.

2. **Provide high performance through tiered execution**
   - Efficient interpreter for cold code.
   - Low-latency baseline JIT for warm code.
   - Optimizing JIT for stable hot code.

3. **Support strong correctness and recovery mechanisms**
   - Deoptimization from optimized code back to interpreter.
   - Precise safepoints and stack maps.
   - Deterministic runtime invariants and comprehensive stress modes.

4. **Enable industrial-strength engineering workflows**
   - Debuggability, profiling, IR dumping, disassembly.
   - Differential testing against official Lua.
   - Fuzzing and fault-injection support.
   - Stable internal interfaces and modular runtime/compiler boundaries.

5. **Remain portable and maintainable**
   - First-tier support for x86-64 and AArch64.
   - Clear backend abstractions.
   - No commitment to legacy ABI constraints that would block implementation quality.

## 2.2 Non-Goals

New LuaJIT does **not** initially aim to:

1. Be a drop-in ABI-compatible replacement for existing LuaJIT.
2. Reuse old trace compiler architecture.
3. Provide FFI in the first production milestone.
4. Support all possible C ABI edge cases at the cost of internal quality.
5. Maximize benchmark peak speed at the expense of correctness, debuggability, or maintainability.

---

# 3. Design Principles

The following principles guide all subsystem decisions.

## 3.1 Correctness before peak speed
Every optimization must preserve Lua semantics or be clearly documented as optional non-default behavior.

## 3.2 The interpreter is a first-class engine
The interpreter is not a bootstrap artifact. It is:
- the semantic reference engine,
- the deoptimization landing engine,
- the debug-mode engine,
- the test oracle inside the runtime.

## 3.3 All speculation must be revocable
Any JIT assumption about types, metatables, globals, shapes, or control flow must be guarded and invalidatable.

## 3.4 Deoptimization is designed from day one
No optimization tier may be introduced without:
- value maps,
- stack maps,
- bytecode position mapping,
- frame reconstruction logic.

## 3.5 Runtime observability is mandatory
Every subsystem should be inspectable through logs, counters, dumps, stress modes, and validation hooks.

## 3.6 Prefer explicit internal models over implicit host-language behavior
Examples:
- explicit Lua call frames rather than relying on native stack shape,
- explicit unwind model rather than uncontrolled exception propagation,
- explicit object metadata rather than ad hoc side tables where core invariants are involved.

## 3.7 Stage delivery to control risk
Deliver cold correctness first, then warm performance, then hot optimization, then ecosystem features.

---

# 4. Compatibility Policy

## 4.1 Compatibility Layers

Compatibility is divided into three separate commitments:

### A. Language Compatibility
Highest priority. New LuaJIT shall aim to match Lua 5.3.0+ language semantics.

### B. C API Semantic Compatibility
Important but secondary. The runtime should support the official Lua C API behavior as closely as practical.

### C. ABI Compatibility
Not a goal. The runtime may define its own ABI, object layout, stack layout, and JIT conventions.

## 4.2 Compatibility Boundaries

The following must be documented with exact behavior:

- debug API interactions with optimized code,
- finalizer scheduling details where implementation latitude exists,
- weak table and GC timing effects,
- performance-only invalidation behavior,
- any optional extensions beyond standard Lua.

---

# 5. System Architecture Overview

## 5.1 High-Level Pipeline

```text
Lua Source
  ↓
Lexer
  ↓
Parser
  ↓
AST
  ↓
Semantic Analysis / Scope Binding
  ↓
Lowering
  ↓
Portable Register Bytecode (LBC)
  ↓
Interpreter
  ↓ hotness/profile
Baseline JIT
  ↓ stability/profile
Optimizing JIT
  ↓
Native Machine Code
```

## 5.2 Major Subsystems

1. **Frontend**
   - lexical analysis
   - parsing
   - AST construction
   - semantic checks
   - debug metadata generation
   - lowering to LBC

2. **Core VM**
   - bytecode format
   - frame model
   - interpreter
   - coroutine system
   - runtime error handling
   - debug hooks

3. **Runtime Object Model**
   - tagged values
   - strings
   - tables
   - functions/closures
   - upvalues
   - userdata
   - threads
   - metatables
   - registry/environment state

4. **Memory Management**
   - object allocation
   - GC root enumeration
   - barriers
   - weak references
   - finalizers

5. **JIT Compiler**
   - hotness and profiling
   - inline caches
   - HIR/MIR/LIR
   - baseline codegen
   - optimizing compiler
   - deoptimization
   - backend per target ISA

6. **Embedding/API Layer**
   - Lua-compatible C API
   - native runtime API
   - module loading bridge

7. **Tooling/Validation**
   - differential tests
   - fuzzers
   - IR dumps
   - perf counters
   - stress modes
   - debugger/profiler hooks

---

# 6. Frontend Architecture

## 6.1 Lexer

The lexer shall:
- tokenize Lua source according to Lua 5.3.0+ rules,
- preserve source positions,
- handle long strings/comments,
- preserve raw string slices for diagnostics and optional tooling,
- distinguish identifier intern candidates early where useful.

## 6.2 Parser

A full parser shall build an explicit AST rather than emitting bytecode directly.

### Rationale
A full AST improves:
- semantic validation,
- scope analysis,
- local lifetime tracking,
- upvalue capture analysis,
- tail-call analysis,
- goto/label validation,
- source mapping,
- static transforms,
- future tooling support.

## 6.3 AST Requirements

The AST must represent at minimum:

- chunk / block
- local declarations
- assignments
- function declarations
- local functions
- if / while / repeat / numeric for / generic for
- break / goto / label / return
- calls / method calls
- table constructors
- unary / binary expressions
- variable references
- index/field access
- vararg
- literals

Each node must include:
- source span,
- parent block/function identity,
- scope or symbol references once bound,
- lowering hints as needed.

## 6.4 Semantic Analysis

Semantic analysis shall perform:

1. scope creation and symbol binding,
2. local shadowing checks,
3. upvalue capture classification,
4. break/goto legality checks,
5. label resolution,
6. function self-reference resolution,
7. vararg legality checks,
8. tail-call candidate marking,
9. constant folding where semantically safe,
10. debug name table preparation.

## 6.5 Lowering

The frontend lowers AST into **LBC**: a portable register-based bytecode.

Lowering must:
- preserve language semantics exactly,
- preserve sufficient metadata for debug, deopt, and diagnostics,
- normalize control flow,
- represent multiple returns explicitly,
- mark possible metamethod and yield sites.

---

# 7. Bytecode Design

## 7.1 Why Register-Based Bytecode

A register-based VM is preferred because it:
- reduces push/pop overhead,
- makes dataflow explicit,
- maps naturally to SSA construction,
- simplifies JIT lowering,
- aligns well with Lua semantics.

## 7.2 LBC Requirements

LBC must be:
- architecture independent,
- compact enough for practical deployment,
- rich enough to preserve full Lua semantics,
- stable enough to serve as the execution contract between frontend and VM/JIT.

## 7.3 Bytecode Categories

Representative instruction categories:

### Data movement
- `LOADNIL`
- `LOADBOOL`
- `LOADINT`
- `LOADFLOAT`
- `LOADK`
- `MOVE`

### Arithmetic and logic
- `ADD`, `SUB`, `MUL`, `DIV`, `IDIV`, `MOD`, `POW`
- `UNM`, `BNOT`, `NOT`
- comparisons and tests

### String and length
- `CONCAT`
- `LEN`

### Table operations
- `NEWTABLE`
- `GETTABLE`
- `SETTABLE`
- `GETFIELD`
- `SETFIELD`
- `GETI`
- `SETI`

### Function/closure/upvalue
- `CLOSURE`
- `GETUPVAL`
- `SETUPVAL`
- `CALL`
- `TAILCALL`
- `RETURN`
- `VARARG`

### Control flow
- `JMP`
- `TEST`
- `TESTSET`
- loop ops or lowered loop sequences

### Runtime interaction
- `SAFETYPOINT`
- `CHECKGC`
- optional explicit `META_*` lowered ops or helper markers

## 7.4 Metadata Required per Function Prototype

Each function prototype shall include:
- constant pool,
- instruction stream,
- local variable table,
- upvalue descriptors,
- line map,
- source span map,
- deopt/debug reconstruction descriptors,
- optional profile slot descriptors.

---

# 8. Runtime Value Representation

## 8.1 Chosen Initial Strategy

Version 0.1 proposes **explicit tagged values** as the baseline representation.

## 8.2 Rationale

Compared to NaN-boxing, explicit tagged values offer:

- better implementation clarity,
- easier sanitizer and debugger support,
- cleaner GC interaction,
- simpler deoptimization materialization,
- fewer platform-specific corner cases,
- lower risk during early development.

## 8.3 Representative Layout

A representative value structure:

```c
struct TValue {
    uint64_t payload;
    uint32_t type;
    uint32_t aux;
};
```

Exact layout may vary, but the design requires:
- stable type discrimination,
- room for immediate ints/bools/nil,
- pointer payload for heap objects,
- optional auxiliary metadata.

## 8.4 Value Kinds

At minimum:
- Nil
- Boolean
- Integer
- Float
- String
- Table
- Function
- Userdata
- Thread
- Lightuserdata
- Internal/native runtime refs

## 8.5 Future Alternative

NaN-boxing may be evaluated after semantic and GC stability is proven. It is not part of v0.1 architectural commitment.

---

# 9. Object Model

## 9.1 Strings

### Requirements
- cached length,
- cached hash,
- support for short-string interning,
- byte-string semantics,
- efficient equality and table-key use.

### Policy
- short strings: always interned,
- long strings: not necessarily interned by default,
- metamethod names and identifiers: strongly interned.

## 9.2 Tables

Tables are the most performance-critical runtime structure.

### Logical Structure
- array part
- hash part

### Required Capabilities
- efficient integer-key access,
- efficient string-key access,
- deletion/tombstone handling,
- predictable rehash behavior,
- metatable integration,
- support for weak semantics.

### Table Shape / Version Metadata
New LuaJIT introduces shape-like metadata to aid optimization without turning tables into JS-style objects.

Possible metadata:
- metatable pointer/version,
- structure version/epoch,
- array density statistics,
- key layout fingerprint,
- optimization class flags.

### Optimization Classes
Tables may be classified dynamically as:
- plain
- dense-array
- record-like
- megamorphic
- weak
- metamethod-observed

These classes guide inline caches and specialization.

## 9.3 Function Prototypes and Closures

### FunctionProto
Immutable compiled function metadata:
- constants
- bytecode
- nested proto refs
- debug data
- upvalue layout

### Closure
Runtime function object containing:
- proto pointer or native C target,
- upvalue array,
- type/kind metadata.

## 9.4 Upvalues

Upvalues shall have two states:

1. **Open**
   - reference a slot in a live activation.

2. **Closed**
   - own a heap-stored value after stack lifetime ends.

The VM must maintain an explicit open-upvalue list per thread.

## 9.5 Userdata

Userdata design must support:
- opaque payload ownership,
- metatable association,
- finalization hooks,
- stable embedding API references.

## 9.6 Threads / Coroutines

A thread object represents a resumable Lua execution context with:
- value stack,
- frame stack,
- open upvalue list,
- status,
- pending yield state,
- error-protection chain,
- debug/hook state,
- GC scan integration.

---

# 10. Execution Model

## 10.1 Abstract Frame Model

The runtime shall not depend on the native stack as the sole source of truth for Lua execution state.

Instead, Lua execution is represented by explicit **VM activation frames**.

Representative logical frame fields:

```text
CallFrame {
  callee
  function_proto_or_target
  base
  top
  expected_results
  caller_pc
  frame_kind
  flags
}
```

## 10.2 Frame Kinds

At minimum:
- interpreted Lua frame
- baseline JIT Lua frame
- optimizing JIT Lua frame
- C/native API frame
- continuation/resume frame
- protected-call boundary frame

## 10.3 Multiple Returns

The runtime shall model multiple return values explicitly rather than overfitting native ABI return registers.

Recommended strategy:
- a small fixed number of fast return registers/slots,
- overflow returned through a VM result area.

## 10.4 Tail Calls

Tail calls are a VM-level frame replacement operation and must not rely on host compiler tail-call optimization.

---

# 11. Interpreter Design

## 11.1 Role

The interpreter is the semantic reference implementation inside the runtime.

It must support:
- all language semantics,
- exact stack traces,
- debug hooks,
- GC safepoints,
- deopt target execution.

## 11.2 Dispatch Strategy

Preferred implementation:
- direct-threaded dispatch / computed goto where available,
- fallback switch dispatch when required.

## 11.3 Fast and Slow Paths

Each opcode handler should be structured as:
- fast path for common stable cases,
- helper or runtime slow path for uncommon/general cases.

## 11.4 Required Opcode Semantics Metadata

Each opcode must document:
- input registers,
- output registers,
- side effects,
- possible metamethod invocation,
- possible allocation,
- possible safepoint,
- possible yield,
- possible error throw.

## 11.5 Interpreter Instrumentation

Interpreter mode should expose:
- per-op counters,
- branch/loop execution counts,
- IC statistics,
- slow-path frequency,
- deopt destination counters.

---

# 12. Hotness, Profiling, and Tiering

## 12.1 Tiering Overview

New LuaJIT uses multiple execution tiers:

1. Interpreter
2. Baseline JIT
3. Optimizing JIT

## 12.2 Hotness Signals

Candidate hotness inputs:
- function entry counts,
- loop backedge counts,
- callsite execution counts,
- inline cache stability,
- polymorphism level,
- deopt rates.

## 12.3 Tiering Policy

Recommended initial policy:
- cold code starts in interpreter,
- hot function or loop enters baseline JIT,
- stable hot code graduates to optimizing JIT,
- unstable code may be blacklisted or kept in baseline.

## 12.4 Blacklisting / Cooldown

Compilation should be suppressed or delayed for:
- highly megamorphic table access,
- unstable arithmetic types,
- frequent debug hook activity,
- unsupported yield patterns in optimized code,
- repeated deoptimization without stability recovery.

---

# 13. Inline Caches and Feedback Collection

## 13.1 Importance

Inline caches are central to performance for:
- globals,
- table access,
- method dispatch,
- arithmetic specialization,
- call target specialization.

## 13.2 Feedback Slot Model

Each eligible operation site should own a feedback slot storing:
- observed operand type classes,
- table/class shape data,
- metatable versions,
- call target identities,
- hit/miss counts,
- polymorphism state.

## 13.3 IC States

Recommended states:
1. uninitialized
2. monomorphic
3. polymorphic
4. megamorphic generic

## 13.4 Invalidation Sources

Any speculation may be invalidated by:
- metatable mutation,
- table shape mutation,
- global environment mutation,
- debug hook enablement,
- runtime configuration changes affecting semantics.

---

# 14. JIT Compiler Architecture

## 14.1 Tiered JIT Structure

New LuaJIT shall implement:
- **Baseline JIT** for fast compilation and moderate speedup,
- **Optimizing JIT** for hot stable code.

## 14.2 Baseline JIT

### Objectives
- low compilation latency,
- direct lowering from LBC/HIR,
- minimal speculation,
- straightforward deopt integration,
- significant interpreter speedup.

### Likely Features
- linear-scan register allocation,
- inline cache expansion,
- fast-path arithmetic,
- efficient call sequence lowering,
- safepoint polls,
- stack maps.

## 14.3 Optimizing JIT

### Objectives
- exploit stable types and shapes,
- reduce dynamic dispatch,
- optimize loops and calls,
- preserve deopt correctness.

### Likely Features
- SSA-based MIR,
- type propagation,
- constant folding,
- CSE,
- dead code elimination,
- LICM,
- guarded inlining,
- scalar replacement where safe,
- specialized table access lowering.

## 14.4 Compilation Units

Initial compilation units should be:
- individual Lua functions,
- optionally with hot-loop specialization inside them.

Cross-function compilation may evolve later through inlining, but full-module whole-program optimization is not a v0.1 requirement.

---

# 15. Intermediate Representation Design

## 15.1 IR Layers

The architecture defines three major internal IR layers.

### HIR
High-level IR preserving Lua semantics:
- dynamic operations,
- calls,
- varargs,
- metamethod-sensitive ops,
- guard/safepoint placeholders.

### MIR
SSA-based, typed, control-flow explicit representation for optimization.

### LIR / Target IR
Lowered, target-aware representation used for final register allocation and code emission.

## 15.2 HIR Characteristics

HIR should still express semantically rich operations such as:
- `GetTable`
- `SetTable`
- `Call`
- `Return`
- `VarArg`
- `LoadUpvalue`
- `StoreUpvalue`
- `GuardType`
- `GuardShape`
- `GuardMetaVersion`
- `Safepoint`

## 15.3 MIR Characteristics

MIR must provide:
- SSA values,
- explicit CFG,
- explicit memory effects,
- deopt metadata attachment,
- typed or refined type lattice values,
- explicit runtime helper call nodes where needed.

## 15.4 MIR Type Lattice

Representative value classes:
- Any
- Nil
- Bool
- Int
- Float
- Number
- String
- Table
- Function
- Closure
- Thread
- Userdata
- InternalRef

Refined speculative classes:
- Table[plain]
- Table[array-dense]
- String[short]
- Function[lua]
- Function[c]
- Number[int-stable]

## 15.5 LIR Characteristics

LIR includes:
- physical or virtual register classes,
- stack slots,
- spills/reloads,
- target branch forms,
- addressing modes,
- call ABI mapping,
- patchable call/jump stubs.

---

# 16. Speculation, Guards, and Deoptimization

## 16.1 Core Requirement

Any optimized code that assumes:
- types,
- shapes,
- metatables,
- call targets,
- control-flow structure,
must be guarded.

## 16.2 Guard Types

Representative guards:
- value tag/type guard,
- integer/float kind guard,
- table shape/version guard,
- metatable pointer/version guard,
- callee identity guard,
- environment version guard.

## 16.3 Deopt Metadata

Each deopt point must retain:
- originating bytecode PC,
- active inlined frame chain if any,
- live value location map,
- materialization recipes for elided values,
- target interpreter state reconstruction info.

## 16.4 Deopt Triggers

Deoptimization may occur on:
- guard failure,
- unsupported runtime event,
- debug hook activation,
- explicit runtime invalidation,
- GC/runtime state requiring fallback,
- unsupported yield boundary crossing.

## 16.5 Deopt Correctness Rule

After deopt, execution must continue in the interpreter as if the optimized code had never violated Lua semantics.

---

# 17. Metamethod Semantics and Optimization

## 17.1 Design Principle

Metamethods are not exceptional behavior. They are part of the normal semantic model.

## 17.2 Fast Path Model

For operations that may trigger metamethods:
- fast path executes only if guard confirms no special handling is needed,
- slow path calls runtime helpers,
- optimized tiers may guard on stable metamethod identity.

## 17.3 Optimizable Cases

Examples:
- fixed metatable with no relevant metamethod,
- stable `__index` table path,
- stable `__call` target,
- stable arithmetic metamethod closure.

These may be specialized via IC or guarded inlining.

## 17.4 Invalidation Discipline

Metatable mutation or related structure changes must invalidate all affected assumptions conservatively.

---

# 18. Coroutines, Yield, and Resume

## 18.1 Coroutine Model

Coroutines are first-class VM execution contexts.

## 18.2 Required Semantics

The runtime must support:
- yielding from valid Lua contexts,
- resuming into interpreter or JIT frames,
- correct interaction with protected calls,
- stack trace preservation,
- correct upvalue behavior across yield/resume.

## 18.3 Initial Optimization Policy

Version 0.1 may conservatively deopt when:
- optimized frames approach unsupported yield boundaries,
- runtime state becomes too complex for safe optimized continuation.

This is acceptable if behavior remains correct.

## 18.4 Resume Mechanics

Resumption should reconstruct or continue a VM frame chain explicitly; it must not depend on fragile native stack continuation assumptions alone.

---

# 19. Errors and Unwinding

## 19.1 Error Model

Lua-style `error` / `pcall` / `xpcall` semantics require controlled unwinding across interpreter, JIT, and C boundaries.

## 19.2 Recommended Internal Model

Use:
- explicit protected-call boundaries,
- explicit unwind descriptors,
- runtime-managed error objects,
- controlled fallback from native frames as needed.

## 19.3 Host Mechanism

The embedding layer may use `setjmp/longjmp`-style protection at selected boundaries, but internal VM execution should prefer explicit structured unwinding state where practical.

## 19.4 Requirements

Unwinding must:
- preserve stack trace information,
- cleanly close upvalues,
- honor protected-call semantics,
- interact correctly with finalization and hooks.

---

# 20. Garbage Collection Architecture

## 20.1 Initial GC Strategy

Version 0.1 proposes:
- **precise, non-moving, incremental mark-sweep GC**

Version roadmap:
- v1: precise incremental non-moving collector
- v2: generational extension
- v3: optional selective compaction for suitable object classes if justified

## 20.2 Rationale

A non-moving collector reduces complexity for:
- JIT embedded references,
- deopt metadata,
- C API compatibility,
- userdata/FFI future constraints,
- debugging and observability.

## 20.3 Required Capabilities

The GC must support:
- precise root scanning,
- incremental progress,
- weak references,
- finalizers,
- write barriers,
- safepoint integration,
- code cache and metadata scanning.

## 20.4 Roots to Scan

At minimum:
- thread value stacks,
- frame stacks,
- open upvalues,
- global/registry roots,
- interned strings table,
- weak tables and ephemeron structures as required,
- JIT metadata roots,
- machine-code embedded object references,
- pending exception or resume state.

## 20.5 Write Barriers

The runtime must provide barriers for:
- old-to-young references once generations exist,
- black-to-white references during incremental marking.

## 20.6 Allocation Strategy

Recommended:
- arena or segregated allocation classes by object type/size,
- efficient fast allocation path,
- allocation-triggered safepoint/GC polling.

## 20.7 Finalization

Finalizer execution must be controlled carefully:
- clearly ordered relative to collection phases,
- resilient to object resurrection,
- semantically documented,
- integrated with debug and error reporting.

---

# 21. Safepoints and Stack Maps

## 21.1 Safepoint Requirements

Safepoints are required at:
- interpreter loop boundaries,
- function call boundaries,
- backward branches,
- allocation slow paths,
- JIT poll locations,
- runtime helper entry/exit where necessary.

## 21.2 Stack Maps

All JIT code that can be interrupted for GC or deopt must carry stack maps indicating:
- live references,
- value locations,
- frame reconstruction anchors.

## 21.3 Polling Strategy

A lightweight polling mechanism should check:
- pending GC work,
- invalidation requests,
- debug hook requests,
- async runtime control flags if any.

---

# 22. Native Code Generation and Backend Strategy

## 22.1 Initial Target Platforms

Tier-1 targets:
- x86-64
- AArch64

## 22.2 Backend Strategy

Recommended plan:
- self-defined HIR/MIR/LIR pipeline,
- self-owned baseline codegen backend,
- optimizing backend may remain self-owned or later integrate selected external support if justified.

## 22.3 Rationale Against Heavy Initial Dependencies

A heavy external backend can harm:
- JIT latency,
- runtime integration simplicity,
- deopt control,
- maintenance independence.

## 22.4 Backend Responsibilities

Each target backend must implement:
- calling convention lowering,
- register classes,
- stack frame layout,
- branches and patching,
- safepoint poll insertion,
- stack map encoding,
- constant/reference materialization,
- deopt exit trampolines.

---

# 23. C API and Embedding Model

## 23.1 Compatibility Position

New LuaJIT should provide a Lua-compatible embedding API where practical, but not promise ABI compatibility with existing implementations.

## 23.2 API Layers

### Layer 1: Lua-Compatible C API
For standard embedders and modules expecting official Lua API semantics.

### Layer 2: Native Runtime API
A modern API with:
- handle-based references,
- safer lifetime control,
- explicit yield-safe calls,
- lower-overhead typed operations.

### Layer 3: Optional Future FFI
Not part of initial milestone.

## 23.3 Why ABI Compatibility Is Not Required

Avoiding ABI lock-in enables:
- better value representation,
- explicit frame design,
- better GC evolution,
- cleaner JIT integration,
- more robust deopt and debug support.

---

# 24. FFI Position

## 24.1 Initial Decision

FFI is explicitly **out of scope** for the first production architecture milestone.

## 24.2 Rationale

FFI significantly increases complexity in:
- ABI handling,
- callback support,
- structure layout,
- GC pinning,
- exception/yield boundary behavior,
- portability.

## 24.3 Future Path

Recommended future sequence:
1. stable C module API,
2. limited foreign function call support,
3. struct/union support,
4. callback support,
5. advanced JIT-integrated FFI optimization.

---

# 25. Debugging, Introspection, and Tooling

## 25.1 Mandatory Debug Metadata

Each function/proto must support:
- line mapping,
- source spans,
- local variable names and ranges,
- upvalue names,
- inline/deopt frame reconstruction mapping.

## 25.2 Debugging Modes

Recommended execution policies:
- full debug mode may prefer interpreter or baseline only,
- enabling certain hooks may invalidate optimizing code,
- source-correlated stack traces are mandatory.

## 25.3 Internal Tooling

The runtime should expose:
- bytecode dump,
- IR dumps (HIR/MIR/LIR),
- disassembly,
- deopt logs,
- IC state dumps,
- GC statistics,
- hotness/tiering counters.

## 25.4 Profiling

The system should support:
- opcode profile,
- callsite profile,
- allocation profile,
- GC pause/profile stats,
- native code perf symbol mapping where available.

---

# 26. Testing and Validation Strategy

## 26.1 Correctness Strategy

Correctness must be validated through multiple layers.

### A. Conformance Testing
Against official Lua test suites and language behavior references.

### B. Differential Testing
Execute the same programs under:
- official Lua,
- New LuaJIT interpreter,
- New LuaJIT baseline JIT,
- New LuaJIT optimizing JIT.

Compare:
- outputs,
- errors,
- stack traces,
- observable semantics.

### C. Fuzzing
Target:
- parser,
- lowering,
- bytecode verifier if any,
- table/metatable interactions,
- coroutine/error/GC combinations,
- JIT invalidation/deopt paths.

### D. Stress Modes
Required stress knobs include:
- force GC at every safepoint,
- force deopt frequently,
- disable ICs randomly,
- limit code cache,
- tier up/down aggressively,
- poison speculative paths.

## 26.2 Performance Validation

Benchmark groups should include:
- microbenchmarks,
- allocation-heavy workloads,
- table-heavy workloads,
- coroutine-heavy workloads,
- real-world Lua application suites.

---

# 27. Security and Robustness Considerations

## 27.1 Memory Safety

Where implementation language allows, use:
- hardened allocators or allocator diagnostics,
- bounds checks in debug builds,
- poisoning/fill in debug allocators,
- sanitizer-friendly code paths.

## 27.2 JIT Safety

The JIT must:
- maintain W^X discipline where supported,
- separate code generation and execution permissions safely,
- validate patch points and relocation metadata,
- avoid use-after-free in code cache invalidation paths.

## 27.3 Untrusted Code Considerations

If the runtime is used for sandbox-like workloads, additional policy layers may be required for:
- module loading,
- host function exposure,
- memory quotas,
- instruction/CPU quotas.

These are deployment concerns, not core architectural guarantees.

---

# 28. Deployment and Code Cache Strategy

## 28.1 Initial Position

v0.1 does not require persistent code cache.

## 28.2 In-Memory Code Cache

The runtime should maintain:
- code pages by tier/target,
- metadata side tables,
- invalidation lists,
- reclamation policy for stale code.

## 28.3 Future Extensions

Potential later additions:
- AOT bytecode cache,
- profile-guided reoptimization,
- persisted native cache with strict versioning.

---

# 29. Implementation Roadmap

## Phase 0: Specification and Design Freeze
Deliverables:
- compatibility charter,
- bytecode spec,
- frame model spec,
- GC design note,
- JIT IR design note,
- invalidation/deopt spec,
- initial benchmark/test plan.

## Phase 1: Correctness Core
Implement:
- lexer/parser/AST,
- semantic analysis,
- LBC generation,
- object model,
- interpreter,
- basic precise incremental GC,
- coroutine core,
- error/protected call model,
- debug metadata basics.

Success criteria:
- broad Lua conformance without JIT,
- stable runtime invariants,
- passing regression suite.

## Phase 2: Warm Performance
Implement:
- hotness counters,
- inline caches,
- baseline JIT,
- deopt infrastructure,
- stack maps,
- safepoint integration.

Success criteria:
- meaningful speedup on warm workloads,
- stable fallback behavior,
- no semantic regressions.

## Phase 3: Hot Optimization
Implement:
- SSA MIR,
- optimizing compiler,
- guarded specialization,
- inlining,
- LICM/CSE/DCE/const-prop,
- loop optimizations,
- more advanced table specialization.

Success criteria:
- strong gains on hot stable code,
- manageable deopt rates,
- benchmark competitiveness.

## Phase 4: Ecosystem Maturity
Implement:
- stronger C API coverage,
- production-grade loader/module behavior,
- advanced profiling/debugging,
- improved diagnostics and observability.

## Phase 5: Advanced Features
Potential features:
- generational GC,
- background compilation,
- persistent caches,
- optional FFI,
- selective compaction,
- PGO.

---

# 30. Team Structure Recommendations

A project of this scope should split ownership approximately as follows:

## 30.1 Frontend Team
- lexer/parser
- AST
- semantic analysis
- lowering
- source/debug metadata

## 30.2 Runtime Team
- values
- strings
- tables
- closures/upvalues
- threads/coroutines
- embedding API

## 30.3 Memory/GC Team
- allocator
- GC phases
- barriers
- weak/finalizer behavior
- stress and diagnostics

## 30.4 VM Team
- bytecode semantics
- interpreter
- frame model
- error/unwind model

## 30.5 Compiler/JIT Team
- profiling
- IC system
- HIR/MIR/LIR
- baseline backend
- optimizing passes
- deopt and stack maps

## 30.6 Validation/Tooling Team
- differential testing
- fuzzing
- benchmark infrastructure
- dump/disasm tools
- perf dashboards

---

# 31. Open Questions for v0.1

The following questions remain to be resolved explicitly in follow-on design notes:

1. Final Lua 5.3.0+ compatibility matrix by subsystem.
2. Exact LBC binary encoding format.
3. Exact TValue layout and pointer tagging opportunities.
4. Table shape/version mechanism specifics.
5. Debug API behavior under optimized execution.
6. Yield support envelope inside optimized code.
7. Code cache eviction policy.
8. Whether optimizing backend is fully self-hosted from the start.
9. Long-string interning heuristics.
10. Initial scope of native runtime API.

---

# 32. Recommended Initial Technical Decisions

For clarity, v0.1 recommends the following concrete initial defaults:

- **Frontend:** full AST + semantic lowering
- **Execution IR:** register-based portable bytecode
- **Interpreter:** direct-threaded if supported
- **Values:** explicit tagged value representation
- **GC:** precise non-moving incremental mark-sweep
- **JIT:** baseline JIT + optimizing JIT
- **Optimization model:** feedback-driven specialization with guards and deopt
- **Table optimization:** array/hash with shape/version metadata
- **Compatibility:** language first, C API second, ABI not promised
- **FFI:** deferred
- **Tier-1 platforms:** x86-64 and AArch64

---

# 33. Conclusion

New LuaJIT should be built as a modern language runtime platform rather than a narrowly optimized benchmark engine. The recommended architecture emphasizes:

- faithful Lua semantics,
- strong interpreter foundation,
- tiered JIT with explicit deoptimization,
- GC and runtime correctness,
- maintainable and observable engineering structure,
- staged performance evolution.

This architecture is designed to support both near-term delivery and long-term extensibility. If executed with discipline, it can become a robust, high-performance Lua implementation suitable for production systems and future compiler/runtime research alike.

---

如果你愿意，我下一步可以继续补下面这些配套文档之一：

1. **《New LuaJIT Bytecode Specification v0.1》**
2. **《New LuaJIT IR Design: HIR / MIR / LIR v0.1》**
3. **《New LuaJIT GC & Memory Management Design v0.1》**
4. **《New LuaJIT Table/Object Model Design v0.1》**
5. **《New LuaJIT Deoptimization & Safepoint Design v0.1》**
6. **《New LuaJIT 12-Month Execution Plan》**
7. **《New LuaJIT Risk Register and Technical Milestones》**

如果你要，我建议下一份先写 **Bytecode Specification**，因为它会把前端、解释器、JIT、deopt 的接口统一下来。