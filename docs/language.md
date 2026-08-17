# Coglet Language Notes

Coglet is a modern systems language focused on explicit semantics, predictable behavior, and a compiler architecture that can grow without accumulating technical debt.

This document records current user-visible language behavior.

## Modules and Imports

A physical source file may optionally begin with an absolute dotted module name:

```c
module std.math;

export tau :: 6.283185307179586;
export counter: s32 = 0;
export add::(a: s32, b: s32) -> s32 { return a + b; }

helper::() -> s32 { return 1; } // private
```

Another file imports that namespace explicitly and uses the same dot syntax for
qualified lookup:

```c
import std.math;

main::() -> s32 {
    std.math.counter = std.math.add(20, 22);
    return std.math.counter;
}
```

For shorter local spelling, an import may declare a file-local alias:

```c
import std.math as math;

main::() -> s32 {
    math.counter = math.add(20, 22);
    return math.counter;
}
```

The canonical module remains `std.math`; the alias affects only qualification in
the importing file. Automatic discovery, diagnostics about the imported module,
and module identity continue to use the canonical name. An aliased import is
qualified through the alias in that file, so `std.math.*` is not simultaneously
made visible by `import std.math as math;`.

Module names may contain any number of identifier components (`math`,
`std.math`, `vendor.graphics.math`). Files without a module declaration belong
to the root namespace. Multiple physical files may contribute declarations to
the same named module. Imports are file-scoped: importing `std.math` in one file
does not make it visible in another file automatically, and it does not
implicitly import the parent module `std`.

The command-line compiler automatically discovers a missing imported module using
one canonical source path. Dots map to path separators, so `import std.math;`
searches for `std/math.cog` first beside the importing file and then under
repeated `-I` roots in command-line order:

```sh
coglet app/main.cog -I lib -I vendor -o app
```

If no candidate has been found and the canonical module name is `std` or starts
with `std.`, the compiler consults its configured standard-library module root as
a final fallback. The root contains the top-level `std` directory, so a normal
installation with root `<prefix>/lib/coglet` resolves `std.math` as
`<prefix>/lib/coglet/std/math.cog`. Runtime-backed modules such as `std.io` use
the same root and expect their implementation beneath `<stdlib-root>/runtime/`.
`--stdlib-root <dir>` replaces this fallback
for one compiler invocation and `--print-stdlib-root` reports the configured
default. Local/importer files and `-I` roots intentionally win over the installed
stdlib; module names outside `std` never consult the stdlib root.

The first existing candidate wins and its imports are discovered transitively.
Explicit inputs are parsed before discovery; if any explicit source already
declares `module std.math;`, no canonical `std/math.cog` is loaded for that
import. Multi-file module fragments beyond the canonical discovered file remain
explicit inputs until a package/manifest layer exists.

Declarations in a named module are private by default. A top-level contextual
`export` prefix makes a declaration visible to importing files:

```c
module std.math;

export Pair::struct { x: s32; y: s32; }
export answer :: 42;

secret :: 7;
```

Same-module code may use `secret` normally, including as `std.math.secret`. A
file that merely `import std.math;` may access `std.math.Pair` and
`std.math.answer` but receives a privacy diagnostic for `std.math.secret`.
`export` is rejected in the root namespace. Exported declarations are checked
for interface closure: a public function, global, constant, or exported struct
field may not expose a private nominal type.

The single dot is intentionally both the module qualifier and ordinary member
operator. Semantic analysis chooses the longest module prefix visible in the
current file. Therefore, if both `pkg` and `pkg.math` are imported,
`pkg.math.answer` resolves through module `pkg.math`; if only `pkg` is imported,
the same token shape may validly mean exported value `pkg.math` followed by its
ordinary `answer` field. A lexical symbol still shadows a module name in
expression lookup.

Qualified functions and constants are rvalues. Qualified globals retain their
ordinary storage category, so mutation, address-of, indexing, and field selection
compose normally:

```c
state.data.counter += 1;
pointer := &state.data.counter;
state.data.values[0] = 7;
state.data.point.x = 9;
```

Qualified constants retain compile-time constant behavior and may be used
wherever the corresponding unqualified constant is valid, including other
constant declarations and switch case expressions. Their dependencies are
resolved independently of physical input-file order, and cyclic constant
definitions are rejected. Function bodies likewise see imported global
declarations regardless of input-file ordering; runtime-bearing top-level
initialization itself still executes in physical input order. Nominal types,
constructors, and enum members use the same hierarchical spelling
(`std.math.Pair`, `std.math.Pair { ... }`, `std.math.Mode.Red`).

The current module layer remains compile-time namespace/visibility only. Import
cycles are allowed, and imports do not define runtime dependency ordering.
Explicit files retain command-line order; discovered files are appended after
them in first-discovery order, and that physical order remains the
module-initializer order. Only a root-namespace `main::() -> s32` is the
executable entry. Package manifests, automatic multi-file package membership,
actual standard-library modules, and separate compilation remain future work.

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
x: s32 = 1;
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
takes_s32(x += 1);
return x++;
1 + (x = 2);
```

Successful mutation nodes produce no value.

## Definite Assignment and Reachability

A local variable declaration does not implicitly initialize the variable:

```c
value: s32;
```

The variable exists and has type `s32`, but its value cannot be read until semantic analysis can prove that it has been initialized on every reachable incoming control-flow path.

A plain whole-variable assignment initializes it:

```c
value: s32;

value = 10;

return value; // valid
```

Parameters and local variables declared with initializers begin initialized:

```c
use_value::(parameter: s32) -> s32 {
    local: s32 = 10;

    return parameter + local;
}
```

Typed mutable declarations may bind more than one name:

```c
a, b: u64 = 0;
p, q: s32;
```

This is exact left-to-right sugar for separate declarations. In particular:

```c
a, b: u64 = next();
```

is equivalent to:

```c
a: u64 = next();
b: u64 = next();
```

The initializer is therefore evaluated independently for every declared variable. Each declaration retains ordinary value-copy semantics, so grouped array declarations also produce distinct array values. Grouped syntax currently applies only to typed mutable declarations; constants and inferred `:=` declarations remain single-name forms.

Definite-assignment analysis applies to function-local variables and parameters. Global variables are not tracked by this local flow analysis.

### Reads and writes

An ordinary identifier use reads the variable and therefore requires prior initialization:

```c
value: s32;

other := value; // invalid
```

A direct plain-assignment target does not read the previous value:

```c
value: s32;

value = 10; // valid: initializes value
```

Compound assignment and increment/decrement read the previous value before writing it:

```c
value: s32;

value += 1; // invalid
value++;    // invalid
value--;    // invalid
```

Taking the address of a local also requires the local to be initialized:

```c
value: s32;

pointer := &value; // invalid
```

This prevents an uninitialized local from becoming observable indirectly through a pointer.

### Whole values and subobjects

Assigning a complete struct or array variable initializes that variable:

```c
values: s32[3];

values = [1, 2, 3];

first := values[0]; // valid
```

Writing only a field, element, pointee, or pointer-indexed location does not initialize the complete base variable:

```c
point: Point;
values: s32[3];
pointer: s32* = get_pointer();

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
value: s32;

if condition {
    value = 10;
} else {
    value = 20;
}

return value; // valid
```

Without an `else`, the unchanged incoming path remains possible:

```c
value: s32;

