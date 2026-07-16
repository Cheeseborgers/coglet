# Coglet Language Notes

Coglet is a modern systems language focused on explicit semantics, predictable behavior, and a compiler architecture that can grow without accumulating technical debt.

This document records current user-visible language behavior.

## Values, Storage, and No-Value Expressions

Coglet distinguishes among:

- expressions that produce values
- expressions that denote assignable storage
- successful expressions or operations that produce no value

Semantic analysis records a value category for expressions:

- `lvalue`: assignable storage
- `rvalue`: a produced value
- `none`: no usable value

Examples:

```text
variable:
    type = declared type
    category = lvalue

numeric literal:
    type = numeric type
    category = rvalue

void-returning call:
    type = void
    category = none

assignment statement:
    type = none
    category = none
```

A no-value expression cannot be used where a value is required.

## Assignability

An expression is assignable only when semantic analysis determines that it is an lvalue.

Variables are assignable:

```c
x: i32 = 1;
x = 2;
```

Fields and indexes are assignable only when their base expression is assignable:

```c
point.x = 1;
values[0] = 10;
```

The following are not assignable:

```c
CONSTANT = 1;           // constants are not assignable
Color.Red = Color.Blue; // enum members are not assignable
make_point().x = 1;     // field of temporary value is not assignable
make_array()[0] = 1;    // index of temporary value is not assignable
```

## Mutation Operations

Plain assignment, compound assignment, and increment/decrement are statement-only operations.

Valid:

```c
x = 1;
x += 1;
x++;
```

They are also valid in a `for` post clause.

Invalid:

```c
y := (x = 1);
takes_i32(x += 1);
return x++;
1 + (x = 2);
```

Successful mutation nodes produce no value.

## Void-Returning Calls

A call to a function returning `void` is a successful no-value expression.

Valid when discarded:

```c
does_nothing();
```

Invalid when a value is required:

```c
x := does_nothing();
takes_i32(does_nothing());

bad::() -> i32 {
    return does_nothing();
}

x := does_nothing() + 1;
```

This differs from mutation operations:

- a void call remains an expression with type `void`
- a mutation statement has no expression type
- both have value category `none`

## Arrays

Arrays are fixed-size values with an element type and compile-time length.

```c
values: i32[3];
```

The type means an array of three `i32` values.

Array size is part of the type:

```c
a: i32[3];
b: i32[4];
```

`a` and `b` have different types.

Array elements may be accessed by index:

```c
values[0] = 1;
values[1] += 2;
```

Constant indexes are checked against fixed array bounds during semantic analysis.

Runtime integer indexes are allowed.

## Array Literals

Array literals initialize fixed-size arrays.

```c
values: i32[3] = [1, 2, 3];
```

The expected type supplies the element type and required length.

Invalid:

```c
values: i32[3] = [1, 2];       // too few elements
values: i32[3] = [1, 2, 3, 4]; // too many elements
values: i32[3] = [1, true, 3]; // wrong element type
```

Supported expected-type contexts include:

```c
values: i32[3] = [1, 2, 3];
values = [4, 5, 6];

takes_i32_array([1, 2, 3]);

make_values::() -> i32[3] {
    return [1, 2, 3];
}

Point :: struct {
    values: i32[3];
}

p := Point {
    values = [1, 2, 3],
};
```

Array literals are not yet inferred standalone expressions.

Rejected:

```c
values := [1, 2, 3];
[1, 2, 3];
```

## String Literals

String literals represent immutable compile-time byte data.

They are currently contextual initializers for fixed-size byte arrays.

```c
name: u8[6] = "hello";
```

The literal contains five visible bytes and one trailing null byte:

```text
h e l l o \0
```

The destination must therefore have length 6.

Supported expected-type contexts include:

```c
name: u8[6] = "hello";
name = "hello";

takes_name("hello");

make_name::() -> u8[6] {
    return "hello";
}

Person :: struct {
    name: u8[6];
}

p := Person {
    name = "hello",
};
```

