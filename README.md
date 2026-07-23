# Coglet

## Overview

Coglet is an experimental statically typed systems programming language with a modern, name-first syntax inspired by languages such as Jai.

Its early semantics intentionally remain close to C: explicit control, manual memory management, predictable layout, and no garbage collector. The compiler is being developed in small stages with correctness and understandable architecture taking priority over feature count.

The long-term objective is a compiler capable of compiling itself. Language design can then evolve beyond C where improvements provide clear value without sacrificing performance, control, or implementation clarity.

## Current Goals

- Build a complete, understandable compiler.
- Keep language semantics explicit and predictable.
- Maintain a clean separation between parsing, semantic analysis, runtime design, and any later backend work.
- Reach a stable self-hosting implementation.
- Add features incrementally without accumulating special-case behavior.

## Design Philosophy

- Statically typed.
- Manual memory management.
- No garbage collector.
- Explicit control over memory and performance.
- Familiar low-level semantics with cleaner syntax.
- Correctness and simplicity before advanced features.
- Semantic facts should be explicit enough to support diagnostics, testing, tooling, interpretation, and any later backend design.

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
- concrete default types for inferred mutable numeric storage
- adaptable compile-time numeric constants

### Functions

```c
add::(a, b: i32) -> i32 {
    return a + b;
}
```

Supported features include:

* typed parameters
* grouped parameter declarations
* optional default parameter values
* explicit return types
* omitted return types defaulting to `void`
* argument type checking
* unified reachability and non-void fallthrough checking
* unreachable-statement diagnostics
* nested function declarations

Nested functions do not currently support closure capture. 
They may access visible globals, constants, types, and function declarations, but cannot read or modify 
locals and parameters belonging to an enclosing function.


### Types

Primitive and built-in types:

- `bool`
- `i8`, `i16`, `i32`, `i64`
- `u8`, `u16`, `u32`, `u64`
- `f32`, `f64`
- `void`

`null` is a dedicated contextual pointer literal. It is not integer zero and is
not a user-declarable storage type.

Compound and declared types:

- raw nullable object pointers
- fixed-size arrays
- nominal structs
- nominal enums
- function types

`void` is valid as a function return type, but not as a variable, constant, parameter, struct field, pointer element, or array element type in the current language.

### Expressions

Supported expression forms include:

- numeric, boolean, character, `null`, and contextual string literals
- identifiers
- arithmetic operators
- bitwise operators (`&`, `|`, `^`, `~`)
- shift operators (`<<`, `>>`)
- equality and ordered comparisons
- logical operators (`&&`, `||`)
- unary negation, logical negation, and bitwise complement
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
x &= mask;
x <<= count;
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

### Definite Assignment

Local variables are not implicitly initialized:

```c
value: i32;
```

A variable may be read only when semantic analysis can prove that it has been initialized on every 
reachable incoming path.

A direct whole-variable assignment initializes it:

```c
value: i32;

value = 10;

return value;
```

Parameters and variables declared with initializers begin initialized.

Compound assignment and increment/decrement require prior initialization because they read 
the previous value:

```c
value: i32;

value += 1; // invalid
value++;    // invalid
```

Assigning an entire struct or array initializes that variable. Assigning only a field, element, 
pointee, or pointer-indexed location does not initialize the complete base variable.

Taking the address of an uninitialized local is rejected.

Branch merging is reachability-aware. An unreachable branch does not weaken a branch that continues, 
and non-exhaustive switches include an implicit no-match path.

Loop analysis is conservative because a loop may execute zero times. Initialization performed only 
inside a loop is not generally available afterward.


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

### Raw Object Pointers

```c
value: i32 = 10;
pointer: i32* = &value;

*pointer = 20;
pointer[0] = 30;
pointer = null;
```

`T*` is a raw, nullable, non-owning pointer to mutable `T`. Dereference and
pointer indexing produce lvalues. Pointer operations are unchecked for
lifetime, bounds, ownership, and dangling values.

`null` is the only source-level null-pointer value:

```c
pointer: i32* = null; // valid
pointer: i32* = 0;    // invalid
```

An explicit `null`-to-pointer cast may provide a concrete pointer type:

```c
typed_null := cast(i32*, null);
```

### Arrays

```c
values: i32[3] = [1, 2, 3];
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

Structs are nominal types: two separate struct declarations are distinct even
when they have identical fields or the same source-level name in different
scopes.

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
- enum-to-integer casts
- compile-time integer-to-enum casts when the value names a declared member

Enums are closed, strongly typed, and nominal. A backing type defines
representation and range, but it does not make every backing-type value a
valid enum value. Separate enum declarations remain distinct even when they
share a backing type, members, or a shadowed source-level name. Runtime
integer-to-enum casts are currently rejected because checked runtime
conversion has not yet been implemented.

A future `#repr_c` annotation is planned for explicit ABI representation. It will not make an enum open.