if condition {
    value = 10;
}

return value; // invalid
```

A branch that cannot continue does not weaken a branch that can:

```c
value: s32;

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

### Control-flow bodies

`if`, `else`, `while`, and `for` accept either one statement or a braced block.
A one-statement body still creates a lexical scope, so declaration visibility is
independent of whether braces were written. `else` binds to the nearest unmatched
`if`.

```c
if n == 0
    return 0;

while i < limit
    i++;
```

### Loops

The compact `for condition : post` syntax remains available and may optionally
parenthesize its header. Coglet also accepts a parenthesized three-clause form:

```c
for (i: u32 = 0; i < limit; i++) {
    work(i);
}
```

The initializer executes once in a lexical scope containing the condition, body,
and post expression; that scope ends after the loop. The initializer, condition,
and post clauses may be omitted as appropriate, including `for (;;)`. The
three-clause form requires parentheses.

Loop analysis is intentionally conservative. Initialization performed only during an iteration is not generally available after the loop:

```c
value: s32;

while condition {
    value = 10;
}

return value; // invalid
```

The same rule applies to `for` loops.

After any three-clause initializer has executed, `for` flow is checked in runtime order:

1. condition;
2. body;
3. post expression.

Normal body fallthrough and `continue` paths reach the post expression. `break` and `return` paths do not.

`break` and `continue` apply to the nearest enclosing loop.

A loop whose Boolean condition is known at compile time to be `true` and has no reachable `break` does not continue to the statement following the loop. This includes the literal `true`, named/local constants, and other checked constant Boolean expressions:

```c
run_forever::() -> s32 {
    while true {
    }
}
```

This function is valid even though it contains no `return`, because normal control flow cannot reach the end of the function.

The same rule applies when compile-time constant evaluation proves the condition true:

```c
ALWAYS :: true;

run_forever::() -> s32 {
    while ALWAYS {
    }
}
```

A compile-time-true loop with a reachable `break` may continue after the loop and does not satisfy a non-void function's return obligation by itself.

### Unreachable statements

`return`, `break`, and `continue` make the remainder of their current control-flow path unreachable.

Coglet reports unreachable statements during block traversal:

```c
test::() -> s32 {
    return 10;

    value := 20; // unreachable
}
```

A non-void function is valid when normal control flow cannot reach the end of its body. This includes both explicit returns and provably non-continuing control flow.

### Nested functions

Nested functions do not currently support closure capture.

A nested function cannot read, modify, or take the address of a local variable or parameter belonging to an enclosing function:

```c
outer::() -> s32 {
    value: s32 = 10;

    inner::() -> s32 {
        return value; // invalid: capture is not supported
    }

    return 0;
}
```

Nested functions may still refer to visible global variables, compile-time constants, types, and function declarations.

Closure environments and captured runtime storage remain future language-design work.


## Exact Function Overloads

Ordinary non-generic Coglet functions may share a name when their concrete
parameter type lists differ. Overload resolution is intentionally exact and does
not use C++-style conversion ranking:

```c
measure::(value: f32) -> f32 { return value; }
measure::(value: f64) -> f64 { return value; }

test::() -> void {
    x: f32 = 1.0;
    a := measure(x);   // measure(f32)
    b := measure(1.0); // untyped float defaults to f64, then measure(f64)
}
```

Arguments are checked once and untyped numeric literals use the language's normal
inference/defaulting rules before overload selection. A candidate matches only when
each resulting argument type exactly equals the corresponding parameter type. The
return type is not used to choose an overload, and there is no implicit-conversion
ranking, best-match scoring, or source-order tie breaking.

Two overloads may not differ only by return type because their parameter lists are
identical. This first overload facility is limited to ordinary non-generic,
non-variadic Coglet functions. `#extern(c)`, `#repr(c)`, generic functions, methods,
and the executable `main` entry point do not form overload sets. An overloaded
function name is currently usable only as a direct call target; selecting an
overload as a first-class function value requires future function-type contextual
resolution.

Overloads are ordinary semantic functions. The selected concrete declaration is
recorded before CogIR lowering, so CogIR and both backends see a normal direct
function reference and contain no overload-resolution machinery.

## Generic Functions and Structs

Coglet generics are deliberately frontend-only and compile-time monomorphized.
The current language supports ordinary top-level generic functions and ordinary
top-level generic structs. Both use the same `::<...>` declaration marker and
closed builtin constraints.

Generic function example:

```c
identity::<T>(value: T) -> T {
    return value;
}
```

Generic struct example:

```c
Pair::<T, U> struct {
    first: T;
    second: U;
}

Vec2::<T: numeric> struct {
    x: T;
    y: T;
}
```

A concrete generic struct type always supplies all type arguments explicitly:

```c
pair: Pair::<s32, f32> = Pair::<s32, f32> {
    first = 7,
    second = 2.5
};

point := Vec2::<f32> { x = 1.0, y = 2.0 };
```

Nested applications use the same type syntax, including adjacent closing `>`
delimiters:

```c
box: Box::<Pair::<s32, f32>>;
```

Generic struct type arguments are not inferred from an initializer or expected
type in this first version. A bare use of a generic struct template is an error.
Generic functions retain their existing argument-based inference: a call either
supplies all concrete type arguments explicitly (`min::<s64>(a, b)`) or omits the
list and infers them from ordinary function arguments. The bottom-up checker does
not use an expected result type for inference. Generic-function inference also
propagates through concrete generic-struct shapes, so a parameter such as
`Pair::<T, U>` can infer `T` and `U` from an argument of type
`Pair::<s32, f32>`, including when the template is defined in an imported module.

A type parameter may carry one closed compiler-defined constraint:

```c
min::<T: ordered>(a: T, b: T) -> T {
    if a < b
        return a;
    return b;
}

Vec3::<T: floating> struct {
    x: T;
    y: T;
    z: T;
}
```

The builtin constraints are deliberately small:

- `integer`: any concrete signed or unsigned integer type;
- `signed_integer`: `s8`, `s16`, `s32`, or `s64`;
- `unsigned_integer`: `u8`, `u16`, `u32`, or `u64`;
- `floating`: `f32` or `f64`;
- `numeric`: any concrete integer or floating-point type;
- `ordered`: the concrete types accepted by Coglet's ordered comparisons,
  currently the same integer and floating-point domain as `numeric`.

A constraint is an early admissibility contract, not permission to bypass the
ordinary type system. Generic function bodies are still checked under concrete
substitution using the ordinary operator/call/flow/conversion rules, and generic
struct fields are resolved using ordinary concrete type rules. Constraints do not
steer literal defaulting or invent a satisfying type. There are no user-defined
constraints, trait/interface declarations, implementations, or constraint
composition rules. `ordered` means Coglet defines `<`, `<=`, `>`, and `>=` for
the type; it does not promise a mathematical total-order law.

Each specialization is identified by the generic template's stable semantic
declaration ID plus the structural concrete type-argument list. Generated display
names such as `Pair<s32, f32>` are diagnostic/debug names only and are not cache
keys. Repeated equivalent requests reuse one concrete semantic specialization.

Recursive generic structs are supported when the concrete layout is finite. A
self-reference behind a pointer is valid:

```c
Node::<T> struct {
    value: T;
    next: Node::<T>*;
}
```

