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
    type = untyped-int or untyped-float
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

## Definite Assignment and Reachability

A local variable declaration does not implicitly initialize the variable:

```c
value: i32;
```

The variable exists and has type `i32`, but its value cannot be read until semantic analysis can prove that it has been initialized on every reachable incoming control-flow path.

A plain whole-variable assignment initializes it:

```c
value: i32;

value = 10;

return value; // valid
```

Parameters and local variables declared with initializers begin initialized:

```c
use_value::(parameter: i32) -> i32 {
    local: i32 = 10;

    return parameter + local;
}
```

Definite-assignment analysis applies to function-local variables and parameters. Global variables are not tracked by this local flow analysis.

### Reads and writes

An ordinary identifier use reads the variable and therefore requires prior initialization:

```c
value: i32;

other := value; // invalid
```

A direct plain-assignment target does not read the previous value:

```c
value: i32;

value = 10; // valid: initializes value
```

Compound assignment and increment/decrement read the previous value before writing it:

```c
value: i32;

value += 1; // invalid
value++;    // invalid
value--;    // invalid
```

Taking the address of a local also requires the local to be initialized:

```c
value: i32;

pointer := &value; // invalid
```

This prevents an uninitialized local from becoming observable indirectly through a pointer.

### Whole values and subobjects

Assigning a complete struct or array variable initializes that variable:

```c
values: i32[3];

values = [1, 2, 3];

first := values[0]; // valid
```

Writing only a field, element, pointee, or pointer-indexed location does not initialize the complete base variable:

```c
point: Point;
values: i32[3];
pointer: i32* = get_pointer();

point.x = 10;    // does not initialize point
values[0] = 10; // does not initialize values
*pointer = 10;  // does not initialize another tracked local
pointer[0] = 10;
```

The base and index expressions used by a subobject write are still ordinary reads and must already be initialized where applicable.

### Conditional branches

Each `if` branch is checked from the same incoming state.

When both branches can continue, a variable is initialized afterward only when both branches initialize it:

```c
value: i32;

if condition {
    value = 10;
} else {
    value = 20;
}

return value; // valid
```

Without an `else`, the unchanged incoming path remains possible:

```c
value: i32;

if condition {
    value = 10;
}

return value; // invalid
```

A branch that cannot continue does not weaken a branch that can:

```c
value: i32;

if condition {
    value = 10;
} else {
    return 20;
}

return value; // valid
```

### Switch statements

Every switch case begins from the same incoming definite-assignment state. Cases do not inherit initialization from earlier cases, and Coglet switches do not fall through.

A switch is exhaustive when it contains:

* a `default` case;
* both validated Boolean values, `true` and `false`;
* every distinct runtime value declared by a closed enum.

Case coverage is based on successfully checked compile-time values, not on the source spelling of the case expression. Constants and explicit constant casts may therefore contribute to exhaustiveness.

Invalid case expressions never contribute to exhaustiveness.

For enums with aliased members, one case covers all member names representing the same runtime value:

```c
Color :: enum(u8) {
    Red = 0,
    Crimson = 0,
    Green = 1,
}
```

An exhaustive switch over `Color` requires cases for values `0` and `1`, not separate cases for both `Red` and `Crimson`.

An integer switch is exhaustive only when it contains `default`.

A non-exhaustive switch has an implicit path on which no case matches. That path preserves the incoming initialization state.

### Loops

Loop analysis is intentionally conservative. Initialization performed only during an iteration is not generally available after the loop:

```c
value: i32;

while condition {
    value = 10;
}

return value; // invalid
```

The same rule applies to `for` loops.

For a `for` loop, flow is checked in runtime order:

1. condition;
2. body;
3. post expression.

Normal body fallthrough and `continue` paths reach the post expression. `break` and `return` paths do not.

`break` and `continue` apply to the nearest enclosing loop.

A literal-true loop with no reachable `break` does not continue to the statement following the loop:

```c
run_forever::() -> i32 {
    while true {
    }
}
```

This function is valid even though it contains no `return`, because normal control flow cannot reach the end of the function.

A literal-true loop with a reachable `break` may continue after the loop and does not satisfy a non-void function's return obligation by itself.

### Unreachable statements

`return`, `break`, and `continue` make the remainder of their current control-flow path unreachable.

Coglet reports unreachable statements during block traversal:

```c
test::() -> i32 {
    return 10;

    value := 20; // unreachable
}
```

A non-void function is valid when normal control flow cannot reach the end of its body. This includes both explicit returns and provably non-continuing control flow.

### Nested functions

Nested functions do not currently support closure capture.

A nested function cannot read, modify, or take the address of a local variable or parameter belonging to an enclosing function:

```c
outer::() -> i32 {
    value: i32 = 10;

    inner::() -> i32 {
        return value; // invalid: capture is not supported
    }

    return 0;
}
```

