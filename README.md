# Coglet

## Overview

Coglet is an experimental statically typed systems programming language with a modern, name-first syntax inspired by languages such as Jai.

Its early semantics intentionally remain close to C: explicit control, manual memory management, predictable layout, and no garbage collector. The compiler is being developed in small stages with correctness and understandable architecture taking priority over feature count.

The long-term objective is a compiler capable of compiling itself. Language design can then evolve beyond C where improvements provide clear value without sacrificing performance, control, or implementation clarity.

## Current Goals

- Build a complete, understandable compiler.
- Keep language semantics explicit and predictable.
- Maintain a clean separation between parsing, semantic analysis, and later lowering/code generation.
- Reach a stable self-hosting implementation.
- Add features incrementally without accumulating special-case behavior.

## Design Philosophy

- Statically typed.
- Manual memory management.
- No garbage collector.
- Explicit control over memory and performance.
- Familiar low-level semantics with cleaner syntax.
- Correctness and simplicity before advanced features.
- Semantic facts should be explicit enough to support later lowering, diagnostics, tooling, and code generation.

## Current Language Features

### Declarations

```c
value: i32 = 42;
other := 42;
name: u8[6] = "hello";
```

Supported declaration behavior includes:

- explicit types
- type inference with `:=`
- name-first declaration syntax
- compile-time constants
- contextual array and string initializers
- rejection of `void` as a stored value type

### Functions

```c
add::(a, b: i32) -> i32 {
    return a + b;
}
```

Supported features include:

- typed parameters
- grouped parameter declarations
- optional default parameter values
- explicit return types
- omitted return types defaulting to `void`
- argument type checking
- non-void return-path analysis
- unreachable-statement diagnostics

### Types

Primitive types:

- `bool`
- `i8`, `i16`, `i32`, `i64`
- `u8`, `u16`, `u32`, `u64`
- `f32`, `f64`
- `void`

Compound and declared types:

- pointers
- fixed-size arrays
- structs
- enums
- function types

`void` is valid as a function return type, but not as a variable, constant, parameter, struct field, pointer element, or array element type in the current language.

### Expressions

Supported expression forms include:

- numeric, boolean, character, and contextual string literals
- identifiers
- arithmetic operators
- equality and ordered comparisons
- logical operators (`&&`, `||`)
- unary negation and logical negation
- function calls
- field access
- array and pointer indexing
- explicit casts
- struct initializers
- contextual array literals

### Mutation Statements

Assignment, compound assignment, and increment/decrement are statement-only operations.

```c
x = 1;
x += 1;
x++;
```

They are valid as standalone statements and in `for` post clauses.

They are not value-producing expressions:

```c
y := (x = 1);       // invalid
takes_i32(x += 1);  // invalid
return x++;         // invalid
```

Mutation targets must denote assignable storage.

Valid targets include mutable variables and fields/indexes whose base is assignable.

Invalid targets include constants, enum members, and fields or indexes derived from temporary values.

### Void-Returning Calls

A call to a function returning `void` is valid when its result is discarded:

```c
does_nothing();
```

It is invalid in a value-required context:

```c
x := does_nothing();
takes_i32(does_nothing());
return does_nothing();
does_nothing() + 1;
```

### Arrays

```c
values: i32[3];
```

Arrays have a fixed compile-time size that is part of the type.

Supported array behavior includes:

- indexing
- assignable indexed elements when the base is assignable
- contextual array literals
- array assignment
- array arguments
- array return values
- struct fields containing arrays
- compile-time bounds checking for constant indexes

### String Literals

String literals are currently contextual initializers for fixed-size `u8` arrays.

```c
name: u8[6] = "hello";
```

The trailing null byte is included, so `"hello"` requires six bytes.

String literals currently work in expected-type contexts such as:

- variable declarations
- assignments
- function arguments
- return values
- struct field initializers

They are not yet inferred standalone expressions:

```c
name := "hello"; // invalid
"hello";         // invalid
```

### Structs

```c
Point :: struct {
    x: i32;
    y: i32;
}

p := Point {
    x = 1,
    y = 2,
};
```

Semantic analysis validates field names, duplicates, missing fields, and field initializer types.

### Enums

```c
Color :: enum(u16) {
    Red,
    Green,
    Blue,
}
```

Supported enum behavior includes:

- explicit or default integer backing types
- implicit and explicit member values
- member range validation
- enum member access
- enum comparisons for equality
- enum switch cases and exhaustiveness analysis
- explicit integer-to-enum and enum-to-integer casts where allowed

Enums are strongly typed. Values of different enum types are not interchangeable.

### Control Flow

Supported control flow includes:

- `if` / `else`
- `while`
- `for`
- `switch`
- `break`
- `continue`
- `return`
- lexical block scopes

Switch analysis includes case type checking, compile-time case validation, duplicate-case detection, duplicate-default detection, and enum exhaustiveness checks.

## Semantic Model

Semantic analysis records expression information in a side table.

Facts include:

- resolved type
- resolved symbol, when applicable
- value category: lvalue, rvalue, or none

Examples:

```text
variable expression:
    type = variable type
    category = lvalue

numeric expression:
    type = numeric type
    category = rvalue

void-returning call:
    type = void
    category = none

successful assignment statement:
    type = none
    category = none
```

This distinction is used to enforce assignability and value-required contexts.

## Memory Model

Coglet currently follows a traditional C-style memory model.

- No garbage collector.
- Manual memory management.
- Low-level pointer access.
- Predictable runtime behavior.
- Explicit control remains a core design principle.

Higher-level facilities may be added later, but they should not obscure ownership, allocation, or performance costs.

## Current Status

The parser and semantic analyzer support a substantial core language.

Current compiler work is focused on:

- semantic-info invariant verification
- deterministic semantic dumps
- auditing failed-expression fact recording
- clarifying enum-member semantic representation
- preparing the semantic model for lowering and code generation

## Roadmap

Near-term work:

1. Complete semantic side-table verification.
2. Add deterministic source-order semantic dumps.
3. Audit failed-expression paths.
4. Clarify enum-member symbol semantics.
5. Design and implement slices.
6. Continue native code generation and ABI work.

Later work:

- imports and modules
- multi-file compilation
- package visibility
- standard library
- generics
- self-hosting

## License

TBD