A by-value recursive layout is rejected, and a recursion that continually creates
new concrete type arguments is bounded and diagnosed instead of specializing
forever. Generic structs obey the same module/export visibility model as ordinary
nominal types; exported interfaces may not expose private generic templates or
private concrete type arguments.

Generic templates never reach CogIR. Generic functions lower only as concrete
ordinary functions, and generic structs lower only as concrete ordinary nominal
struct layouts. Generic enums, generic unions, generic aliases, generic
`#repr(c)` aggregates, nested generic declarations, specialization, dynamic
dispatch, type erasure, user-defined traits, and separate generic compilation
remain outside this first aggregate-generic facility.

## Struct Methods and Associated Functions

Ordinary complete Coglet structs may declare functions directly in the struct
body. Fields still use `name: Type;`; a function member uses the same `name::(...)`
spelling as an ordinary Coglet function. No separate `impl` block or out-of-class
member-definition syntax exists.

```c
Vec3::<T: floating> struct {
    x: T;
    y: T;
    z: T;

    new::(x: T, y: T, z: T) -> Self {
        return Self { x = x, y = y, z = z };
    }

    length_squared::(self: readonly Self*) -> T {
        return (*self).x * (*self).x +
               (*self).y * (*self).y +
               (*self).z * (*self).z;
    }

    set_x::(self: Self*, value: T) -> void {
        (*self).x = value;
    }
}
```

`Self` is a method-scope type alias for the concrete owning struct. For a generic
owner it includes the concrete type arguments automatically. This avoids repeating
long names such as `Vec3::<T>` in every signature and constructor.

A member whose first parameter is named exactly `self` is an instance method. The
receiver type must be `Self`, `Self*`, or a qualified pointer to `Self`. A member
without a `self` parameter is an associated function. A parameter named `self` in
any later position is rejected.

Calls use ordinary member spelling, and value-returning calls may be chained:

```c
point := Vec3::<f32>.new(1.0, 2.0, 3.0);
length2 := point.length_squared();
point.set_x(5.0);
scaled := point.add(offset).scale(0.5);
```

For a pointer receiver, calling through an addressable struct value implicitly
passes its address. The compiler does not implicitly borrow a temporary; a
pointer-receiver call therefore requires addressable storage. A by-value receiver
is passed as an ordinary value. Method bodies currently use explicit pointer
dereference for field access, for example `(*self).x`.

Methods on generic structs are specialized together with their concrete owning
struct. Their concrete signatures are registered with the specialization, while a
method body is checked lazily the first time that concrete method is used. This
allows a broad owner constraint such as `T: numeric` to coexist with a method whose
body is valid only for floating concrete specializations: unused invalid method
bodies do not invalidate the owning type, but calling one produces the ordinary
body diagnostic plus a specialization-use summary. Successfully checked bodies are
cached and lower normally.

Associated and instance calls are resolved semantically and rewritten to ordinary
concrete function calls with an explicit receiver before CogIR lowering. CogIR and
both backends contain no method-dispatch concept. There is no virtual dispatch,
method overloading, extension-method mechanism, generic method type parameter list,
or operator overloading in this first version.

## Void-Returning Calls

A call to a function returning `void` is a successful no-value expression.

Valid when discarded:

```c
does_nothing();
```

Invalid when a value is required:

```c
x := does_nothing();
takes_s32(does_nothing());

bad::() -> s32 {
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
integer literal        -> untyped-int
floating-point literal -> untyped-float
```

Floating-point source may use ordinary decimal notation or hexadecimal notation with a mandatory binary `p`/`P` exponent:

```c
decimal := 3.141592653589793;
hex     := 0x1.921fb54442d18p+1;
three   := 0x1.8p1;
small   := 0x.8p-2;
```

The hexadecimal significand is base 16 and the exponent is a power of two. `0x1p0` therefore denotes 1, while `0x1.8p1` denotes 3. Hexadecimal floating-point spelling does not introduce a distinct semantic type: it is still an adaptable `untyped-float` constant and follows the same contextual `f32`/`f64` materialization rules as a decimal floating literal. A `p`/`P` exponent is required for hexadecimal floats; without a point or `p` exponent, forms such as `0x1e3` remain hexadecimal integers.

The exact integer value is retained independently of a concrete machine type. Negative values are parsed as unary negation applied to a positive literal:

```text
-2147483648

unary '-'
└── number 2147483648
```

This permits correct handling of signed minimum values.

Mutable inferred storage receives a concrete default type:

```c
a := 1;                    // s32
b := 2147483648;           // s64
c := 9223372036854775808;  // u64
d := 1.5;                  // f64
```

Inferred compile-time constants remain adaptable:

```c
A :: 255;  // untyped-int
B :: 1.5;  // untyped-float

small: u8 = A;
wide: s64 = A;
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

signed: s32 = 1;
unsigned: u32 = 1;

signed + unsigned; // invalid: use an explicit cast
```

Compile-time integer arithmetic is exact and range checked. Ordinary runtime
integer arithmetic follows the same representability rules.

For both signed and unsigned integer types, the following ordinary operations
require their mathematical result to fit the operation type:

```
left + right;
left - right;
left * right;

left += right;
left -= right;
left *= right;

value++;
value--;
```

A statically known unrepresentable result is a compile-time error. If the
result depends on runtime values and is not representable, execution traps.

Unsigned arithmetic does not wrap implicitly. Unsigned underflow and overflow
use the same checked rule as signed overflow.

Typed unsigned integers do not support unary negation:

```
value: u32 = 1;

-value; // invalid
```

Signed unary negation is valid only when its result is representable. Negating
the minimum value of a signed integer type is therefore a compile-time error
when known and a runtime trap otherwise.

Binary subtraction on unsigned values remains valid:

```
difference: u32 = left - right;
difference -= right;
```

These operations trap when right is greater than left; they do not wrap to
a large unsigned result.

A runtime trap means that the operation produces no value and normal execution
does not continue. The eventual trap mechanism, panic handler, diagnostic
format, and runtime ABI remain execution-layer decisions.

The remainder operator % is integer-only.

Integer division truncates toward zero. Integer remainder has the sign of the
left operand.

The following integer division and remainder failures trap:

division by zero;
remainder by zero;
the minimum value of a signed integer type divided by -1;
the minimum value of a signed integer type remaindered by -1.

The same rules apply to `/=` and `%=`.

A statically known failure is rejected during semantic analysis:

```
value / 0;   // invalid
value % 0;   // invalid
value /= 0;  // invalid
value %= 0;  // invalid
```

Known failures are diagnosed when their operands are produced by literals,
named constants, casts, or other compile-time constant expressions. When the
operands depend on runtime values, the expression remains well typed and a
future execution layer must perform the required checks.

Equality is currently defined for:

- numeric values with compatible types
- `bool`
- values of the same enum declaration
- raw pointers with exactly equal immediate pointee types, ignoring immediate readonly/volatile qualifier differences
- a mutable or readonly pointer and `null`

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
wide_count: s64 = 3;

