Library
/roadmap.md

# Coglet Roadmap

Coglet is focused on building a small, correct systems-language core with explicit semantics and a compiler architecture that remains understandable.

The project is intentionally frontend-first. Code generation and backend selection are deferred until the intended use of the language, runtime model, and execution strategy are clearer. This avoids committing large amounts of backend code to assumptions that may later change.

## Current State

The lexer, parser, AST, compiler driver, semantic analyzer, and semantic test tooling support a substantial core language.

Implemented areas include:

- lexical scopes, symbol lookup, and shadowing
- explicit and inferred variables, parameters, functions, and constants
- primitive numeric and boolean types
- raw object pointers, fixed arrays, structs, enums, and function types
- arithmetic, comparisons, logic, calls, fields, indexes, casts, and aggregate initializers
- contextual fixed-array and null-terminated `u8` string literals
- assignment, compound assignment, and increment/decrement as statement-only mutations
- lvalue/rvalue/no-value tracking
- `if`, `while`, `for`, `switch`, `break`, `continue`, and `return`
- return-path and unreachable-statement analysis
- compile-time constant evaluation with exact signed-magnitude integers
- checked constant arithmetic, division/remainder diagnostics, and numeric representability checks
- constant array-index bounds checking
- deterministic semantic-information verification and dumps

## Recently Completed

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
- variables, assignable fields, assignable array indexes, dereferences, and pointer indexes may be addressed
- pointer indexing remains an unchecked low-level operation
- pointer arithmetic operators, array-to-pointer decay, `void*`, pointee const qualification, and lifetime checking remain unsupported
- literal zero remains the only implicit null-pointer initializer

Raw pointers are intentionally the C-interop and unsafe-memory foundation, not the final abstraction for ordinary safe Coglet APIs. Future work may add non-null references, mutable and readonly slices, read-only raw pointers, opaque pointers, and checked or unsafe conversions without changing the basic `T*` representation.

### Explicit Untyped Numeric Kinds

Numeric literals and adaptable constants now use:

```text
TYPE_UNTYPED_INT
TYPE_UNTYPED_FLOAT
```

The previous mixed state of a concrete kind plus an `is_untyped` flag has been removed. Mutable inferred variables and parameters receive concrete default types (`i32`, `i64`, `u64`, or `f64`), while inferred compile-time constants may remain adaptable.

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

Normal enums are now closed. The backing type determines representation and range, while the valid enum values are exactly the declared member values.

Current cast rules:

- enum to integer: allowed
- compile-time integer to enum: allowed only for a declared member value
- runtime integer to enum: rejected until checked runtime conversion exists

This makes enum switch exhaustiveness sound. A future `#repr_c` attribute is planned for explicit C ABI representation; it will not make enums open.

## Immediate Design Work

### 1. Runtime Numeric Cast Semantics

Compile-time casts are range-checked. Runtime narrowing still needs a deliberate rule:

- checked conversion
- wrapping/truncating conversion
- rejection unless a distinct explicit operation is used

This should be settled before a runtime or backend depends on cast behavior.

### 2. Choose the Next Language Feature by Intended Use

Slices remain a strong candidate, but implementation should wait until these rules are designed:

- mutable versus readonly slices
- pointer-and-length versus pointer-length-capacity layout
- fixed-array-to-slice conversion
- string-literal-to-byte-view conversion
- lifetime of temporary arrays and literals
- null termination and visible length

A first-class `string` type should wait until slice, ownership, mutability, encoding, and C interop rules are clearer.

### 3. C Interoperability Design

Plan, but do not yet implement syntax-only ABI promises. Relevant future work includes:

- `extern` functions
- target-aware primitive ABI types
- C-compatible struct layout
- enum backing representation
- `#repr_c` or an equivalent general representation attribute
- checked conversion for values entering closed enums from C

## Later Frontend Work

- imports and modules
- multi-file compilation
- package and visibility rules
- stable declaration identity across files
- structured diagnostics with source spans and notes
- a standard library design
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

