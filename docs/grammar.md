# Coglet Grammar Notes

This document records intended surface syntax and current semantic restrictions for supported and near-term language features.

It is not yet a complete formal grammar.

## Type Syntax

A simplified type grammar is:

```ebnf
type =
    ["readonly"] base_type
    {"*"}
    ["[" integer_constant "]"];
```

`readonly` is valid only when at least one pointer layer follows the base type.
It qualifies the first pointer layer following that base type.

```c
mutable: i32*;
view: readonly i32*;
nested: readonly i32**;
```

`readonly i32**` means a mutable pointer to a readonly pointer to `i32`.
Additional outer pointer layers remain mutable.

These are invalid:

```c
value: readonly i32;
values: readonly i32[4];
```

Examples of ordinary types:

```c
value: i32;
pointer: i32*;
values: i32[4];
points: Point[8];
```

Fixed-size array bounds must currently be compile-time integer constants.

### Initialization

A local variable declaration may omit an initializer:

```c
value: i32;
```

This is syntactically valid.

The variable is **not** implicitly initialized.

Semantic analysis requires every read of a local variable or parameter to be provably initialized
along every reachable incoming control-flow path.

Parameters and declarations with successful initializers begin initialized.

## Numeric Literals and Unary Minus

A minus sign is an operator, not part of a numeric token.

```ebnf
unary_expression =
      "-" unary_expression
    | "!" unary_expression
    | "~" unary_expression
    | "&" unary_expression
    | "*" unary_expression
    | primary_expression;
```

For example, `-2147483648` is parsed as unary negation applied to the positive literal `2147483648`. Numeric literals initially have adaptable `untyped-int` or `untyped-float` semantic types. Inferred mutable variables and parameters are concretized to default runtime types, while inferred compile-time constants may remain adaptable.

## Raw Object Pointers

Pointer types use postfix `*`. The optional `readonly` keyword describes
access through the first pointer layer.

```c
value: i32 = 10;

mutable: i32* = &value;
view: readonly i32* = mutable;
```

`T*` grants mutable access to `T`. `readonly T*` grants read access without
write permission through that pointer.

Dereference and pointer indexing produce lvalues. Their storage access is
determined by the pointer type:

```text
T*            -> writable lvalue
readonly T*   -> readonly lvalue
```

Address-of requires an lvalue and preserves its storage access. It does not
require the operand to be writable.

Postfix operators bind more tightly than prefix unary operators, so
`*p.field` parses as `*(p.field)`, while `(*p).field` accesses a field through
a pointer.

Mutable pointers may adapt to matching readonly pointers. The reverse and
recursive nested-pointer adaptations are rejected.

Pointers with equal immediate pointee types may be compared despite an
immediate mutable-versus-readonly difference. Both pointer forms may be
compared with `null`.

Arrays do not decay implicitly to pointers. General pointer arithmetic,
opaque pointers, and ownership or lifetime checking are not yet supported.
`null` is the only source-level null-pointer value; integer zero does not
implicitly or explicitly become a pointer.

## Array Indexing

Indexing uses postfix syntax:

```c
object[index]
```

The index expression must have an integer type.

Fixed-array indexing inherits the value category and storage access of the
array expression. Pointer indexing always produces an lvalue whose access
comes from the pointer type:

```c
mutable_pointer[0] = 1;       // valid
readonly_pointer[0] = 1;      // invalid
value := readonly_pointer[0]; // valid
```

Compile-time-known indexes into fixed-size arrays are bounds checked. Raw
pointer indexing remains unchecked because pointers carry no length.

## Array Literals

```ebnf
array_literal =
    "[" [initializer {"," initializer} [","]] "]";
```

Examples:

```c
values: i32[3] = [1, 2, 3];
values: i32[3] = [1, 2, 3,];
```

Array literals are contextual initializers. They require an expected array type from the surrounding context.

Supported expected-type contexts include:

```c
values: i32[3] = [1, 2, 3];

values = [1, 2, 3];

takes_i32_array([1, 2, 3]);

make_values::() -> i32[3] {
    return [1, 2, 3];
}

p := Point {
    values = [1, 2, 3],
};
```

Rejected:

```c
values := [1, 2, 3];
[1, 2, 3];
```

Array literals are not yet general inferred standalone expressions.