value << small_count; // u32
value >> wide_count;  // u32
```

Every shift count must satisfy:

```
0 <= count < left_operand_bit_width
```

A statically known invalid count is a compile-time error. A runtime-dependent
negative count or a count equal to or greater than the width traps.

Shift counts are never masked modulo the operand width.

An untyped left operand uses its ordinary default integer width. Therefore,
1 << count uses s32; an explicitly wider operation starts with a cast such
as cast(u64, 1) << count.

Shift result semantics

Left shift is a fixed-width bit-pattern operation. Zero bits enter from the
right and bits shifted beyond the width are discarded:

```
cast(u8, 128) << 1; // u8 value 0
cast(s8, 64)  << 1;  // s8 value -128
```
Discarding high bits during a valid left shift does not cause an arithmetic
overflow trap. This rule is specific to shifts; ordinary addition,
subtraction, multiplication, increment, and decrement remain checked
arithmetic operations.

Unsigned right shift fills with zero. Signed right shift is arithmetic and
sign-extending:

```c
cast(u8, 128) >> 1; // 64
cast(s8, -3) >> 1;  // -2
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

Floating-point semantics

Coglet defines:

f32 as IEEE-754 binary32;
f64 as IEEE-754 binary64;
round-to-nearest, ties-to-even as the normal rounding mode.

Compile-time evaluation and future runtime execution must follow the same
value-level rules.

Floating-point division by zero is not an integer-style semantic error:

```
1.0 / 0.0;   // positive infinity
-1.0 / 0.0;  // negative infinity
0.0 / 0.0;   // NaN
```
Signed zero is preserved:
```
0.0 == -0.0; // true
1.0 / -0.0;  // negative infinity
```
NaN is unordered:
```
NAN_VALUE :: 0.0 / 0.0;

NAN_VALUE == NAN_VALUE; // false
NAN_VALUE != NAN_VALUE; // true

NAN_VALUE < 0.0;  // false
NAN_VALUE <= 0.0; // false
NAN_VALUE > 0.0;  // false
NAN_VALUE >= 0.0; // false
```
Floating-point arithmetic overflow produces infinity. Underflow follows
IEEE-754 gradual-underflow behavior, including subnormal values.

f32 operations produce f32-precision results rather than being evaluated
as f64 operations and rounded only afterward.

Compilation modes must not silently enable reassociation, flush-to-zero,
discarded signed zero, or other fast-math transformations that change
observable Coglet behavior.

Exact NaN payload behavior, signalling NaNs, floating-point exception flags,
configurable rounding modes, and fused-operation contraction remain subjects
for a later complete floating-point specification.

## Raw Object Pointers

Coglet supports mutable/readonly raw object pointers with optional volatile access as its low-level memory and C-interoperability foundation.

```c
value: s32 = 10;

pointer: s32* = &value;
view: readonly s32* = pointer;
device: volatile s32* = pointer;
status: readonly volatile s32* = pointer;
```

`T*` grants mutable ordinary access to its pointee. `readonly T*` removes write permission. `volatile T*` remains writable but requires accesses through that pointer to retain volatile semantics. `readonly volatile T*` combines both qualifiers.

All forms remain nullable, non-owning, unchecked, and potentially dangling. Readonly access is not ownership, borrowing, lifetime checking, deep
immutability, or a guarantee that the underlying object cannot change through
another alias.

The pointer variable itself remains assignable:

```c
first: readonly s32* = get_first();
second: readonly s32* = get_second();

first = second; // valid
```

The restriction applies to storage reached through the pointer:

```c
value := *first; // valid
*first = 10;     // invalid
first[0] = 10;   // invalid
```

### Access propagation

Dereferencing or indexing preserves both permission and volatility:

```text
*T*                     -> writable ordinary T lvalue
*readonly T*            -> readonly ordinary T lvalue
*volatile T*            -> writable volatile T lvalue
*readonly volatile T*   -> readonly volatile T lvalue
```

A struct field selected from an lvalue inherits the access of that lvalue:

```c
Point :: struct {
    x: s32;
}

read::(point: readonly Point*) -> s32 {
    return (*point).x;
}

write::(point: readonly Point*) -> void {
    (*point).x = 10; // invalid
}
```

Address-of accepts writable or readonly lvalues and preserves both permission and volatility:

```text
&writable ordinary T lvalue -> T*
&readonly ordinary T lvalue -> readonly T*
&writable volatile T lvalue -> volatile T*
&readonly volatile T lvalue -> readonly volatile T*
```

An address/dereference round trip therefore cannot recover mutable access:

```c
view: readonly s32* = get_view();

same_view: readonly s32* = &*view; // valid
mutable: s32* = &*view;            // invalid
```

### Pointer conversions

Coglet permits monotonic immediate pointer qualification. A matching raw pointer may add `readonly`, `volatile`, or both, but may not remove either qualifier. The immediate pointee types must otherwise be exactly equal. These conversions are valid implicitly and through `cast`:

```c
mutable: s32* = get_pointer();

implicit_view: readonly s32* = mutable;
volatile_view: volatile s32* = mutable;
both: readonly volatile s32* = mutable;
explicit_view := cast(readonly volatile s32*, mutable);
```

Qualifier removal is rejected because it could invent write permission or discard volatile access semantics:

```c
view: readonly volatile s32* = get_view();

mutable: s32* = view;            // invalid
nonvolatile: readonly s32* = view; // invalid
cast(s32*, view);                 // invalid
```

Qualifiers are not introduced recursively through nested pointers. In particular, `s32**` does not adapt to `volatile s32**` or `readonly s32**`.

### Pointer equality

Pointers with the same immediate pointee type may be compared even when their immediate readonly/volatile qualifiers differ:

```c
mutable: s32* = get_pointer();
view: readonly s32* = mutable;
device: volatile s32* = mutable;

mutable == view;   // valid
mutable == device; // valid
```

Comparison observes pointer values and does not transfer permissions. Nested
access differences remain significant because the immediate pointee types are
different.

### Null pointers

`null` is Coglet's dedicated null-pointer literal. It adapts to any raw-pointer qualifier combination:

```c
mutable: s32* = null;
view: readonly s32* = null;
device: volatile s32* = null;
status: readonly volatile s32* = null;

mutable == null;
view != null;
```

Explicit casts may provide either concrete pointer type:

```c
cast(s32*, null);
cast(readonly s32*, null);
```

Integer zero is not a null-pointer constant. Integer-to-pointer,
pointer-to-integer, and null-to-non-pointer casts are rejected.

Pointer arithmetic, array-to-pointer decay, ownership, borrowing, and lifetime
checking remain unsupported.

## Opaque Raw Pointers

Coglet provides a dedicated opaque raw-pointer family for handle-oriented APIs
and future C interoperability:

```c
handle: opaque* = null;
view: readonly opaque* = handle;
device: volatile opaque* = handle;
status: readonly volatile opaque* = handle;
```

`opaque*` is pointer-sized and address-like, but it has no Coglet pointee type.
It therefore cannot be dereferenced or indexed:

```c
*handle;    // invalid
handle[0];  // invalid
```

Readonly and volatile qualifiers are part of opaque-pointer type identity. As with typed raw pointers, qualifiers may be added monotonically but not discarded:

```c
mutable: opaque* = get_handle();
view: readonly opaque* = mutable;
device: volatile opaque* = mutable;
both: readonly volatile opaque* = mutable;
explicit_view := cast(readonly volatile opaque*, mutable);
```

`null` adapts to every opaque-pointer qualifier form, and matching opaque pointers may be compared despite immediate qualifier differences. Typed raw pointers and opaque raw
pointers are not assignment-compatible or directly comparable.

