# Coglet Grammar Notes

This document records intended surface syntax and current semantic restrictions for supported and near-term language features.

It is not yet a complete formal grammar.

## Modules and Imports

The initial module layer uses top-level directives with one identifier component:

```ebnf
module_declaration = "module" identifier ";";
import_declaration = "import" identifier ";";
qualified_name = identifier "." identifier;
```

A physical file may contain at most one `module` declaration, which must precede
imports and ordinary top-level declarations/statements. Imports are file-scoped
and must precede ordinary top-level declarations/statements. Files without a
module declaration belong to the root namespace. Multiple files may contribute
to one named module.

```c
module math;
Pair::struct { x: i32; y: i32; }
add::(a: i32, b: i32) -> i32 { return a + b; }
```

```c
import math;
main::() -> i32 {
    pair: math.Pair = math.Pair { x = 20, y = 22 };
    return math.add(pair.x, pair.y);
}
```

Qualified enum members use `module.Enum.Member`. Imports currently expose
functions and nominal types through qualification; globals/constants, exports,
packages, dotted module names, and automatic file discovery are deferred. Imports
do not imply runtime dependency ordering and import cycles are allowed.

## Type Syntax

A simplified type grammar is:

```ebnf
type =
    {"readonly" | "volatile"}
    (
        base_type {"*"}
      | "opaque" "*" {"*"}
      | "cfn" "(" [cfn_call_option [","]] [type {"," type} ["," "..."]] ")" ["->" type] {"*"}
    )
    ["[" integer_constant "]"];

cfn_call_option = "call" "=" c_calling_convention;
```

`readonly` and `volatile` are valid only when at least one pointer layer follows the base type. They may appear in either order, each at most once, and qualify the first pointer layer following that base type.

```c
mutable: i32*;
view: readonly i32*;
device: volatile i32*;
status: readonly volatile i32*;
nested: readonly volatile i32**;
```

`readonly volatile i32**` means a mutable outer pointer to a readonly+volatile pointer to `i32`. Additional outer pointer layers remain mutable and non-volatile unless separately represented by another type layer.

These are invalid:

```c
value: readonly i32;
register: volatile i32;
values: readonly i32[4];
```

Named types may be module-qualified as `math.Pair` when `math` is visible through a file import (or is the current module).

Examples of ordinary types:

```c
value: i32;
pointer: i32*;
values: i32[4];
points: Point[8];
```

Fixed-size array bounds must currently be compile-time integer constants.

`cfn(...) -> T` is a native C function-pointer type. Callback type parameters
are types only; names and defaults are not part of function-pointer type syntax.
An optional leading `call=<convention>` selects an explicit native calling
convention, for example `cfn(call=win64, c_int) -> c_int`. Supported convention
names are `cdecl`, `stdcall`, `sysv64`, and `win64`; absence of `call=` means the
platform/default C ABI. Calling convention is part of function-pointer type
identity. A trailing `...` marks a C-variadic function-pointer type and requires
at least one fixed parameter, for example `cfn(c_int, ...) -> c_int`.
`call=stdcall` cannot be variadic. Omitting `-> T` means `-> void`. `cfn` values
are nullable and callable. An ordinary Coglet function does not implicitly become
a `cfn`; a Coglet-defined callback must opt into the C ABI with `#repr(c)` on the
function declaration.

```c
callback: cfn(c_int, opaque*) -> c_int;
win_callback: cfn(call=win64, c_int) -> c_int;
missing: cfn(c_int) -> c_int = null;
```

`readonly`/`volatile` do not qualify a bare `cfn` value. As with any other base type, they may qualify an explicit pointer layer following the function-pointer type.

### Initialization

A local variable declaration may omit an initializer:

```c
value: i32;
```

Typed mutable declarations may group multiple names before one type:

```c
a, b: u64 = 0;
x, y: i32[2] = [1, 2];
p, q: i32;
```

A grouped declaration is defined as left-to-right syntax sugar for separate declarations. The initializer is cloned semantically for each name and is evaluated once per variable. Therefore `a, b: u64 = next();` calls `next()` twice. Grouped declarations do not introduce grouped constants or inferred `:=` syntax.

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

Pointer types use postfix `*`. The optional `readonly` and `volatile` qualifiers describe access through the first pointer layer.

```c
value: i32 = 10;

mutable: i32* = &value;
view: readonly i32* = mutable;
device: volatile i32* = mutable;
status: readonly volatile i32* = mutable;
```

`T*` grants mutable ordinary access to `T`. `readonly T*` grants read access without write permission. `volatile T*` remains writable but marks accesses through the pointer as volatile; `readonly volatile T*` combines both properties.

