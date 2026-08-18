# Coglet Grammar Notes

This document records intended surface syntax and current semantic restrictions for supported and near-term language features.

It is not yet a complete formal grammar.

## Modules and Imports

Module and import names are absolute dotted identifier paths:

```ebnf
module_name = identifier {"." identifier};
module_declaration = "module" module_name ";";
import_declaration = "import" module_name ["as" identifier] ";";
exported_declaration = "export" top_level_declaration;
qualified_name = identifier {"." identifier};
```

A physical file may contain at most one `module` declaration, which must precede
imports and ordinary top-level declarations/statements. Imports are file-scoped
and must precede ordinary top-level declarations/statements. Files without a
module declaration belong to the root namespace. Multiple files may contribute
to one named module.

`export` is contextual rather than a globally reserved lexer keyword. It is
recognized only as a top-level declaration prefix; declarations in named modules
are private when the prefix is absent. `export` is invalid in the root namespace.

```c
module std.math;
export Pair::struct { x: s32; y: s32; }
export add::(a: s32, b: s32) -> s32 { return a + b; }

helper::() -> s32 { return 1; } // private to module std.math
```

```c
import std.math;
main::() -> s32 {
    pair: std.math.Pair = std.math.Pair { x = 20, y = 22 };
    return std.math.add(pair.x, pair.y);
}
```

An import may bind a shorter file-local qualifier without changing the canonical
module identity or source-discovery path:

```c
import std.math as math;

main::() -> s32 {
    pair: math.Pair = math.Pair { x = 20, y = 22 };
    return math.add(pair.x, pair.y);
}
```

The alias replaces the canonical qualifier in that file: after
`import std.math as math;`, use `math.*`, not `std.math.*`. Aliases are simple
identifiers, are file-scoped, and must not collide with another visible import
qualifier. Source discovery still uses the canonical `std.math` name.

The same dot syntax composes module qualification, enum qualification, and
ordinary runtime field selection: `std.math.add`, `std.math.Mode.Red`, and
`state.point.x`. Semantic resolution chooses the longest module prefix visible
in the current file, then resolves the remaining suffix according to declaration
kind. This permits hierarchical module names without introducing a second
namespace operator. Importing a child module does not implicitly import any
parent module, and importing a parent does not implicitly expose its children.

A qualified global remains an lvalue, so further ordinary field/index access such
as `state.data.point.x` or `state.data.values[0]` composes normally. Imported
qualification requires the selected declaration to be exported; same-module code
may use private members. An exported declaration may not expose a private nominal
type in its public type surface.

When command-line discovery is enabled, a dotted module name maps directly to a
relative source path by replacing dots with path separators: `import std.math;`
searches for `std/math.cog` beside the importing source and then under ordered
`-I` roots. Unresolved names in the standard-library `std` namespace additionally consult
the compiler-configured standard-library module root as the final fallback; this
discovery policy does not change the grammar. The CLI may override that root with
`--stdlib-root`, while non-`std` module names never consult it. Imports do not imply
runtime dependency ordering and import cycles are allowed. Package manifests, broader/runtime-facing standard-library contents, and separate compilation remain deferred; the first shipped `std.math` module uses these ordinary module/import rules without adding grammar.

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
    ["[" [integer_constant] "]"];