Nested functions may still refer to visible global variables, compile-time constants, types, and function declarations.

Closure environments and captured runtime storage remain future language-design work.


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


## Numeric Literals, Inference, and Constants

Integer and floating-point literals begin as adaptable numeric values:

```text
integer literal       -> untyped-int
floating-point literal -> untyped-float
```

The exact integer value is retained independently of a concrete machine type. Negative values are parsed as unary negation applied to a positive literal:

```text
-2147483648

unary '-'
└── number 2147483648
```

This permits correct handling of signed minimum values.

Mutable inferred storage receives a concrete default type:

```c
a := 1;                    // i32
b := 2147483648;           // i64
c := 9223372036854775808;  // u64
d := 1.5;                  // f64
```

Inferred compile-time constants remain adaptable:

```c
A :: 255;  // untyped-int
B :: 1.5;  // untyped-float

small: u8 = A;
wide: i64 = A;
float_value: f32 = A;
rounded: f32 = B;
```

Adaptation succeeds only when the exact value is representable in the destination type. Two different concrete numeric types do not implicitly convert; an explicit cast is required.

## Numeric Arithmetic and Comparisons

Concrete numeric operands must have the same type unless one operand is an
adaptable untyped literal or constant. The adaptable value must fit the
concrete operation type.

```c
value: u8 = 10;

value + 1;    // valid: 1 adapts to u8
value + 256;  // invalid: 256 does not fit u8

signed: i32 = 1;
unsigned: u32 = 1;

signed + unsigned; // invalid: use an explicit cast
```

Compile-time integer arithmetic is exact and range checked. A result outside
the operation type is an error, including unsigned underflow.

Typed unsigned integers do not support unary negation:

```c
value: u32 = 1;

-value; // invalid
```

Binary subtraction on unsigned values remains a valid runtime operation:

```c
difference: u32 = left - right;
difference -= right;
```

The frontend currently establishes that these runtime expressions are well
typed. Runtime overflow and underflow behavior has not yet been selected.

The remainder operator `%` is integer-only.

A statically known zero divisor is rejected for integer division and
remainder, including compound assignment:

```c
value / 0;   // invalid
value % 0;   // invalid
value /= 0;  // invalid
value %= 0;  // invalid
```

This rule also applies when zero is produced by a named constant, cast, or
other compile-time constant expression.

Equality is currently defined for:

- numeric values with compatible types
- `bool`
- values of the same enum declaration
- pointers with the same pointee type
- a pointer and `null`

Value equality is not currently defined for structs, arrays, or functions.

Ordered comparisons (`<`, `<=`, `>`, and `>=`) require numeric operands.
Boolean, enum, pointer, null, struct, array, and function values do not support
ordered comparison.

## Bitwise and Shift Operators

Coglet supports integer bit manipulation with:

```c
left & right;
left | right;
left ^ right;
~value;
left << count;
left >> count;
```

All bitwise and shift operands must be integers. Enums remain nominal values
and require an explicit cast to an integer type before bit manipulation.

For `&`, `|`, and `^`, two concrete operands must have exactly the same integer
type. An adaptable untyped integer literal or constant may take the other
operand's concrete type only when its exact value fits. `~` returns the same
type as its operand.

Signed bitwise operations are defined using the type's fixed-width two's-
complement bit pattern. This definition is independent of the host C
implementation.

### Shift typing and count rules

The left operand alone determines the result type, signedness, and bit width.
The count may have any integer type:

```c
value: u32 = 1;
small_count: u8 = 3;
wide_count: i64 = 3;

value << small_count; // u32
value >> wide_count;  // u32
```

A statically known count must satisfy:

```text
0 <= count < left_operand_bit_width
```

Negative counts and counts equal to or greater than the width are errors.
Unknown runtime counts are accepted by the frontend; a future execution layer
must enforce the same range rather than masking the count modulo the width.

An untyped left operand uses its ordinary default integer width. Therefore
`1 << count` uses `i32`; an explicitly wider operation starts with a cast such
as `cast(u64, 1) << count`.

### Shift result semantics

Left shift is a fixed-width bit-pattern operation. Zero bits enter from the
right and bits shifted beyond the width are discarded:

```c
cast(u8, 128) << 1; // u8 value 0
cast(i8, 64) << 1;  // i8 value -128
```

This defined truncation applies specifically to shift operations. It does not
settle general runtime overflow behavior for arithmetic operators.

Unsigned right shift fills with zero. Signed right shift is arithmetic and
sign-extending:

```c
cast(u8, 128) >> 1; // 64
cast(i8, -3) >> 1;  // -2
```

### Precedence

Coglet deliberately avoids C's bitwise/comparison precedence trap. Bitwise
operators bind more tightly than equality and ordered comparisons:

```c
flags & mask == 0;
```

is parsed as:

```c
(flags & mask) == 0;
```

From lower to higher precedence, the relevant binary groups are:

```text
||, &&, equality, ordered comparison, |, ^, &, shifts, addition, multiplication
```

### Compound bitwise and shift assignment

The statement-only forms are:

```c
value &= mask;
value |= bits;
value ^= toggle;
value <<= count;
value >>= count;
```

For `&=`, `|=`, and `^=`, the target is the operation type. A concrete right
operand must match it exactly; an untyped integer constant may adapt when it
fits. For `<<=` and `>>=`, the target determines the width while the count may
have any integer type. Known counts use the same range rule as ordinary shifts.

### Floating-point semantics

`f32` and `f64` constant arithmetic follows IEEE-754 behavior. Floating-point
division by zero is not an integer-style semantic error:

```c
1.0 / 0.0;   // positive infinity
-1.0 / 0.0;  // negative infinity
0.0 / 0.0;   // NaN
```

Signed zero is preserved:

```c
0.0 == -0.0; // true
1.0 / -0.0;  // negative infinity
```

NaN is unordered:

```c
NAN_VALUE :: 0.0 / 0.0;

NAN_VALUE == NAN_VALUE; // false
NAN_VALUE != NAN_VALUE; // true

NAN_VALUE < 0.0;  // false
NAN_VALUE <= 0.0; // false
NAN_VALUE > 0.0;  // false
NAN_VALUE >= 0.0; // false
```

Both the `f32` and `f64` constant-evaluation paths preserve infinity, NaN, and
signed zero. Explicit conversion of NaN or infinity to an integer is invalid.
A finite value that does not fit the destination floating-point type is also
rejected during checked constant conversion.

## Raw Object Pointers

Coglet supports raw object pointers as its low-level memory and future C-interoperability foundation.

```c
value: i32 = 10;
pointer: i32* = &value;
*pointer = 20;
```

The current pointer type `T*` is:

- nullable
- a pointer to mutable `T`
- unchecked for lifetime, alignment, bounds, ownership, and aliasing
- an rvalue when produced by address-of
- a source of a `T` lvalue when dereferenced

Address-of requires mutable assignable storage. Valid targets include mutable variables, assignable fields, assignable fixed-array indexes, dereference expressions, and pointer indexes. Constants, enum members, literals, arithmetic results, direct call results, and fields or indexes of temporary aggregate values cannot be addressed.

```c
x: i32 = 1;
p: i32* = &x;       // valid
q: i32* = &*p;      // valid
*&x = 2;            // valid

bad: i32* = &(1 + 2); // invalid
```

Dereferencing a non-pointer is invalid. Dereference produces an lvalue even when the pointer expression itself is an rvalue, because a temporary pointer value may still designate persistent storage:

```c
get_pointer()[0] = 1;
*get_pointer() = 2;
```

Pointer indexing is currently an unchecked low-level operation. General pointer
arithmetic operators remain unsupported, arrays do not decay implicitly to
pointers, and unrelated pointer types do not implicitly convert.

### Null pointers

`null` is Coglet's dedicated null-pointer literal. It is not integer zero, and
integer `0` is not accepted where a pointer is required.

```c
pointer: i32* = null;
pointer = null;

takes_pointer(null);
return null;

pointer == null;
null != pointer;
```

`null` has a contextual pseudo-type rather than a concrete storage type. A
surrounding pointer type must determine its concrete pointer type.

Invalid:

```c
pointer := null;
NONE :: null;

pointer: i32* = 0;
pointer = 0;
takes_pointer(0);

null == null;
null == 0;
null < pointer;
null + 1;

*null;
```

A typed pointer constant may use `null`:

```c
NONE: i32* : null;
NONE_IS_NULL :: NONE == null;
```

An explicit cast may supply the missing concrete pointer type:

```c
pointer := cast(i32*, null);
```

The reverse direction is not supported, and integer values do not become
pointers through casts:

```c
cast(i32*, 0);    // invalid
cast(i32, null);  // invalid
cast(u64, null);  // invalid
cast(bool, null); // invalid
```

At a future C interoperability boundary, Coglet `null` will represent a C null
pointer. Coglet does not need to adopt C's source-language rule that integer
zero may act as a null-pointer constant.

`void*`, read-only pointee qualification, safe references, slices, ownership,
borrowing, and lifetime checking remain future work.

Raw pointers are not intended to become Coglet's only pointer-like abstraction. Future safe references and slices may use stronger source-language rules while retaining direct or wrapper-based C ABI compatibility.

## Arrays

Arrays are fixed-size values with an element type and compile-time length.

```c
values: i32[3] = [0, 0, 0];

values[0] = 1;
values[1] += 2;
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

### Nominal type identity

Structs and enums are nominal types. Their identity comes from the specific
declaration that created them, not from their fields, members, backing types,
or source-level spelling.

```c
First :: struct {
    value: i32;
}