Invalid:

```c
name: u8[5] = "hello";   // no room for trailing null byte
name: i32[6] = "hello";  // destination element type is not u8
name := "hello";         // no expected byte-array type
"hello";                 // bare string literal is not a general expression
```

The required array length is based on decoded bytes plus the trailing null byte.

Supported escape sequences currently include:

```text
\n
\t
\r
\\
\"
\0
```

Invalid escape sequences are rejected during semantic analysis.

## String Mutability

String literals themselves are immutable compile-time data.

When a string literal initializes a mutable array, the destination receives initialized mutable storage.

```c
name: u8[6] = "hello";
name[0] = 'H';
```

This mutates the array, not the original literal.

Future code generation may place literal data in readonly static storage while copying bytes into mutable arrays where required.

## Structs

Struct fields have declared types.

```c
Point :: struct {
    x: i32;
    y: i32;
}
```

Struct initialization is contextual by field type:

```c
p := Point {
    x = 1,
    y = 2,
};
```

Semantic analysis checks:

- unknown fields
- duplicate field initializers
- missing required fields
- initializer compatibility
- invalid `void`-containing field types

A field expression is assignable only when its base expression is assignable.

## Enums

Enums have integer backing types.

```c
Color :: enum(u16) {
    Red,
    Green,
    Blue,
}
```

Members may use implicit or explicit integer constant values.

Semantic analysis checks:

- integer backing type
- duplicate member names
- integer constant member values
- backing-type range
- duplicate declarations
- unknown members
- enum type compatibility

Enum members are values and are not assignable.

Different enum types remain distinct even when their backing types match.

## Switch Statements

Switch expressions may use integer, boolean, or enum values.

Case values must be compile-time constants compatible with the switch type.

Semantic analysis checks:

- switch expression type
- case type compatibility
- duplicate case values
- duplicate default clauses
- enum exhaustiveness
- return-path behavior through switch branches

## Casts

Explicit casts currently support selected conversions among:

- numeric types
- integer and enum backing values
- identical types
- boolean to boolean

Unsupported casts are rejected.

Compile-time integer and enum casts are range-checked when the value is known.

## Control Flow

Supported control flow includes:

- `if` / `else`
- `while`
- `for`
- `switch`
- `break`
- `continue`
- `return`
- nested block scopes

Conditions must produce boolean values.

`break` and `continue` are valid only inside loops.

Non-void functions must return on every reachable path recognized by the current return analysis.

Statements after a definitely returning statement are diagnosed as unreachable.

## Casts

Explicit casts use the form:

```c
cast(TargetType, expression)
```

A cast whose operand is known at compile time must be checked for
representability in the destination type. If the value cannot be represented,
the cast is a semantic error regardless of where the cast appears.

This applies in all expression contexts, including:

constant declarations
variable initializers
return expressions
function arguments
discarded expression statements

Discarding a cast result does not skip semantic validation:

```c
test::() -> void {
    cast(u8, 256); // invalid: 256 does not fit in u8
}
```
Integer-to-enum casts and enum-to-integer casts are checked against the enum's
backing type when the value is known at compile time.

Runtime narrowing-cast behavior is not yet specified. Coglet should make an
explicit decision before relying on runtime narrowing as checked conversion,
wrapping/truncating conversion, or rejection.


## Current Semantic Architecture

Semantic analysis stores expression facts separately from the AST.

This supports:

- lvalue checking
- contextual value requirements
- type propagation
- symbol resolution
- later lowering and code generation
- semantic debugging and invariant testing

A failed parent expression normally has no successful semantic fact, although child expressions that checked successfully may retain their own facts.

## Future Direction

Near-term semantic work includes:

- complete AST-wide invariant verification
- deterministic source-order semantic dumps
- failed-expression fact auditing
- a clear enum-member symbol model

Future language work may include:

- slices
- readonly byte views
- a first-class string type
- imports and modules
- multi-file compilation
- generics
- standard library facilities
- self-hosting
