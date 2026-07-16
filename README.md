# Coglet

## Overview

This project is an experimental systems programming language with a
modern, name-first syntax inspired by languages such as Jai, while
intentionally keeping its semantics close to C during its early
development.

The primary objective is to build a small, understandable compiler
capable of compiling itself. Once the language is self-hosting, the
design will gradually evolve beyond C where improvements make sense
without sacrificing performance or explicit control.

## Current Goals

-   Build a complete compiler.
-   Remain close to C semantics while the language matures.
-   Keep the implementation simple and easy to reason about.
-   Reach a stable self-hosting compiler.
-   Evolve the language after bootstrapping.

## Design Philosophy

-   Statically typed.
-   Manual memory management (no garbage collector).
-   Explicit control over memory and performance.
-   Familiar to C programmers while using cleaner syntax.
-   Simplicity before advanced language features.

## Current Features

### Declarations

``` text
value: i32 = 42;
name: u8[6] = "Hello";
```

-   Explicit type declarations.
-   Type inference using `:=`.
-   Name-first declaration syntax.

### Procedures

``` text
add::(a, b: i32) -> i32 {
    return a + b;
}
```

Supports:

-   Typed parameters.
-   Grouped parameter declarations.
-   Optional default parameter values.
-   Optional return types (defaults to `void`).

### Types

Primitive types:

-   bool
-   i8, i16, i32, i64
-   u8, u16, u32, u64
-   f32, f64
-   void

Compound types:

-   Pointers
-   Arrays
-   Structs

### Expressions

-   Arithmetic operators
-   Comparison operators
-   Logical operators (`&&`, `||`)
-   Assignment and compound assignment
-   Prefix and postfix increment/decrement
-   Function calls
-   Field access
-   Array indexing

### Control Flow

-   if / else
-   while
-   for
-   break
-   continue
-   return
-   Block scopes

## Memory Model

The language currently follows a traditional C-style memory model.

-   No garbage collector.
-   Manual memory management.
-   Low-level access through pointers.
-   Predictable runtime behaviour.

Future revisions may introduce higher-level facilities, but explicit
memory control will remain a core principle.

## Current Status

The language is actively under development.

The focus is on establishing a solid core language and compiler before
introducing more advanced features.

## Roadmap

### Phase 1

-   Complete parser and semantic analysis.
-   Type checking.
-   Native code generation.
-   Basic standard library.

### Phase 2

-   Rewrite the compiler in the language itself.
-   Reach a self-hosting compiler.

### Phase 3

-   Refine the language.
-   Improve ergonomics.
-   Introduce more advanced features where they provide clear benefits
    without compromising performance or explicit control.

## License

TBD.
