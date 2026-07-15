# Coglet Grammar Notes

This document records the intended surface syntax for currently supported and near-term language features.

It is not yet a complete formal grammar for the full language.

## Array Types

Coglet currently uses suffix array type syntax.

```ebnf
type = base_type {"*"} ["[" integer_literal "]"];
```

Examples:

```c
values: i32[3];
bytes: u8[16];
```

The array size must currently be a constant integer bound.

## Array Indexing

```ebnf
index_expression = expression "[" expression "]";
```

Examples:

```c
values[0]
values[i + 1]
```

Indexing is assignable only when the indexed object is assignable:

```c
values[0] = 1;      // valid when values is mutable storage
make_array()[0] = 1; // invalid: make_array() produces a temporary value
```

## Array Literals

```ebnf
array_literal = "[" [initializer {"," initializer} [","]] "]";
```

Examples:

```c
values: i32[3] = [1, 2, 3];
values: i32[3] = [1, 2, 3,];
```

Array literals are currently contextual initializers. They require an expected array type from the surrounding context.

Valid expected-type contexts include:

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

Rejected for now:

```c
values := [1, 2, 3];
[1, 2, 3];
```

Array literals are not yet general standalone expressions.

## String Literals

```ebnf
string_literal = '"' {string_character | escape_sequence} '"';
```

Initial supported escape sequences include:

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

String literals are contextual initializers for fixed-size byte arrays. They require an expected `u8[N]` destination type.

Valid expected-type contexts include:

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

Rejected for now:

```c
name := "hello";
"hello";
```

String literals are not yet general standalone expressions.

## Assignment

```ebnf
assignment = assignable "=" initializer;
```

Examples:

```c
x = 1;
point.x = 2;
values[0] = 3;
name = "hello";
values = [1, 2, 3];
```

The left-hand side must be semantically assignable storage.

Invalid:

```c
CONSTANT = 1;
Color.Red = Color.Blue;
make_point().x = 1;
make_array()[0] = 1;
```

The right-hand side is checked as an initializer against the left-hand side type. This allows contextual string and array literals in assignment.

## Compound Assignment

```ebnf
compound_assignment = assignable compound_assignment_operator expression;

compound_assignment_operator = "+=" | "-=" | "*=" | "/=" | "%=";
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

The left-hand side must be semantically assignable storage.

Compound assignment currently supports arithmetic compound operators only. `%=` is integer-only.

Unlike plain assignment, the right-hand side of compound assignment is a normal expression, not a contextual initializer.
