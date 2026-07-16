# Coglet Roadmap

Coglet is focused on building a small, correct, modern systems-language core with a clean compiler architecture.

The priority is correctness over convenience. Features should be added in small, well-defined stages so the compiler can grow without accumulating technical debt or special-case behavior.

## Current Focus: Semantic Model and Compiler Invariants

The parser and semantic analyzer now cover a substantial core language surface.

Implemented language and semantic features include:

- expressions, declarations, functions, structs, enums, switches, casts, loops, and block scopes
- lexical scope resolution and symbol lookup
- explicit and inferred variable declarations
- constants and compile-time constant evaluation
- primitive numeric, boolean, pointer, array, struct, enum, and function types
- arithmetic, comparison, logical, unary, field-access, call, index, and cast expressions
- plain assignment, compound assignment, and prefix/postfix increment and decrement
- statement-only mutation semantics
- lvalue tracking for variables, fields, and indexes
- rejection of constants, enum members, and temporary-derived fields/indexes as mutation targets
- `if`, `while`, `for`, `switch`, `break`, `continue`, and `return`
- switch case validation, duplicate detection, and enum exhaustiveness checks
- non-void return-path analysis and unreachable-statement diagnostics
- explicit cast validation and compile-time integer range checks
- constant array-index bounds checking
- contextual array literals
- contextual string literals for fixed-size `u8` arrays
- expected-type propagation for array and string literals in declarations, assignments, arguments, returns, and struct field initializers
- integer-only `%` and `%=` behavior
- semantic side-table facts for expression type, resolved symbol, and value category
- explicit distinction between value-producing expressions and no-value expressions
- void-returning calls accepted in statement position and rejected in value-required contexts
- Compile-time-known integer and enum casts are range-checked consistently
  across all expression contexts.
- Runtime narrowing-cast semantics still need a deliberate language decision:
  checked conversion, truncating/wrapping conversion, or rejection.
## Recently Completed: Mutation and Void-Expression Semantics

Mutation operators are statement-only operations.

The following are valid in statement position:

```c
x = 1;
x += 1;
x++;
```

They are also valid in a `for` post clause.

They are rejected in value-required contexts such as:

```c
y := (x = 1);
takes_i32(x += 1);
return x++;
```

Successful mutation nodes are recorded in semantic information as having no value:

```text
type = none
value category = none
```

Void-returning calls are represented differently:

```text
type = void
value category = none
```

This allows:

```c
does_nothing();
```

while rejecting:

```c
x := does_nothing();
takes_i32(does_nothing());
return does_nothing();
does_nothing() + 1;
```

## Current Compiler-Architecture Work

The next architectural work should strengthen semantic information and verification.

### 1. Semantic-Information Invariant Verification

Build a complete source-order AST verifier that checks expression facts after successful semantic analysis.

The verifier should cover all expression-containing AST locations, including:

- declarations and initializers
- function arguments and parameter defaults
- struct initializers
- array literals
- switch expressions and case values
- loop conditions and post clauses
- casts, fields, indexes, calls, unary expressions, and binary expressions

The verifier should distinguish:

- successful value expressions
- successful no-value expressions
- statement-only mutation nodes
- nodes that failed semantic analysis and therefore have no successful fact

### 2. Deterministic Semantic Dumps

The current linked semantic-info list is useful for debugging but reflects reverse insertion order.

Add a deterministic AST-driven semantic dump in source order. It should print, for every expression:

- source location
- expression kind
- resolved type
- resolved symbol, when present
- value category
- whether the expression produces a usable value

### 3. Failed-Expression Fact Auditing

Audit error paths so failed parent expressions do not retain misleading successful facts.

Areas to review include:

- struct initializers
- out-of-bounds indexes
- invalid casts
- failed field/index access
- failed contextual initializers
- invalid switch case expressions

Child expressions that checked successfully may retain facts even when their parent fails.

### 4. Enum Member Symbol Semantics

Clarify and standardize whether enum member field expressions should resolve to:

- the enum type symbol
- a dedicated enum-member symbol
- no symbol, with only enum type and compile-time value metadata

The final representation should support later lowering, debugging, tooling, and code generation without ambiguity.

## Next Language Milestone: Slices

Fixed-size array and contextual string-literal semantics are now implemented.

The next major language-design milestone is slices.

Possible future syntax:

```c
bytes: []u8;
```

The exact syntax and mutability model should be decided before implementation.

Open design questions:

- Should mutable and readonly slices be distinct types?
- Should string literals coerce only to readonly byte slices?
- Should a string-literal-to-slice conversion include the trailing null byte in the visible length?
- Should slices contain only pointer and length, or also capacity?
- Can fixed-size arrays implicitly convert to slices?
- How are temporary arrays and string literals kept alive when viewed through slices?

## Later String Direction

A first-class `string` type should be considered only after slice, mutability, ownership, encoding, and interop rules are defined.

Open design questions:

- Is `string` a builtin type or standard-library type?
- Is it always UTF-8?
- Is it a readonly view or an owned allocation?
- Does it include or guarantee a trailing null byte?
- How does it interoperate with C APIs?
- Are indexing operations byte-based, code-point-based, or unavailable?

## Later Language and Compiler Work

After the semantic model and slice design are stable:

- imports
- modules
- multi-file compilation
- package and visibility rules
- more complete native code generation
- ABI and C interop
- standard library design
- generics
- compiler self-hosting

Imports and modules should wait until core semantic facts, type identity, symbol visibility, file loading, and compilation-unit boundaries are stable.
