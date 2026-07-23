# Coglet Roadmap

Coglet is focused on building a small, correct systems-language core with explicit semantics and a compiler architecture that remains understandable.

The project is intentionally frontend-first. Code generation and backend selection are deferred until the intended use of the language, runtime model, and execution strategy are clearer. This avoids committing large amounts of backend code to assumptions that may later change.

## Current State

The lexer, parser, AST, compiler driver, semantic analyzer, and semantic test tooling support a substantial core language.

Implemented areas include:

- lexical scopes, symbol lookup, and shadowing
- explicit and inferred variables, parameters, functions, and constants
- canonical primitive numeric and boolean semantic types
- raw object pointers with dedicated `null`, fixed arrays, nominal structs, nominal enums, and function types
- arithmetic, bitwise operations, shifts, comparisons, logic, calls, fields, indexes, casts, and aggregate initializers
- contextual fixed-array and null-terminated `u8` string literals
- assignment, arithmetic/bitwise/shift compound assignment, and increment/decrement as statement-only 
mutations
- lvalue/rvalue/no-value tracking
- `if`, `while`, `for`, `switch`, `break`, `continue`, and `return`
- definite-assignment analysis for locals and parameters
- unified reachability for branches, switches, loops, returns, unreachable statements, and non-void 
fallthrough
- value-based Boolean and enum switch exhaustiveness
- nested functions without closure capture
- compile-time constant evaluation with exact signed-magnitude integers
- checked constant integer arithmetic, known zero-divisor diagnostics, and numeric representability 
- checks
- constant array-index bounds checking
- IEEE-754 constant behavior for `f32` and `f64`, including infinity, NaN, and signed zero
- deterministic semantic-information verification and dumps

## Recently Completed

### Definite Assignment and Unified Reachability

Coglet now tracks whether each function-local variable and parameter is definitely initialized at every 
reachable program point.

Completed behavior includes:

* local declarations without initializers remain uninitialized
* parameters and successfully initialized locals begin initialized
* direct whole-variable assignment initializes its target
* whole-struct and whole-array assignment initializes the complete variable
* field, element, pointer-index, and dereference writes do not initialize a complete base variable
* compound assignment and increment/decrement require prior initialization
* taking the address of an uninitialized local is rejected
* `if` branches are checked independently and merged by intersecting continuing paths
* omitted `else` branches preserve the unchanged incoming path
* switch cases begin from independent copies of the incoming state
* non-exhaustive switches include an implicit no-match path
* loops conservatively preserve the possibility of zero iterations
* `break` and `continue` flow targets the nearest loop
* `continue` and body-fallthrough paths reach a `for` post expression
* `break` and `return` paths do not reach a `for` post expression
* literal-true loops with no reachable `break` are non-continuing
* unreachable statements are diagnosed during block traversal
* a non-void function is accepted whenever normal control flow cannot reach the end of its body

Path-dependent initialization is stored in a separate `FlowState` rather than as mutable state on symbols.

Each tracked variable has an owner-qualified flow identity:

```text
(flow owner ID, variable ID)
```

This prevents nested functions from accidentally consulting an enclosing function's numerically identical 
variable slot.

Nested functions do not currently support closure capture. They may access visible globals, constants, 
types, and functions, but references to enclosing locals and parameters are rejected.

Switch exhaustiveness is based on successfully checked runtime values:

* `default` covers every value
* Boolean switches require both `true` and `false`
* enum switches require every distinct declared runtime value
* enum aliases with the same backing value require only one case
* invalid cases never contribute coverage

The older separate return-path and unreachable-analysis helpers have been removed. `FlowState.reachable` is now the single control-flow source of truth.


### Semantic Type and Numeric Hardening

The existing frontend semantics have been hardened without adding backend or
runtime execution work.

Completed areas include:

- canonical shared semantic instances for concrete built-in scalar types
- exhaustive structural type equality with declaration identity for structs and enums
- dedicated `null` semantics with no integer-zero pointer conversion
- direct diagnostics for `*null` and integer zero in pointer contexts
- explicit `null`-to-pointer casts
- equality restricted to supported value categories
- numeric-only ordered comparisons
- rejection of typed unsigned unary negation
- checked compile-time integer overflow and underflow
- shared known integer zero-divisor checks for binary and compound assignment
- IEEE-754 constant behavior for `f32` and `f64`
- correct unordered NaN comparisons and signed-zero handling
- expanded valid, invalid, constant-oracle, snapshot, and semantic-info coverage

Runtime integer overflow, narrowing-cast behavior, and explicit wrapping
operations remain design decisions rather than implied semantics.

### Raw Object Pointers

Coglet now supports the first low-level pointer milestone:

```c
x: i32 = 10;
p: i32* = &x;
*p = 20;
```

Current rules:

- `T*` is a raw, nullable pointer to mutable `T`
- `&expression` requires a mutable lvalue and produces a `T*` rvalue
- `*expression` requires `T*` and produces a `T` lvalue
- variables, assignable fields, assignable array indexes, dereferences, and pointer indexes may be 
addressed
- pointer indexing remains an unchecked low-level operation
- pointer arithmetic operators, array-to-pointer decay, `void*`, pointee const qualification, and 
lifetime checking remain unsupported
- `null` is the only source-level null-pointer value; integer zero is rejected in pointer contexts
- `null` may contextually adapt to a concrete pointer type or be explicitly cast to one

