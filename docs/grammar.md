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

## Array Literals

```ebnf
array_literal = "[" [expression {"," expression} [","]] "]";
```

Examples:

```c
values: i32[3] = [1, 2, 3];
values: i32[3] = [1, 2, 3,];
```

Array literals are currently contextual initializers. They require an expected array type from the surrounding declaration.

Valid:

```c
values: i32[3] = [1, 2, 3];
```

Rejected for now:

```c
values := [1, 2, 3];
foo([1, 2, 3]);
```

## String Literals

```ebnf
string_literal = '"' {string_character | escape_sequence} '"';
```

Initial supported escape sequences should include:

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

In the first implementation stage, string literals are contextual initializers for fixed-size byte arrays. They are not yet general expressions.

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

`%=` is integer-only.
