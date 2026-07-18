# Coglet Grammar Notes

This document records intended surface syntax and current semantic restrictions for supported and near-term language features.

It is not yet a complete formal grammar.

## Type Syntax

A simplified current type grammar is:

```ebnf
type =
    base_type
    {"*"}
    ["[" integer_constant "]"];
```

Examples:

```c
value: i32;
pointer: i32*;
values: i32[3];
bytes: u8[16];
```

Fixed-size array bounds must currently be compile-time integer constants.

The current semantic rules reject stored value types containing `void`, including `void*` and `void[N]`.

A plain `void` return type remains valid for functions.


## Numeric Literals and Unary Minus

A minus sign is an operator, not part of a numeric token.

```ebnf
unary_expression =
      "-" unary_expression
    | "!" unary_expression
    | "&" unary_expression
    | "*" unary_expression
    | primary_expression;
```

For example, `-2147483648` is parsed as unary negation applied to the positive literal `2147483648`. Numeric literals initially have adaptable `untyped-int` or `untyped-float` semantic types. Inferred mutable variables and parameters are concretized to default runtime types, while inferred compile-time constants may remain adaptable.

## Raw Object Pointers

Pointer types use postfix `*` in type syntax. Address-of and dereference use prefix unary operators:

```c
value: i32 = 10;
pointer: i32* = &value;
*pointer = 20;
```

`T*` currently means a raw, nullable pointer to mutable `T`. It carries no bounds, ownership, lifetime, non-null, or aliasing guarantee.

Address-of requires a mutable lvalue. Dereference requires a pointer value and produces an lvalue. Postfix operators bind more tightly than prefix unary operators, so `*p.field` parses as `*(p.field)`, while `(*p).field` accesses a field through a pointer.

Arrays do not decay implicitly to pointers. General pointer arithmetic, `void*`, and pointee const qualification are not yet supported. Literal zero remains the only implicit null-pointer initializer.

## Array Indexing

```ebnf
index_expression =
    expression "[" expression "]";
```

Examples:

```c
values[0]
values[i + 1]
```

The index expression must produce an integer value.

Constant indexes into fixed-size arrays are checked at compile time.

Fixed-array indexing is assignable only when the indexed array expression is assignable. Pointer indexing always denotes the storage selected by the pointer value:

```c
values[0] = 1;              // valid when values is mutable storage
make_array()[0] = 1;        // invalid: the array result is a temporary value
get_pointer()[0] = 1;       // valid: the pointer value designates storage
```

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

The right-hand side is checked as an initializer against the target type. This permits contextual string and array literals.

Assignment is statement-only and does not produce a value.

Invalid:

```c
y := (x = 1);
takes_i32(x = 1);
return x = 1;
```

## Compound Assignment

```ebnf
compound_assignment_statement =
    assignable compound_assignment_operator expression;

compound_assignment_operator =
    "+=" | "-=" | "*=" | "/=" | "%=";
```

Examples:

```c
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 2;
values[0] += 1;
```

Rules:

- the left-hand side must denote assignable storage
- both operands must be numeric
- concrete numeric operand types must be compatible
- `%=` requires integer operands
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

```ebnf
field_expression =
    expression "." identifier;
```

Struct field access produces an lvalue only when the base expression is an lvalue.

Enum member access has the same surface syntax:

```c
Color.Red
```

Enum members are values, not assignable storage.

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