Dereference and pointer indexing produce lvalues. Their storage access is
determined by the pointer type:

```text
T*                     -> writable ordinary lvalue
readonly T*            -> readonly ordinary lvalue
volatile T*            -> writable volatile lvalue
readonly volatile T*   -> readonly volatile lvalue
```

Address-of requires an lvalue and preserves its storage access. It does not
require the operand to be writable.

Postfix operators bind more tightly than prefix unary operators, so
`*p.field` parses as `*(p.field)`, while `(*p).field` accesses a field through
a pointer.

Pointer qualification is monotonic. Matching pointers may add `readonly`, `volatile`, or both. Neither qualifier may be discarded implicitly. The reverse and
recursive nested-pointer adaptations are rejected.

Pointers with equal immediate pointee types may be compared despite immediate readonly/volatile qualifier differences. All raw-pointer qualifier forms may be
compared with `null`.

Arrays do not decay implicitly to pointers. General pointer arithmetic and
ownership or lifetime checking are not yet supported. `null` is the only
source-level null-pointer value; integer zero does not implicitly or explicitly
become a pointer.

### Opaque raw pointers

The source form `opaque*` denotes a dedicated non-dereferenceable raw-pointer
type. `opaque` without at least one `*` is invalid.

```c
handle: opaque*;
view: readonly opaque*;
out: opaque**;
```

Opaque pointers may independently carry `readonly` and `volatile`, may hold `null`, and may compare while ignoring immediate qualifier differences. Qualifiers may be added monotonically but not discarded. Opaque pointers still cannot be dereferenced or indexed.

Additional `*` layers are ordinary typed pointer layers. Consequently,
`opaque**` may be dereferenced once to obtain an `opaque*`, but the resulting
opaque pointer still cannot be dereferenced.

Typed and opaque raw pointers do not implicitly convert or directly compare.
Conversions between them require `reinterpret`.

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

## Array Zero Initializer

```ebnf
array_zero_initializer =
    "{" "0" "}";
```

`{0}` is a contextual initializer for a fixed-size array. It denotes semantic
zero for the complete destination array; it is not a one-element array literal
and does not enable C partial aggregate initialization.

Supported expected-type contexts match ordinary array literals:

```c
values: i32[3] = {0};
values = {0};
takes_i32_array({0});

make_values::() -> i32[3] {
    return {0};
}
```

Rejected:

```c
values := {0};   // no expected array type
value: i32 = {0};
values: i32[3] = {1};
```

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

Function declarations have two forms:

```ebnf
function_declaration =
      coglet_function_declaration
    | extern_c_function_declaration;

coglet_function_declaration =
    identifier "::"
    "(" [parameter_list] ")"
    ["->" type]
    block;

extern_c_function_declaration =
    "#" "extern" "(" "c" {"," extern_c_option} ")"
    identifier "::"
    "(" [parameter_list ["," "..."]] ")"
    ["->" type]
    ";";

extern_c_option =
      "name" "=" string_literal
    | "call" "=" c_calling_convention;
```

Ordinary Coglet function:

```c
add::(a, b: i32) -> i32 {
    return a + b;
}
```

External C declaration:

```c
#extern(c)
puts::(s: readonly c_char*) -> c_int;

#extern(c)
printf::(format: readonly c_char*, ...) -> c_int;

#extern(c, name="SDL_CreateWindow")
create_window::(title: readonly c_char*) -> opaque*;

#extern(c, call=win64)
platform_probe::(value: c_int) -> c_int;
```

The optional `name="..."` option overrides the external C/linker symbol while
leaving the Coglet function identifier unchanged. `call=<convention>` selects an
explicit C calling convention (`cdecl`, `stdcall`, `sysv64`, or `win64`); without
it, the platform/default C ABI is used. Without `name=`, the Coglet name is used
as the external symbol. `#extern(c)` declarations have no Coglet body,
are terminated by `;`, and are currently restricted semantically to top level.
A trailing `...` is available only for the C ABI; ordinary Coglet function
definitions remain non-variadic. C-variadic declarations require at least one
fixed parameter.

C-compatible struct representation uses the same declaration-annotation shape:

```c
#repr(c)
CPoint::struct {
    x: c_int;
    y: c_double;
}
```

Conceptually:

```text
repr_c_decl :=
    "#" "repr" "(" "c" [repr_c_option {"," repr_c_option}] ")"
    identifier "::"
    (repr_c_struct_body | repr_c_union_body | repr_c_enum_body | repr_c_function_body);

repr_c_option :=
      "packed"
    | "align" "=" integer_literal
    | "call" "=" c_calling_convention;

c_calling_convention := "cdecl" | "stdcall" | "sysv64" | "win64";

repr_c_struct_body :=
    "struct" ("{" {struct_field_decl} "}" | ";");

repr_c_union_body :=
    "union" "{" {struct_field_decl} "}";

repr_c_enum_body :=
    "enum" "(" c_integer_alias ")" "{" [enum_member {"," enum_member} [","]] "}";

repr_c_function_body :=
    "(" [parameter_list] ")" ["->" type] block;
```

`repr` and `c` remain identifiers at the lexer level. `union` is a language
keyword. `#repr(c)` applies to top-level struct, union, enum, and function declarations.
The optional `packed` and `align=N` layout controls apply only to complete struct
and union declarations. `call=<convention>` applies only to `#repr(c)` function
definitions and selects the same explicit C calling convention used by `cfn` and
`#extern(c)`. `align=N` requires a positive power of two and requests a
minimum aggregate alignment; `packed` reduces member alignment. They may be
combined as `#repr(c, packed, align=8)`. Incomplete structs cannot carry layout
controls. C-represented enums require an explicit native C integer backing alias. A
represented struct or union may contain another complete `#repr(c)` aggregate by
value or a positive-length fixed array of supported C field types, including
`#repr(c)` enum values and arrays of those enums. Inline aggregate dependencies
reached directly or through array elements must form an acyclic layout graph.
Unsized and zero-length C-layout array fields are rejected. The semicolon form
currently declares only an incomplete foreign C struct, for example
`#repr(c) SDL_Window::struct;`; incomplete structs have no Coglet field layout
and may only be used behind raw pointers. Incomplete `#repr(c)` unions are not
yet supported.

The C scalar-ABI names `c_char`, `c_schar`, `c_uchar`, `c_short`,
`c_ushort`, `c_int`, `c_uint`, `c_long`, `c_ulong`, `c_longlong`,
`c_ulonglong`, `c_size`, `c_bool`, `c_float`, and `c_double` are builtin type
aliases resolved through the selected native C ABI. They are ordinary
identifiers at the lexer level, so adding them does not expand the keyword set.

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
string literals, array literals, and the fixed-array `{0}` zero initializer.

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

Checked, truncating, and raw-pointer reinterpretation conversions share this
surface grammar:

```ebnf
conversion_expression =
      "cast" "(" type "," expression ")"
    | "truncate" "(" type "," expression ")"
    | "reinterpret" "(" type "," expression ")";
```

`cast(TargetType, expression)` is checked and value-preserving. It also permits
safe qualification conversions from `T*` to `readonly T*`, `volatile T*`, or `readonly volatile T*`, but never qualifier removal.

`truncate(TargetIntegerType, expression)` accepts only integer sources and
concrete integer targets. It retains the low destination-width bits and
interprets them using the target signedness.

`reinterpret(TargetPointerType, expression)` crosses only between a top-level
typed raw pointer and a top-level opaque raw pointer. It preserves address bits,
never removes readonly or volatile access, and is not a general `T*`-to-`U*` cast.

## Control-Flow Statement Bodies and `for` Headers

`if`, `else`, `while`, and `for` control one statement. A lexical block is one
statement, so braces are optional for a single statement and required when a
controlled body contains multiple statements:

```c
if n == 0
    return 0;

if (n == 1) {
    return 1;
}

while i < limit
    i++;
```

An unbraced body still introduces the same lexical scope as a braced body. For
example, a variable declared as the sole body of an `if` is not visible after
the `if`. `else` associates with the nearest unmatched `if`.

The compact Coglet `for` header remains supported, with or without parentheses:

```c
for i < limit : i++ {
    work(i);
}

for (i < limit : i++)
    work(i);
```

A parenthesized three-clause form is also supported:

```c
for (i: u32 = 0; i < limit; i++) {
    work(i);
}
```

Its clauses are `initializer; condition; post`. The initializer may be a local
value declaration or expression statement. Each clause may be omitted where
its separators remain, so `for (;;) { ... }` is an infinite loop. An omitted
condition is treated as always true. The initializer executes once and its
lexical scope contains the condition, body, and post clause but ends after the
loop. The three-clause form requires parentheses; the unparenthesized compact
form continues to use `condition : post`.

The parser normalizes unbraced control bodies into lexical blocks and lowers a
three-clause initializer as a surrounding lexical block followed by the existing
`for` node. These surface variants therefore do not add backend-specific loop
semantics.

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
#repr(c)
Color :: enum(u16) {
    Red = 0,
    Green = 1,
    Blue = 2,
}
```

The annotation is not currently implemented and will concern ABI representation, not enum openness.