cfn_call_option = "call" "=" c_calling_convention;
```

The reserved scalar type spellings include `bool`, `s8`/`s16`/`s32`/`s64`,
`u8`/`u16`/`u32`/`u64`, `isize`, `usize`, `f32`, `f64`, and `void`. `isize` and
`usize` resolve to the signed/unsigned fixed-width integer matching the selected
target pointer width; they do not introduce separate nominal integer kinds. The
`c_*` ABI aliases described below remain semantic builtin identifiers rather than
lexer keywords.

`T[N]` is fixed-size owned array storage. `T[]` is a non-owning mutable slice, represented semantically as a pointer-and-length view; `readonly T[]` is the corresponding readonly view. A slice has no capacity or ownership semantics in this first version.

`readonly` and `volatile` still qualify the first pointer layer when a pointer follows the base type. When no pointer layer consumes `readonly`, it may instead qualify an empty `[]` slice suffix. `volatile` slices are not supported.

```c
mutable: s32*;
view: readonly s32*;
device: volatile s32*;
status: readonly volatile s32*;
values: s32[];
readonly_values: readonly s32[];
```

These are invalid:

```c
value: readonly s32;
register: volatile s32;
fixed: readonly s32[4];
volatile_view: volatile s32[];
```

For a pointer element type, the existing prefix qualifier still binds to the first pointer layer. The language does not yet have separate syntax for independently qualifying both that pointer layer and the enclosing slice.

Named types may be module-qualified through any visible hierarchical module path, such as `std.math.Pair` (or through the current module).

Examples of ordinary types:

```c
value: s32;
pointer: s32*;
values: s32[4];
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
value: s32;
```

Typed mutable declarations may group multiple names before one type:

```c
a, b: u64 = 0;
x, y: s32[2] = [1, 2];
p, q: s32;
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
    | "move" unary_expression
    | primary_expression;
```

Integer literals may be decimal or use the `0x`/`0X`, `0b`/`0B`, and `0o`/`0O` radix prefixes. Floating-point literals may use decimal notation with an optional `e`/`E` decimal exponent or hexadecimal notation with a mandatory `p`/`P` binary exponent:

```ebnf
decimal_float =
      decimal_digits "." decimal_digits [decimal_exponent]
    | decimal_digits decimal_exponent;

hex_float =
    ("0x" | "0X")
    (hex_digits ["." [hex_digits]] | "." hex_digits)
    ("p" | "P") ["+" | "-"] decimal_digits;
```

Examples:

```c
1.25
1e3
1.25e-4
0x1p0
0x1.8p1
0x1.921fb54442d18p+1
0x.8p-2
```

The hexadecimal significand is base 16, while the `p` exponent scales by a power of two. A hexadecimal point without a `p`/`P` exponent is invalid, which keeps `0x1e3` unambiguously an integer literal. Both decimal and hexadecimal floating-point spellings produce the same adaptable `untyped-float` semantic kind.

For example, `-2147483648` is parsed as unary negation applied to the positive literal `2147483648`. Numeric literals initially have adaptable `untyped-int` or `untyped-float` semantic types. Inferred mutable variables and parameters are concretized to default runtime types, while inferred compile-time constants may remain adaptable.

## Raw Object Pointers

Pointer types use postfix `*`. The optional `readonly` and `volatile` qualifiers describe access through the first pointer layer.

```c
value: s32 = 10;

mutable: s32* = &value;
view: readonly s32* = mutable;
device: volatile s32* = mutable;
status: readonly volatile s32* = mutable;
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
values: s32[3] = [1, 2, 3];
values: s32[3] = [1, 2, 3,];
```

Array literals are contextual initializers. They require an expected array type from the surrounding context.

Supported expected-type contexts include:

```c
values: s32[3] = [1, 2, 3];

values = [1, 2, 3];

takes_s32_array([1, 2, 3]);