Raw pointers are intentionally the C-interop and unsafe-memory foundation, not the final abstraction 
for ordinary safe Coglet APIs. Future work may add non-null references, mutable and readonly slices, 
read-only raw pointers, opaque pointers, and checked or unsafe conversions without changing the basic `T*` 
representation.

### Explicit Untyped Numeric Kinds

Numeric literals and adaptable constants now use:

```text
TYPE_UNTYPED_INT
TYPE_UNTYPED_FLOAT
```

The previous mixed state of a concrete kind plus an `is_untyped` flag has been removed. 
Mutable inferred variables and parameters receive concrete default types (`i32`, `i64`, `u64`, or `f64`), 
while inferred compile-time constants may remain adaptable.

### Semantic-Information Verifier

The standalone verifier now:

- walks expression-containing AST locations in source order
- verifies one semantic entry per successful expression or mutation
- detects duplicate and orphan side-table entries
- checks value categories and symbol/type consistency
- distinguishes mutation nodes from void-returning calls
- rejects untyped numeric variable and parameter symbols
- prints deterministic semantic tables with `--dump-semantic-info`

### Closed Enums

Normal enums are now closed. The backing type determines representation and range, 
while the valid enum values are exactly the declared member values.

Current cast rules:

- enum to integer: allowed
- compile-time integer to enum: allowed only for a declared member value
- runtime integer to enum: rejected until checked runtime conversion exists

This makes enum switch exhaustiveness sound. A future `#repr_c` attribute is planned for explicit C ABI representation; it will not make enums open.

### Bitwise and Shift Operators

Coglet now supports the integer bit-manipulation core:

- unary `~`
- binary `&`, `|`, and `^`
- `<<` and `>>`
- `&=`, `|=`, `^=`, `<<=`, and `>>=`
- integer-only operand checking
- contextual untyped integer adaptation
- exact concrete type matching for binary bitwise operations
- left-operand result typing for shifts
- statically known shift-count range diagnostics
- fixed-width truncating left shift
- zero-filling unsigned right shift
- arithmetic signed right shift
- compile-time evaluation using explicit width-limited bit patterns
- lexer, parser, semantic, diagnostic, constant-oracle, and semantic-info tests

Coglet intentionally gives bitwise operators higher precedence than equality
and ordered comparison, so `flags & mask == 0` means `(flags & mask) == 0`.

## Candidate Next Design Work

The definite-assignment and unified-reachability milestone is complete. 
The following items are candidate frontend design areas rather than a committed implementation sequence. 
Code generation remains deferred.

### 1. Runtime Integer Arithmetic and Numeric Casts

Compile-time integer operations and casts are checked. Runtime behavior still
needs deliberate rules for:

- overflow and underflow
- signed division overflow
- narrowing conversion
- checked versus wrapping operations
- whether wrapping requires distinct syntax or built-ins

No backend should assume C-style wrapping or undefined behavior before this is
settled.

### 2. Readonly and Opaque Raw Pointers

Design a small const-correctness mechanism without conflating:

- immutable bindings
- non-reassignable pointer variables
- readonly pointees
- deep immutability
- compile-time constants
- safe references or lifetime-checked borrows

A future readonly raw pointer should remain nullable, non-owning, unchecked,
and potentially dangling. Opaque raw pointers should be considered separately
for future `void*`-style interoperation.

### 3. C Interoperability Design

Plan, but do not yet implement syntax-only ABI promises. Relevant future work
includes:

- external declarations
- target-aware primitive ABI types
- C-compatible struct layout
- opaque pointer conversion rules
- enum backing representation
- `#repr_c` or an equivalent general representation attribute
- mapping C null pointers to Coglet `null`

### 4. Slices and Pointer-Length Views

Slices remain a strong candidate after raw-pointer mutability and ABI rules
are clearer. Their design must settle:

- mutable versus readonly slices
- pointer-and-length versus pointer-length-capacity layout
- fixed-array-to-slice conversion
- string-literal-to-byte-view conversion
- lifetime of temporary arrays and literals
- null termination and visible length

A first-class `string` type should wait until slice, ownership, mutability,
encoding, and C interoperability rules are clearer.

## Later Frontend Work

- imports and modules
- multi-file compilation
- package and visibility rules
- stable declaration identity across files
- structured diagnostics with source spans and notes
- a standard library design
- closure and capture semantics, only if nested runtime functions require them
- generics, if justified by real use cases

## Deferred Execution Work

No backend is selected yet. Possible later strategies include:

- an interpreter
- a C transpilation backend
- a custom IR and native backend
- LLVM or another existing backend

The choice should be driven by what Coglet is intended to become. Backend work should begin only when enough language and runtime decisions are stable that the implementation is unlikely to be discarded.

## Self-Hosting Direction

Self-hosting remains a long-term objective rather than the next milestone. The language still needs runtime and file I/O facilities, allocation support, diagnostics suitable for larger programs, and some form of multi-file compilation before self-hosting becomes practical.