## String Literals

```ebnf
string_literal =
    '"' {string_character | escape_sequence} '"';
```

Supported escape sequences include:

```text
\n
\t
\r
\\
\"
\0
```

Example:

```c
name: u8[6] = "hello";
```

String literals are contextual initializers for fixed-size byte arrays.

The destination must be `u8[N]`, where `N` exactly matches the decoded byte length plus a trailing null byte.

Supported expected-type contexts include:

```c
name: u8[6] = "hello";

name = "hello";

takes_name("hello");

make_name::() -> u8[6] {
    return "hello";
}

p := Person {
    name = "hello",
};
```

Rejected:

```c
name := "hello";
"hello";
```

String literals are not yet general inferred standalone expressions.

## Function Declarations and Calls

A simplified function form is:

```ebnf
function_declaration =
    identifier "::"
    "(" [parameter_list] ")"
    ["->" type]
    block;
```

Example:

```c
add::(a, b: i32) -> i32 {
    return a + b;
}
```

A missing return type defaults to `void`.

Call arguments are checked against parameter types as contextual initializers. This allows contextual array and string literals in argument position.

A call returning `void` is valid only where its result is discarded.

```c
does_nothing(); // valid
```

Invalid:

```c
x := does_nothing();
takes_i32(does_nothing());
does_nothing() + 1;
```

### Nested Function Semantics

Nested function declarations are permitted.

Nested functions currently execute without closure environments.

They may reference visible globals, compile-time constants, types, and function declarations.

They may **not** capture locals or parameters belonging to an enclosing function.


## Operator Precedence and Associativity

Binary operators are left-associative. Assignment and compound assignment are
right-associative and remain statement-only.

From lowest to highest precedence:

```text
||
&&
== !=
< <= > >=
|
^
&
<< >>
+ -
* / %
unary: - ! ~ & *
postfix: call, field access, indexing
```

This ordering deliberately differs from C. Bitwise operators bind more tightly
than comparisons, so:

```c
flags & mask == 0;
```

parses as:

```c
(flags & mask) == 0;
```

The same `&` token is unary address-of in prefix position and binary bitwise
AND between value expressions.

## Bitwise and Shift Expressions

```ebnf
bitwise_expression =
      expression "&" expression
    | expression "|" expression
    | expression "^" expression
    | "~" unary_expression;

shift_expression =
      expression "<<" expression
    | expression ">>" expression;
```

Bitwise operators are integer-only. For `&`, `|`, and `^`, concrete operand
types must match exactly unless one operand is an adaptable untyped integer
constant that fits the concrete type. `~` preserves the operand type.

For shifts, the left operand determines the result type and operation width.
The right operand may have any integer type. A compile-time-known count must
satisfy:

```text
0 <= count < left_operand_bit_width
```

Left shift discards bits shifted beyond the fixed width. Unsigned right shift
zero-fills. Signed right shift is arithmetic and sign-extending.

## Assignment

```ebnf
assignment_statement =
    assignable "=" initializer;
```

Examples:

```c
x = 1;
point.x = 2;
values[0] = 3;
name = "hello";
values = [1, 2, 3];
```

The left-hand side must denote assignable storage.

Invalid:

```c
CONSTANT = 1;
Color.Red = Color.Blue;
make_point().x = 1;
make_array()[0] = 1;
```

The right-hand side is checked as an initializer against the target type. This permits contextual
string and array literals.

Assignment is statement-only and does not produce a value.

Invalid:

```c
y := (x = 1);
takes_i32(x = 1);
return x = 1;
```

### Assignment Semantics

A direct assignment

```c
value = expression;
```

assigns to the complete variable and initializes it after successful semantic checking.

Assignments to subobjects do **not** initialize the enclosing variable:

```c
point.x = value;
values[index] = value;
*pointer = value;
pointer[index] = value;
```

These operations still evaluate their component expressions normally.

Compound assignment and increment/decrement read the previous value before writing a replacement and
therefore require the target to have been initialized already.


## Compound Assignment

```ebnf
compound_assignment_statement =
    assignable compound_assignment_operator expression;

compound_assignment_operator =
      "+=" | "-=" | "*=" | "/=" | "%="
    | "&=" | "|=" | "^="
    | "<<=" | ">>=";
```

Examples:

```c
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 2;
x &= mask;
x |= bits;
x ^= toggle;
x <<= count;
x >>= count;
values[0] += 1;
```

Rules:

- the left-hand side must denote assignable storage
- arithmetic compound assignments require numeric operands
- concrete arithmetic operand types must be compatible
- `%=` requires integer operands
- `&=`, `|=`, and `^=` require integer operands; a concrete right operand must match the target type exactly
- an untyped integer right operand may adapt when its exact value fits the target type
- `<<=` and `>>=` require an integer target and an integer count; the count type does not need to match the target type
- a statically known shift count must satisfy `0 <= count < target_bit_width`
- the right-hand side is a normal value expression, not a contextual initializer
- compound assignment is statement-only and produces no value

## Increment and Decrement

```ebnf
increment_decrement_statement =
      assignable "++"
    | assignable "--"
    | "++" assignable
    | "--" assignable;
```

The target must be assignable numeric storage.

Increment and decrement are statement-only and do not produce values.

Invalid:

```c
y := x++;
return ++x;
takes_i32(x--);
```

## Value Expressions and Statement Expressions

Coglet distinguishes value-required contexts from statement position.

A value-required context includes:

- variable inference
- ordinary initializer expressions
- function arguments
- return values
- unary and binary operands
- conditions
- switch expressions
- cast sources
- field and index operands

Statement position includes:

- expression statements
- `for` post clauses

Void-returning calls are accepted in statement position but rejected in value-required contexts.

Mutation operations are accepted only in statement position.

## Field Access

Field access uses postfix syntax:

```c
object.field
```

A struct field selected from an lvalue inherits both its lvalue category and
its writable or readonly access. A field selected from an rvalue remains an
rvalue.

Pointer field access currently requires explicit dereference:

```c
(*pointer).field
```

Enum member syntax uses the same AST form but is resolved as a type-qualified
member rather than runtime storage:

```c
Color.Red
```

## Explicit Conversion Expressions

Checked and truncating conversions share this surface grammar:

```ebnf
conversion_expression =
      "cast" "(" type "," expression ")"
    | "truncate" "(" type "," expression ")";
```

`cast(TargetType, expression)` is checked and value-preserving. It also permits
the safe access conversion from `T*` to `readonly T*`, but not the reverse.

`truncate(TargetIntegerType, expression)` accepts only integer sources and
concrete integer targets. It retains the low destination-width bits and
interprets them using the target signedness.

## Switch

A simplified switch form is:

```ebnf
switch_statement =
    "switch" expression "{"
        {case_clause}
        [default_clause]
    "}";

case_clause =
    "case" constant_expression ":" statement_or_block;

default_clause =
    "default" ":" statement_or_block;
```

The switch expression must produce an integer, boolean, or enum value.

Case expressions must:

- produce values
- be compile-time constants
- match the switch expression type
- not duplicate an earlier case value

At most one default clause is allowed.

Enum switches may be recognized as exhaustive when every member is covered.

### Switch Semantics

Case expressions must be compile-time constants compatible with the switch expression type.

Semantic analysis validates every case before it contributes to duplicate detection or exhaustiveness.

Switch exhaustiveness is value-based:

* `default` covers every possible value.
* Boolean switches require both `true` and `false`.
* Enum switches require every distinct declared runtime value.
* Enum aliases sharing the same runtime value require only one corresponding case.

Invalid case expressions never contribute to exhaustiveness.


## Enum Declarations and Closed Values

A simplified enum form is:

```ebnf
enum_declaration =
    identifier "::" "enum" ["(" integer_type ")"]
    "{" {enum_member [","]} "}";

enum_member =
    identifier ["=" constant_integer_expression];
```

Enums are closed. The backing type constrains member representation, but only declared member values are valid values of the enum type.

```c
Color :: enum(u16) {
    Red = 0,
    Green = 1,
    Blue = 2,
}
```

A compile-time integer-to-enum cast must name a declared value. Runtime integer-to-enum conversion is currently rejected. Enum-to-integer conversion is allowed.

A future representation annotation may use syntax such as:

```c
Color :: enum(u16) #repr_c {
    Red = 0,
    Green = 1,
    Blue = 2,
}
```

The annotation is not currently implemented and will concern ABI representation, not enum openness.