make_values::() -> s32[3] {
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
values: s32[3] = {0};
values = {0};
takes_s32_array({0});

make_values::() -> s32[3] {
    return {0};
}
```

Rejected:

```c
values := {0};   // no expected array type
value: s32 = {0};
values: s32[3] = {1};
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

String literals are immutable compile-time byte data. Standalone literals have type `readonly u8[]`; their visible slice length is the decoded byte length, while the compiler-owned backing storage also carries a trailing NUL byte. They may still contextually initialize fixed-size `u8[N]` arrays, where `N` must equal the decoded byte length plus that trailing NUL.

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

Standalone string literals are ordinary readonly byte-slice expressions and,
like other non-call value expressions, may appear as standalone statements:

```c
name := "hello";        // readonly u8[]
"hello";                // valid standalone expression
```

## Function Declarations and Calls

Function declarations have two forms:

```ebnf
function_declaration =
      coglet_function_declaration
    | extern_c_function_declaration;

discardable_function_declaration =
    "#" "discardable" function_declaration;

coglet_function_declaration =
    identifier "::"
    [generic_type_parameter_list]
    "(" [parameter_list] ")"
    ["->" type]
    block;

generic_type_parameter_list =
    "<" generic_type_parameter {"," generic_type_parameter} ">";

generic_type_parameter =
    identifier [":" generic_builtin_constraint];

generic_builtin_constraint =
      "integer"
    | "signed_integer"
    | "unsigned_integer"
    | "floating"
    | "numeric"
    | "ordered";

parameter_list =
    parameter_group {"," parameter_group};

parameter_group =
    identifier {"," identifier} ":" type;

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
add::(a, b: s32) -> s32 {
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

### Generic functions and structs

Ordinary top-level Coglet functions and structs may declare type parameters
immediately after `::`:

```c
min::<T: ordered>(a: T, b: T) -> T {
    if a < b
        return a;
    return b;
}

Pair::<T, U> struct {
    first: T;
    second: U;
}
```

Conceptually, generic structs and struct members are:

```ebnf
generic_struct_declaration =
    identifier "::" generic_type_parameter_list
    ("struct" | "resource") "{" {struct_member} "}";

struct_member =
      struct_field_decl
    | struct_method_decl
    | struct_operator_block;

struct_method_decl =
    identifier "::"
    "(" [parameter_list] ")"
    ["->" type]
    block;

struct_operator_block =
    "operators" "{" {struct_operator_mapping} "}";

struct_operator_mapping =
      binary_struct_operator "=" identifier ";"
    | "unary" "-" "=" identifier ";";

binary_struct_operator = "+" | "-" | "*" | "/";

generic_type_application =
    named_type "::" "<" type {"," type} ">";
```

The same `struct_member` rule applies to ordinary complete Coglet structs. A
method declaration with first parameter `self` is an instance method; one without
`self` is an associated function. `Self` denotes the concrete owner inside method
signatures and bodies. Generic methods (`method::<U>(...)`) are not part of the
initial method grammar.

The identifier `operators` is contextual: it starts an operator-mapping block only
when followed by `{` in a struct body, so it is not a globally reserved keyword.
`unary` is likewise contextual inside that block. Semantic analysis resolves each
mapping to a method on the same concrete struct and validates the method signature.

Move-only owning aggregates use the same member grammar as ordinary structs:

```ebnf
resource_declaration =
    identifier "::" "resource" "{" {struct_member} "}";
```

Generic resources use `resource` in place of `struct`, for example
`Array::<T> resource { ... }`. `move value` is a prefix expression that explicitly
transfers an existing owner. The first implementation requires `move` to name a
direct local or parameter whose type is a resource (or a fixed array containing
resources); moving out of fields and globals is intentionally deferred.

Calls use:

```c
value.method(args);
Point.new(args);
Vec2::<f32>.new(args);
```

A generic type application followed by `.` is parsed as a type-qualified
associated call. This context also preserves the existing interpretation of `>>`
as nested generic-list closing delimiters while ordinary expression `>>` remains
the shift operator.

Concrete generic struct types and constructors use explicit arguments:

```c
pair: Pair::<s32, f32> = Pair::<s32, f32> {
    first = 1,
    second = 2.5
};
```

Nested type applications may close with adjacent `>` characters, for example
`Box::<Pair::<s32, f32>>`. In generic-list context the parser interprets lexical
`>>` as two closing delimiters; expression syntax continues to interpret `>>` as
the shift operator.

A generic function call may supply all type arguments explicitly using the same
`::<...>` marker:

```c
small := min::<s64>(10, 20);
value := first::<s32, u64>(1, 2);
```

When ordinary arguments determine every function type parameter unambiguously,
the call uses ordinary call spelling and semantic analysis infers the arguments.
Inference can recurse through pointer/array/slice/function shapes and through a
concrete generic-struct shape such as `Pair::<T, U>`. Generic struct type uses and
constructors do not infer their own type arguments in this first version.

Explicit arguments are all-or-nothing. A type parameter may carry one closed
compiler-defined constraint. Constraints are not user-declared traits and cannot
be combined or implemented by user types. Generic enums, unions, aliases,
`#repr(c)` aggregates, nested generic declarations, and generic C ABI
declarations remain outside this grammar.

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

A call returning `void` is a no-value expression and is valid as a bare
statement:

```c
does_nothing(); // valid
```

It is invalid where a value is required:

```c
x := does_nothing();
takes_s32(does_nothing());
does_nothing() + 1;
```

A non-void function-call result is must-use by default. A direct call may appear
bare only when its declaration carries `#discardable`; otherwise the caller must
use the value or write an explicit statement-position `discard`:

```c
value := compute();
discard compute();

#discardable
probe::() -> s32 { return 0; }
probe(); // valid
```

`#discardable` is allowed only on value-returning functions whose return type does
not contain a move-only resource. It is declaration metadata rather than function
type/ABI identity. When combined with `#extern(c)` or `#repr(c)`, it precedes that
ABI attribute. Indirect calls therefore remain must-use unless explicitly
discarded.

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
takes_s32(x = 1);
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
takes_s32(x--);
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
- the expression form registered by `defer`

Void-returning calls are accepted in statement position but rejected in value-required contexts.

Value-returning function calls are must-use in statement position unless the
source form is a direct call to a `#discardable` function. Other value-producing
expressions, including arithmetic and comparisons, remain valid statement
expressions without `discard`. `discard` explicitly consumes the complete following
assignment-level expression and itself produces no value; using it inside a
value-required context is an error. Resource-owning values may not be discarded.

Mutation operations are accepted only in statement position.

## Conditional Expressions

Conditional expressions use the same braced control-flow shape as `if` statements, but require an `else` value and produce a value:

```c
value := if condition {
    when_true
} else {
    when_false
};
```

The branch bodies currently contain one value expression. `else if` chains are accepted. Both branch values must have compatible types; exact matches and the language\'s existing contextual numeric/pointer compatibility rules are used. Only the selected branch is evaluated, and ownership-flow analysis merges the two branch states like ordinary conditional control flow.

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

## Runtime Type Layout Queries

Layout queries use dedicated type-query syntax rather than ordinary generic-call
syntax:

```ebnf
layout_query_expression =
    ("size_of" | "align_of") "(" type ")";
```

```c
bytes := size_of(Packet);
alignment := align_of(Packet);
```

The operand is a type, not a runtime expression, and the result type is `usize`.
Generic code may name an in-scope type parameter (`size_of(T)`), while ordinary
generic functions and generic types continue to use `::<...>`. The legacy-looking
`size_of::<T>()` / `align_of::<T>()` spelling is rejected with a migration
diagnostic.

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

## Static Assertions

```ebnf
static_assert_statement =
    "static_assert" "(" constant_boolean_expression
        ["," string_literal] ")" ";";
```

`static_assert` is a reserved statement keyword and may appear at top level or wherever an ordinary statement is accepted. Its condition must have type `bool` and must be evaluable by the frontend compile-time constant evaluator. A false condition is a semantic error; when present, the string literal is appended to the diagnostic. The assertion is semantic-only and emits no runtime operation.

Assertions inside generic function bodies are checked when the concrete specialization body is checked. Target-layout queries `size_of(T)` and `align_of(T)` are compile-time constants for concrete runtime object types and may be used inside `static_assert`.

## Deferred cleanup

```ebnf
defer_statement =
      "defer" expression ";"
    | "defer" block;
```

Deferred bodies execute in reverse registration order when their lexical block
is left, including exits through `return`, `break`, and `continue`. A deferred
value-returning call follows the function-result must-use rule, so use
`defer discard call();` unless the direct target is `#discardable`. The first
version rejects nested `defer` and control-transfer
statements inside a deferred body.

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

C-compatible enum representation uses the implemented `#repr(c)` annotation
and requires an explicit native C integer backing alias:

```c
#repr(c)
Color :: enum(c_uint) {
    Red = 0,
    Green = 1,
    Blue = 2,
}
```

`#repr(c)` changes the ABI representation contract, not enum openness: represented
enums remain closed to their declared runtime values.