Opaque pointers compose with ordinary pointer layers. `opaque**` is a normal
mutable pointer to storage containing an `opaque*`, so one dereference is
valid:

```c
slot: opaque* = null;
out: opaque** = &slot;
*out = get_handle(); // valid

handle := *out;      // valid: result is opaque*
**out;               // invalid: opaque* is not dereferenceable
```

`readonly` and `volatile` continue to qualify only the first pointer layer. Therefore `readonly volatile opaque**` means a mutable/non-volatile outer pointer to a readonly+volatile `opaque*` value.

Crossing between typed and opaque raw pointers requires `reinterpret`; there
is no implicit conversion and ordinary `cast` does not perform this operation.

## Arrays

Arrays are fixed-size values with an element type and compile-time length.

```c
values: s32[3] = [0, 0, 0];

values[0] = 1;
values[1] += 2;
```

The type means an array of three `s32` values.

Array size is part of the type:

```c
a: s32[3];
b: s32[4];
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
values: s32[3] = [1, 2, 3];
```

The expected type supplies the element type and required length.

Invalid:

```c
values: s32[3] = [1, 2];       // too few elements
values: s32[3] = [1, 2, 3, 4]; // too many elements
values: s32[3] = [1, true, 3]; // wrong element type
```

Supported expected-type contexts include:

```c
values: s32[3] = [1, 2, 3];
values = [4, 5, 6];

takes_s32_array([1, 2, 3]);

make_values::() -> s32[3] {
    return [1, 2, 3];
}

Point :: struct {
    values: s32[3];
}

p := Point {
    values = [1, 2, 3],
};
```

Array literals are not yet inferred standalone expressions.

## Array Zero Initializer

A fixed-size array may be initialized with the special contextual spelling
`{0}`:

```c
values: s32[64] = {0};
```

This initializes the entire destination array to Coglet semantic zero. The
spelling is intentionally one indivisible initializer: it is not equivalent to
a one-element array literal, does not mean "initialize the first element and
fill the rest", and does not introduce general C aggregate-initializer rules.

The same expected-array contexts as array literals are supported:

```c
values = {0};
takes_s32_array({0});

make_values::() -> s32[3] {
    return {0};
}

p := Point {
    values = {0},
};
```

`{0}` requires an expected fixed-array type. It cannot infer storage by itself
and cannot initialize a scalar or struct directly:

```c
values := {0};      // invalid: no expected array type
value: s32 = {0};   // invalid: destination is not an array
```

The frontend records the destination array type on the contextual initializer;
CogIR then represents the value with its existing typed `zeroinit` constant.
Backends therefore receive a backend-neutral semantic zero value rather than
reconstructing source syntax.

Rejected:

```c
values := [1, 2, 3];
[1, 2, 3];
```

## Slices

A slice is a non-owning view over contiguous storage. `T[]` grants mutable element access and `readonly T[]` grants readonly element access. The first representation is exactly two machine-independent Coglet values: a typed data pointer and a `u64` element count. There is no capacity, allocator, or ownership field.

```c
values: s32[3] = [1, 2, 3];
view: s32[] = values;
readonly_view: readonly s32[] = view;

view[1] = 20;
count := readonly_view.len;
first_ptr := readonly_view.data;
```

`.len` is the number of elements and `.data` is a pointer carrying the same mutable/readonly access as the slice. Slice metadata fields are observational rvalues; code may reassign a whole slice variable, but cannot mutate only its `.data` or `.len` field. Indexing a mutable slice produces a writable lvalue; indexing a readonly slice produces a readonly lvalue.

A fixed array may adapt to a matching slice only when the array expression denotes addressable storage. Mutable storage may become either a mutable or readonly slice. Readonly storage may only become a readonly slice. A mutable slice may weaken to a matching readonly slice; readonly access cannot be recovered as mutable.

```c
values: s32[3] = [1, 2, 3];
mutable: s32[] = values;
readonly_view: readonly s32[] = mutable;
```

Array temporaries do not adapt to slices because the resulting view would immediately refer to temporary storage:

```c
make_values::() -> s32[3] {
    return [1, 2, 3];
}

view: readonly s32[] = make_values(); // invalid
```

Slices are deliberately non-owning and Coglet does not yet perform borrow/lifetime analysis. The addressability rule prevents the most immediate temporary-array error, but it does not prove that a slice cannot escape a local array's lifetime. Slice indexing also has no runtime bounds check in this first version. These limitations are tracked in `docs/known_shortcomings.md`.

Slices are ordinary Coglet values and may be function arguments/returns, generic parameter shapes, locals, and fields of ordinary Coglet structs. Direct by-value slice parameters/returns are not part of the current `#extern(c)` ABI subset; FFI boundaries should pass pointer and length explicitly.

## String Literals

String literals represent immutable compile-time byte data and are now ordinary readonly byte-slice expressions.

```c
text := "hello";
// text: readonly u8[]
// text.len == 5
```

The visible slice contains the decoded bytes only. Compiler-owned static backing storage also includes one trailing NUL byte for compatibility with the existing direct C-string boundary, but that terminator is not counted by `.len`. An embedded `\0` is an ordinary visible byte and does not shorten the slice.

A string literal may also contextually initialize a fixed-size byte array. In that context the destination receives the decoded bytes plus the trailing NUL, so the fixed array must be one byte larger than the visible string length:

```c
name: u8[6] = "hello";
view: readonly u8[] = "hello";
```

The explicit C FFI convenience remains intentionally narrow:

```c
#extern(c)
puts::(s: readonly c_char*) -> c_int;

puts("hello");
```

Only a direct literal argument to a `#extern(c)` parameter spelled exactly `readonly c_char*` receives that NUL-terminated pointer conversion. General slices and arrays do not decay to C pointers, and direct slice-by-value C ABI parameters are rejected.

```c
normal::(s: readonly c_char*) -> void { }

#extern(c)
bytes::(s: readonly u8*) -> void;

normal("hello"); // invalid: ordinary Coglet call does not use the C-string conversion
bytes("hello");  // invalid: parameter is not readonly c_char*

array: u8[6] = "hello";
puts(array);      // invalid: arrays do not decay to pointers
```

Supported escape sequences currently include `\n`, `\t`, `\r`, `\\`, `\"`, and `\0`. Invalid escapes are rejected during semantic analysis.

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
    x: s32;
    y: s32;
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
    value: s32;
}

Second :: struct {
    value: s32;
}
```

`First` and `Second` are different types even though their fields match.

The same rule applies to shadowed declarations:

```c
Point :: struct {
    x: s32;
}