Second :: struct {
    value: i32;
}
```

`First` and `Second` are different types even though their fields match.

The same rule applies to shadowed declarations:

```c
Point :: struct {
    x: i32;
}

test::() -> void {
    outer: Point = Point { x = 1 };

    Point :: struct {
        x: i32;
    }

    inner: Point = Point { x = 2 };

    outer = inner; // invalid: different Point declarations
}
```

Enum declarations follow the same declaration-identity rule. Values from
different enum declarations are never interchangeable merely because their
members or backing types match.

## Enums

Enums have integer backing types and are closed by default.

```c
Color :: enum(u16) {
    Red,
    Green,
    Blue,
}
```

Members may use implicit or explicit integer constant values. The backing type controls representation and range, but the valid values of `Color` are only the declared member values.

Semantic analysis checks:

- integer backing type
- duplicate member names
- integer constant member values
- backing-type range
- duplicate declarations
- unknown members
- enum type compatibility

Enum members are values and are not assignable. Different enum types remain distinct even when their backing types match.

Enum-to-integer casts are allowed. A compile-time integer-to-enum cast is allowed only when the value equals a declared member:

```c
GREEN :: cast(Color, 1); // valid
BAD   :: cast(Color, 9); // invalid
```

Runtime integer-to-enum casts are currently rejected. A future checked conversion may validate the incoming value at runtime.

A future `#repr_c` annotation is planned to make the backing representation and ABI contract explicit. `#repr_c` will not change closed-enum validity or switch exhaustiveness.

## Switch Statements

Switch expressions may use integer, Boolean, or enum values.

Case expressions must be compile-time constants compatible with the switch expression type.

Semantic analysis checks:

* the switch expression type;
* case-value compatibility and representability;
* duplicate runtime case values;
* duplicate `default` clauses;
* Boolean and enum exhaustiveness;
* definite-assignment state across independent cases;
* whether control flow can continue after the switch.

Case values contribute to exhaustiveness only after they have been successfully checked and converted to the switch expression type. An invalid case never improves definite-assignment or return-flow results.

Each case begins from the same incoming state. Cases do not execute sequentially and do not inherit initialization from earlier cases.

A switch is exhaustive when it has:

* `default`;
* both Boolean runtime values;
* every distinct declared runtime value of a closed enum.

Enum exhaustiveness is value-based. Aliased enum member names with the same value require only one corresponding case.

Integer switches require `default` to be exhaustive.

A non-exhaustive switch includes an implicit no-match path that retains the incoming definite-assignment state.


## Casts

Explicit casts use the form:

```c
cast(TargetType, expression)
```

Supported cast categories currently include:

- identical types
- selected numeric conversions
- enum-to-integer conversion
- compile-time integer-to-enum conversion to a declared member
- `null` to a concrete raw-pointer type
- boolean to boolean

A `null` cast may provide a concrete pointer type:

```c
typed_null := cast(i32*, null);
```

Integer-to-pointer, pointer-to-integer, and null-to-non-pointer casts are not
currently supported.

A cast whose operand is known at compile time is checked for representability in the destination type in every expression context, including discarded expression statements:

```c
test::() -> void {
    cast(u8, 256); // invalid even though the result is discarded
}
```

For closed enums, fitting the backing type is necessary but not sufficient:

```c
Small :: enum(u8) { A, B }

cast(Small, 1);   // valid: Small.B
cast(Small, 255); // invalid: no declared member has value 255
```

Runtime integer-to-enum casts are rejected until checked runtime conversion exists. Runtime narrowing between numeric types remains intentionally unspecified; Coglet still needs to choose checked conversion, wrapping/truncating conversion, or rejection.

## Current Semantic Architecture

Semantic analysis stores expression facts separately from the AST. The semantic-info verifier walks successful programs in source order and checks:

- one semantic entry for every successful expression or mutation node
- no duplicate or orphan side-table entries
- correct lvalue, rvalue, and no-value categories
- symbol/type consistency
- concrete types for variables and parameters
- distinct handling of mutation nodes and void-returning calls

The verifier can also print a deterministic source-order dump for debugging. Semantic tables from failed programs may be partial and are dumped only when explicitly requested.

## Future Direction

Backend work is deliberately deferred until the language's purpose, runtime model, and execution strategy are clearer. Near-term work should focus on language semantics and tests. Candidate areas include:

- runtime integer overflow and narrowing-cast semantics
- readonly and opaque raw-pointer variants
- slices and readonly byte views
- ownership, lifetime, and mutability rules
- a later first-class string type
- C ABI representation and a future `#repr_c` attribute
- imports and modules
- multi-file compilation
- standard library facilities
- generics
- self-hosting