### Numeric Semantics

Coglet keeps concrete numeric conversions explicit. Untyped literals adapt to
a concrete operation or destination type only when their exact value fits.

Compile-time integer arithmetic is range checked. Known integer division or
remainder by zero is rejected, including in compound assignments. Unary
negation is not defined for typed unsigned integers, while ordinary unsigned
subtraction remains a valid runtime operation.

`f32` and `f64` constant evaluation follows IEEE-754 behavior, including
infinity, NaN, and signed zero:

```c
1.0 / 0.0;   // positive infinity
-1.0 / 0.0;  // negative infinity
0.0 / 0.0;   // NaN
```

NaN compares unequal to itself, and ordered comparisons involving NaN are
false.

Bitwise operators are integer-only. Concrete operands for `&`, `|`, and `^`
must have the same type unless one operand is an adaptable untyped integer
constant that fits the concrete type. Signed bitwise operations use a defined
fixed-width two's-complement representation.

For shifts, the left operand determines the result type and bit width. The
count may have any integer type, but a statically known count must satisfy
`0 <= count < bit_width`. Left shift is a fixed-width bit-pattern operation:
bits shifted beyond the width are discarded. Unsigned right shift zero-fills,
while signed right shift is arithmetic and sign-extending.

Coglet intentionally gives bitwise operators higher precedence than equality
and ordered comparisons. Therefore:

```c
flags & mask == 0;
```

parses as `(flags & mask) == 0`, avoiding C's surprising precedence rule.

### Control Flow

Supported control flow includes:

* `if` / `else`
* `while`
* `for`
* `switch`
* `break`
* `continue`
* `return`
* lexical block scopes

Semantic analysis uses a unified reachability model for:

* definite assignment;
* branch and switch merging;
* `return`, `break`, and `continue`;
* unreachable-statement diagnostics;
* non-void function fallthrough checking.

A non-void function is valid when normal control flow cannot reach the end of its body. 
This includes functions that return on every continuing path and functions containing a provably 
non-terminating literal-true loop with no reachable `break`.

Switch analysis includes:

* switch-expression type checking;
* compile-time case validation;
* duplicate runtime-value detection;
* duplicate-`default` detection;
* Boolean and enum exhaustiveness;
* independent definite-assignment flow for every case.

Exhaustiveness is based on successfully validated runtime values. Invalid cases do not contribute coverage, and aliased enum members with the same value require only one corresponding case.


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

numeric literal or adaptable constant:
    type = untyped-int or untyped-float
    category = rvalue

inferred mutable numeric variable:
    type = concrete i32, i64, u64, or f64
    category = lvalue

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

The parser and semantic analyzer support a substantial core language. The semantic-information verifier now walks successful programs in source order and checks table completeness, duplicate/orphan entries, value categories, symbol associations, and concrete variable/parameter types. An optional diagnostic flag prints the semantic table deterministically.

Recently completed work includes:

- canonical shared semantic types for built-in scalars
- dedicated `null` semantics with no integer-zero pointer conversion
- nominal declaration identity for structs and enums
- restricted equality and ordered-comparison operand categories
- checked known integer zero-divisor diagnostics
- integer-only bitwise and shift operators with defined fixed-width semantics
- bitwise and shift compound assignments
- checked statically known shift counts
- IEEE-754 constant behavior for `f32` and `f64`
- explicit `TYPE_UNTYPED_INT` and `TYPE_UNTYPED_FLOAT` kinds
- concrete default typing for inferred mutable numeric variables and parameters
- adaptable compile-time numeric constants
- exact constant arithmetic and representability checks
- complete semantic-info invariant verification
- deterministic source-order semantic dumps
- closed enum value sets
- declared-member validation for constant integer-to-enum casts
- rejection of runtime integer-to-enum casts until checked conversion exists
- definite-assignment analysis for local variables and parameters
- stable per-function variable flow identity
- reachability-aware `if` and switch merging
- validated value-based Boolean and enum switch exhaustiveness
- conservative loop flow with `break` and `continue` tracking
- unified reachability for returns, unreachable statements, and function fallthrough
- rejection of unsupported nested-function captures

Backend and code-generation work is intentionally deferred until the language's direction and runtime model are clearer.

## Roadmap

Near-term work should remain language- and frontend-focused:

1. Decide runtime integer overflow and narrowing-cast behavior.
2. Design a small readonly raw-pointer mechanism without introducing borrowing or lifetime checking.
3. Plan opaque raw pointers and explicit C ABI types.
4. Design slices and pointer-length views after the raw-pointer mutability rules are settled.

Later work may include:

- imports and modules
- multi-file compilation
- package visibility
- C interoperability and explicit representation attributes
- a standard library
- generics
- interpretation or code generation once the execution strategy is chosen
- self-hosting

## License

TBD