test::() -> void {
    outer: Point = Point { x = 1 };

    Point :: struct {
        x: s32;
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

For C interoperability, an enum may opt into an explicit ABI representation:

```c
#repr(c)
CMode::enum(c_int) {
    Idle = -1,
    Running = 3,
}
```

`#repr(c)` enums must spell an explicit native C integer backing alias (`c_char`, the signed/unsigned C integer families, or `c_size`). The representation annotation does not change closed-enum validity, nominal typing, or switch exhaustiveness. The host-C backend lowers the enum ABI type to the exact selected C integer spelling rather than a native C `enum`, because C99 cannot portably force a fixed enum underlying representation.

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


## Casts and Explicit Integer Conversion

### Checked casts

Checked casts use:

```c
cast(TargetType, expression)
```

Supported categories include:

- identical types;
- selected numeric conversions;
- enum-to-integer conversion;
- compile-time integer-to-enum conversion to a declared member;
- `null` to a concrete mutable or readonly typed or opaque raw-pointer type;
- mutable typed or opaque raw pointer to the corresponding readonly raw-pointer type;
- Boolean to Boolean.

A mutable pointer may explicitly drop write permission when the immediate
pointee type is otherwise exactly equal:

```c
view := cast(readonly s32*, mutable_pointer);
```

A checked pointer cast may not add write permission or recursively qualify a
nested pointer.

Integer-to-pointer, pointer-to-integer, and null-to-non-pointer casts are not
supported.

`cast` is value-preserving and checked. A statically known invalid conversion
is a compile-time error; a runtime-dependent invalid conversion remains well
typed and must trap in a future execution layer.

Integer-to-integer conversion requires the mathematical source value to fit
the destination type. It never implicitly discards high bits, reinterprets a
bit pattern, or reduces the value modulo the destination width.

Floating-point-to-integer conversion:

- rejects NaN and positive or negative infinity;
- truncates a finite value toward zero;
- requires the truncated integer to fit the destination type.

Integer-to-floating-point conversion rounds to the nearest representable value
of the destination format, with ties to even. Precision loss is permitted
because the conversion is explicit.

`f32`-to-`f64` conversion is exact. Finite `f64`-to-`f32` conversion rounds to
the nearest `f32` value, with ties to even, but rejects a finite source outside
the finite `f32` range. NaN, infinity, and signed zero are preserved.

For closed enums, fitting the backing type is necessary but not sufficient:

```c
Small :: enum(u8) { A, B }

cast(Small, 1);   // valid: Small.B
cast(Small, 255); // invalid: no declared member has value 255
```

Runtime integer-to-enum casts remain rejected until Coglet has a checked
runtime enum-conversion facility.


### Raw-pointer reinterpretation

Unchecked raw-pointer representation conversion uses:

```c
reinterpret(TargetPointerType, expression)
```

`reinterpret` is deliberately narrower than a general pointer cast. Exactly
one side must be a top-level opaque raw pointer and the other a typed raw
pointer:

```c
p: s32* = get_pointer();
h: opaque* = reinterpret(opaque*, p);
recovered: s32* = reinterpret(s32*, h);
```

The operation preserves the address bits and changes only their static pointer
interpretation. Coglet cannot prove that an opaque address actually denotes an
object of the recovered pointee type, so this operation is explicitly
unchecked.

`reinterpret` never grants stronger access or weaker volatility guarantees. A readonly source may only produce a readonly target, and a volatile source may only produce a volatile target:

```c
rp: readonly s32* = get_view();
rh: readonly opaque* = reinterpret(readonly opaque*, rp); // valid

reinterpret(opaque*, rp); // invalid: would discard readonly access

vp: volatile s32* = get_device();
reinterpret(opaque*, vp);          // invalid: would discard volatile access
reinterpret(volatile opaque*, vp); // valid
```

Mutable/non-volatile sources may add readonly and/or volatile qualifiers while being reinterpreted. General
typed-pointer-to-typed-pointer conversions and opaque-pointer-to-opaque-pointer
conversions are rejected; safe access-only changes continue to use ordinary
assignment or `cast`.

`reinterpret` is not a compile-time constant conversion.

### Truncating integer conversion

Explicit truncating conversion uses:

```c
truncate(TargetIntegerType, expression)
```

The target must be a concrete integer type and the source must be an integer.
The result retains the low `N` bits of the source mathematical value, where
`N` is the destination width, and interprets that bit pattern using the target
signedness.

```c
truncate(u8, 256);            // 0
truncate(u8, -1);             // 255
truncate(s8, 255);            // -1
truncate(s8, 128);            // -128
truncate(u16, cast(s8, -1));  // 65535
```

Truncation never fails because the mathematical source value is outside the
destination range. Floating-point, Boolean, pointer, and enum truncation are
not supported.

When its operand is compile-time constant, `truncate` is also a compile-time
constant expression and is evaluated with explicit fixed-width bit-pattern
semantics.

### Explicit wrapping arithmetic

Coglet provides compiler builtins:

```c
wrapping_add(left, right)
wrapping_sub(left, right)
wrapping_mul(left, right)
wrapping_neg(value)
```

Operands currently require matching concrete integer types. Results are
computed modulo the type width and never fail because of arithmetic overflow.

Wrapping operations and `truncate` are explicit alternatives. They do not
change the checked semantics of ordinary arithmetic or `cast`.

## C Interoperability: External Function Declarations

The first C-interoperability surface is a declaration-only function annotation:

```c
#extern(c)
puts::(s: readonly c_char*) -> c_int;
```

This means that the function body is supplied outside Coglet and is resolved
through the platform's C ABI/linker when the current host-C backend is used. By
default the Coglet identifier is also the external symbol name. A declaration
may override that symbol without changing the name used by Coglet code:

```c
#extern(c, name="SDL_CreateWindow")
create_window::(title: readonly c_char*) -> opaque*;

#extern(c, call=win64)
platform_probe::(value: c_int) -> c_int;
```

Calls in Coglet still refer to `create_window`; host-C lowering emits a reference
to `SDL_CreateWindow`. The `name` value is a decoded, non-empty Coglet string and
may not contain a NUL byte. `call=<convention>` may independently select `cdecl`,
`stdcall`, `sysv64`, or `win64`; absence of the option uses the platform/default
C ABI.

External C declarations:

- are valid only at top level;
- have a signature but no Coglet body;
- use the Coglet identifier as the external symbol unless `name="..."` overrides it;
- end with `;`;
- may be called through the ordinary Coglet function-call rules;
- may not use parameter default values;
- accept scalar/raw-pointer ABI candidates and explicitly `#repr(c)` structs;
- do not introduce C's implicit `void*` conversions into Coglet.

The currently accepted frontend type subset is:

```text
bool
s8 s16 s32 s64
u8 u16 u32 u64
f32 f64
c_char c_schar c_uchar
c_short c_ushort c_int c_uint
c_long c_ulong c_longlong c_ulonglong
c_size
c_bool c_float c_double
T* and readonly T* when T is recursively in this subset
opaque* and readonly opaque*
additional raw-pointer layers such as opaque**
#repr(c) structs and unions by value or through raw pointers
#repr(c) enums
cfn(...) -> T native C function pointers with recursively supported signatures
void as a return type only
```

Arrays, ordinary Coglet structs, ordinary Coglet enums, and ordinary Coglet
function types remain rejected in `#extern(c)` signatures. Structs, unions, and
enums must opt into the C ABI contract explicitly; callbacks use the explicit
`cfn` function-pointer type described below:

```c
#repr(c)
CPoint::struct {
    x: c_int;
    y: c_double;
}

#extern(c)
consume_point::(point: CPoint) -> c_int;

#repr(c)
CMode::enum(c_int) {
    Idle = -1,
    Running = 3,
}

#extern(c)
consume_mode::(mode: CMode) -> c_int;

#repr(c)
CValue::union {
    integer: c_int;
    real: c_double;
}

#extern(c)
consume_value::(value: CValue) -> c_int;
```

`#repr(c)` is top-level only. C-compatible scalar/raw-pointer fields are
accepted, including pointers to represented aggregates, complete `#repr(c)`
structs/unions embedded directly by value, and fixed-size arrays whose element
type is itself a supported C field type. Array lengths must be greater than zero;
unsized and zero-length arrays are rejected. By-value aggregate dependencies may
be declared in any source order, including dependencies reached through an array
element; their inline layout graph must be acyclic. Raw-pointer cycles remain
valid because the pointee is not laid out inline. Empty structs/unions and
ordinary Coglet structs/enums remain rejected as represented fields. Explicit
`#repr(c)` enums are accepted directly, through pointers, and inside fixed
arrays. Arrays remain rejected as `#extern(c)` parameters and returns because C
function-parameter array syntax decays to pointers rather than representing a
by-value array ABI.

Represented structs and unions may request native layout controls:

```c
#repr(c, packed)
WireHeader::struct {
    tag: c_uchar;
    value: c_uint;
}

#repr(c, align=16)
AlignedValue::struct {
    value: c_int;
}

#repr(c, packed, align=8)
PackedAligned::struct {
    tag: c_uchar;
    value: c_uint;
}
```

`packed` reduces member alignment using the native C toolchain's packed-layout
mechanism. `align=N` requests a **minimum** aggregate alignment; it does not
promise to reduce a larger natural alignment. `N` must be a positive power of
two. Combining the options gives packed member placement while retaining the
requested aggregate alignment. These controls are rejected on incomplete C
structs because there is no Coglet-owned layout to modify. The current host-C
backend implements them with GNU-compatible `packed`/`aligned` attributes and
emits a compile-time error on a host compiler that does not provide those
attributes; broader toolchain-specific lowering is deferred.

A native C union uses explicit represented syntax:

```c
#repr(c)
CValue::union {
    integer: c_int;
    real: c_double;
    pointer: opaque*;
}
```

The union has native C size/alignment and may cross `#extern(c)` by value, appear
behind raw pointers, or be embedded in another `#repr(c)` struct/union (including
fixed arrays). Coglet intentionally does not yet expose direct union member
construction or member access. Those operations need an explicit active-member
policy rather than inheriting C's unchecked type-punning behavior accidentally.
For now unions are ABI carrier types: values may be received from C, transported
through Coglet, and passed back to C unchanged.

An incomplete native C struct is declared without a body:

```c
#repr(c)
SDL_Window::struct;

#extern(c, name="SDL_DestroyWindow")
destroy_window::(window: SDL_Window*) -> void;
```

This form models C APIs that publish a named `struct T` but keep its layout
private. The type is nominal and may be used through mutable/readonly raw
pointers (including nested pointer layers and `cfn` signatures). Coglet cannot
store an incomplete struct by value, embed it inline, construct it, dereference
or index a pointer to it, access fields, pass it by value, or return it by value.
The host-C backend emits only a forward declaration for such a type.

### C function pointers and callbacks

Native C callback pointers use the explicit structural type syntax:

```c
cfn(c_int, opaque*) -> c_int
```

For example:

```c
#extern(c)
run_callback::(callback: cfn(c_int) -> c_int, value: c_int) -> c_int;

#repr(c)
identity::(value: c_int) -> c_int {
    return value;
}

main::() -> s32 {
    return run_callback(identity, 7);
}
```

`#repr(c)` on a Coglet-defined function is a calling-convention/ABI contract:
its signature must use the current C-ABI-compatible type subset, it must be
top-level, and its parameters may not have defaults. `#extern(c)` function
symbols already have the C ABI and may also be used where a matching `cfn` is
expected. Ordinary Coglet functions retain the Coglet function ABI and do not
implicitly adapt to a `cfn`, even when their parameter and return types match.
This prevents an accidental callback boundary from becoming ABI-compatible only
because of the current bootstrap backend.

`cfn` values are first-class callable values. They may be stored in variables,
used as supported `#repr(c)` struct fields, passed to or returned from extern C
functions, compared with a matching `cfn` or with `null`, and initialized from
`null`. Integer zero is not a null callback pointer. Callback signatures are
validated recursively against the same C ABI subset used by `#extern(c)`.

The current host-C backend emits C function-pointer typedefs and can pass a
`#repr(c)` Coglet function to native C. The executable callback regression test
uses a separate C object that invokes the pointer and returns the Coglet
callback's result. Callback closures/capture are not introduced: `#repr(c)`
functions are top-level and therefore have no enclosing local state to capture.

### C calling conventions

Native C function declarations, callback definitions, and callback pointer types
may carry an explicit calling convention:

```c
#extern(c, call=win64)
native_probe::(callback: cfn(call=win64, c_int) -> c_int, value: c_int) -> c_int;

#repr(c, call=win64)
callback::(value: c_int) -> c_int {
    return value;
}
```

The supported source names are `cdecl`, `stdcall`, `sysv64`, and `win64`. No
`call=` option means the platform/default C ABI. Calling convention is part of a
`cfn` type's identity, so callbacks with different conventions do not implicitly
convert even when their parameter and return types are otherwise identical.
`stdcall` is rejected for variadic declarations/types because the Win32 stdcall
stack-cleanup model is not compatible with C variadics.

The current host-C backend maps explicit conventions through generated C
attributes/macros. `cdecl` uses the ordinary C ABI (with an explicit attribute on
32-bit x86 where available); `stdcall` requires GNU/Clang-compatible 32-bit x86
(or the unified Win64 ABI); `sysv64` and `win64` require GNU/Clang-compatible
x86-64 `sysv_abi` / `ms_abi` support. If a requested convention cannot be
represented by the host C compiler/architecture, generated C fails explicitly
instead of silently using another convention. Cross-target ABI selection remains
a separate future milestone.

### Volatile raw-pointer access

Coglet models C `volatile` as an immediate raw-pointer pointee qualifier, independent from `readonly`:

```c
read_write: volatile c_uint*;
read_only: readonly volatile c_uint*;
raw: volatile opaque*;
```

`volatile T*` remains writable; it means loads/stores through that pointer are volatile accesses. `readonly volatile T*` combines read-only permission with volatile access semantics. The qualifiers may be written in either order, but diagnostics and AST output canonicalize them as `readonly volatile`. Like `readonly`, `volatile` applies only to the first pointer layer, so `volatile T**` is a mutable/non-volatile outer pointer to a volatile `T*`.

Pointer qualification is monotonic. A matching pointer may implicitly add `readonly`, `volatile`, or both, but neither qualifier may be discarded implicitly. `cast` follows the same safe qualification rule, and `reinterpret` refuses to discard either readonly or volatile access while crossing between typed and opaque raw pointers. Pointer equality may ignore immediate readonly/volatile differences because comparison does not access the pointee.

Volatility is also carried on semantic lvalue metadata. Dereference, pointer indexing, and field selection through a volatile lvalue preserve that property, and address-of reconstructs a volatile-qualified pointer. This prevents operations such as `&*p` from silently stripping volatility before a future native backend/optimizer sees the access.

The host-C backend lowers represented pointer types to `volatile T *`, `const volatile T *`, `volatile void *`, and corresponding nested forms. Volatile-qualified raw pointers are valid anywhere the current C ABI subset accepts the corresponding unqualified raw pointer, including extern signatures, `cfn` types, and `#repr(c)` aggregate fields. Direct string-literal binding remains intentionally restricted to exactly `readonly c_char*`; it does not implicitly produce a volatile string pointer.

### C variadic calls

C variadics are an FFI-only feature. A declaration may place `...` after one or
more fixed parameters:

```c
#extern(c)
printf::(format: readonly c_char*, ...) -> c_int;
```

The fixed arguments are checked exactly like an ordinary `#extern(c)` call.
Arguments after `...` are restricted to values whose C variadic ABI behavior is
currently defined: scalar integer/Boolean/floating values, supported raw
pointers, explicit `#repr(c)` enums, native C function pointers, and direct
string literals. Aggregate struct/array values, ordinary Coglet function values,
and bare `null` are rejected in the variadic tail.

The host-C backend emits portable identifier-safe external C symbols without GNU
symbol-label extensions; arbitrary non-identifier linker names remain a GNU/Clang
host-C extension. The host-C backend intentionally delegates the standard C default argument
promotions to the native C compiler. Thus Coglet `bool` and narrow integer
values are promoted according to the host C integer-promotion rules, and `f32`
is passed as C `double`. Untyped floating literals are already emitted as C
`double` literals. Untyped integer literals are conservatively accepted only
when their value fits native `c_int`; wider literal typing remains explicit
future work rather than guessing a C suffix/type at the variadic boundary.

Variadic native C function-pointer types use the corresponding syntax:

```c
callback: cfn(c_int, ...) -> c_int;
```

Variadic and non-variadic `cfn` types are distinct. Native Coglet variadics are
not introduced by this feature; Coglet-defined functions, including
`#repr(c)` callback definitions, remain non-variadic for now.

Opaque pointers provide the C `void*`-style representation boundary:

```c
#extern(c)
consume_handle::(handle: opaque*) -> void;
```

In the host-C backend, `opaque*` uses a `void*`-compatible object-pointer
representation, while Coglet still requires explicit `reinterpret()` when
crossing between typed and opaque raw pointers. Readonly access remains
monotonic across that conversion.

Coglet provides transparent aliases for the native C scalar ABI family:

```text
c_char      -> native C char representation and signedness
c_schar     -> signed char
c_uchar     -> unsigned char
c_short     -> short
c_ushort    -> unsigned short
c_int       -> int
c_uint      -> unsigned int
c_long      -> long
c_ulong     -> unsigned long
c_longlong  -> long long
c_ulonglong -> unsigned long long
c_size      -> size_t
c_bool      -> _Bool
c_float     -> float
c_double    -> double
```

These names are semantic builtin type aliases rather than lexer keywords. The
integer-family aliases resolve to matching canonical Coglet fixed-width integer
types; `c_bool` resolves to `bool`; `c_float` and `c_double` resolve to `f32` and
`f64` when the native C formats match Coglet's IEEE-754 binary32/binary64
contracts. Ordinary arithmetic, literal contextualization, casts, and pointer
composition therefore continue to use the existing Coglet semantics. Plain
`c_char` remains distinct from `c_schar`/`c_uchar` because native C decides
whether plain `char` is signed. For example, on a conventional LP64 host
`c_int` resolves to `s32`, `c_long` resolves to `s64`, and `c_size` resolves to
`u64`.

The frontend now receives an explicit `TargetInfo` describing the selected C
ABI rather than consulting the compiler host directly during semantic analysis.
The normal compiler driver constructs a host target by default, while
target-aware compilation APIs may supply a different description. `TargetInfo`
currently carries pointer width, C integer-family widths, plain-`char`
signedness, `_Bool` width, and C floating formats. CLI target-triple selection
and non-host native code generation remain future work; the existing host-C
backend rejects a semantic target that does not match the build host. C `long
double` does not currently have a Coglet alias because the language has no
scalar type whose representation and precision can model it portably.

This makes common declarations portable across native targets:

```c
#extern(c)
puts::(s: readonly c_char*) -> c_int;

#extern(c)
malloc::(size: c_size) -> opaque*;
```

Fixed-width Coglet types remain valid in C declarations when the corresponding
C interface itself uses a representation-compatible fixed-width type.

The host-C backend consumes verified CogIR and adapts Coglet's executable entry
to the host process ABI. A source executable uses `main::() -> s32`; `c_*` types
remain explicit C-interoperability types rather than a requirement imposed by
the bootstrap backend. The generated C translation unit provides an
`int main(void)` adapter that runs module initialization and returns the Coglet
entry result through the C process interface. The LLVM backend maps the same
resolved CogIR entry to its native process ABI and can emit/link a native
executable; future native backends must preserve the same language-level contract.

Additional platform-specific conventions beyond the current `cdecl`/`stdcall`/
`sysv64`/`win64` set, callback lifetime policies beyond raw function pointers,
and cross-target lowering are still deferred.

## Current Semantic Architecture

Semantic analysis stores declaration and expression facts separately from the
AST. Each successfully resolved source declaration receives a `SemDeclInfo`
entry containing a stable per-compilation declaration ID, its resolved semantic
type, and its lexical symbol when one exists. This includes aggregate members
and parameters on body-less external declarations, which intentionally may not
have lexical symbols.

Each successful expression records:

- its resolved type;
- its resolved symbol, when applicable;
- a `ValueCategory`;
- a `ValueAccess`.

`ValueCategory` distinguishes no-value expressions, rvalues, and lvalues.
`ValueAccess` independently distinguishes no storage access, readonly storage,
and writable storage.

The valid combinations are:

```text
NONE    + NONE
RVALUE  + NONE
LVALUE  + READONLY
LVALUE  + WRITABLE
```

This allows a readonly dereference to remain an lvalue without being
assignable.

The semantic-info verifier walks successful programs in source order and
checks:

- one declaration entry for every successful source declaration;
- unique stable declaration IDs with node/ID reverse lookup;
- declaration/Symbol/type consistency;
- one semantic entry for every successful expression or mutation node;
- no duplicate or orphan side-table entries;
- valid value-category and storage-access combinations;
- pointer dereference and indexing access propagation;
- field-access inheritance;
- address-of access preservation;
- symbol/type consistency;
- concrete types for variables and parameters;
- distinct handling of mutation nodes and void-returning calls.

The verifier can print a deterministic source-order dump for debugging.
Semantic tables from failed programs may be partial and are dumped only when
explicitly requested.

## Future Direction

The host-C bootstrap backend and an LLVM native executable path now consume the
same frozen CogIR contract. Remaining runtime/language work must preserve the
frontend-defined scalar semantics, explicit wrapping/truncating operations,
mutable/readonly typed raw pointers, and opaque raw pointers across backends.

Near-term candidate areas include:

- explicit target/C-ABI selection for cross compilation;
- C-compatible aggregate and enum layout;
- mutable and readonly slices and byte views;
- ownership and lifetime rules only when justified by concrete use cases;
- a later first-class string type;
- package manifests and separate-compilation policy on top of the configured stdlib module root;
- standard library facilities;
- broader generics (generic nominal types or user-defined constraint/trait systems) only when justified by real use cases;
- self-hosting.

At a future C ABI boundary, opaque pointers should map to C `void*`-style
representations without importing C's implicit conversion rules into Coglet.
