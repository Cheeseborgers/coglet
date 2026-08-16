# CogIR design

CogIR is Coglet's compiler-owned, backend-neutral intermediate representation.
It sits after successful semantic analysis and before host-C, LLVM, or future
native backends.

```text
source files
    -> lexer/parser AST
    -> semantic analysis
    -> CogIR lowering
    -> CogIR verification
    -> optional CogIR passes
    -> host-C / LLVM / native backend
```

CogIR is a typed control-flow-graph MIR. It is intentionally higher-level than
LLVM IR and lower-level than the AST. It preserves Coglet semantics directly,
including checked arithmetic, explicit wrapping arithmetic, raw-pointer access
qualifiers, volatile memory operations, nominal aggregates/enums, C ABI
contracts, and source provenance.

## Boundary and ownership

CogIR lowering is the final phase allowed to inspect frontend syntax or semantic
objects. After successful lowering, a backend must not require:

- `Node *`;
- `Symbol *`;
- frontend `Type *`;
- `SemanticContext *`;
- unresolved names;
- `TYPE_NAMED`, `TYPE_UNTYPED_INT`, `TYPE_UNTYPED_FLOAT`, or `TYPE_NULL`.

A `CogIrModule` owns all strings, types, constants, declarations, function CFGs,
ABI metadata, and source-file text needed by later diagnostics. The intended
lifetime test is:

```text
CompileResult
    -> cog_ir_lower()
    -> cog_ir_verify()
    -> destroy CompileResult
    -> run backend using CogIrModule only
```

The module copies `TargetInfo`. Backend-specific target structures such as an
LLVM target triple/data layout, object format, register file, relocation model,
or instruction-selection tables are not part of CogIR.

## Core invariants

A verified CogIR module guarantees:

1. every runtime value has a concrete CogIR type;
2. all names and declarations have already been resolved to stable IR IDs;
3. constants fit their concrete runtime types;
4. CFG edges target valid blocks and satisfy block-parameter types;
5. every basic block has exactly one terminator;
6. every instruction operand has a valid definition and type;
7. mutable storage is explicit through globals, local slots, addresses,
   loads, and stores;
8. volatile behavior is explicit on memory operations;
9. checked operations retain Coglet trap semantics and are not translated to
   host-language/LLVM undefined behavior;
10. C ABI intent is normalized IR metadata, never reconstructed from source
    annotations;
11. source spans can be resolved without the frontend remaining alive.

CogIR does not perform name lookup, overload resolution, type inference,
contextual-conversion selection, constant-expression legality checking, or
control-flow legality checking. Those are frontend responsibilities.

## Compilation unit and module initialization

A `CogIrModule` represents one Coglet compilation unit, not necessarily exactly
one physical source file. Its source table is multi-file-capable from the
beginning.

Coglet currently permits runtime-bearing program-scope code, including global
initializers that call functions, top-level expression/mutation statements, and
control-flow statements. CogIR therefore gives program-scope execution an
explicit representation rather than inheriting startup behavior from C.

### Storage phase

All source global variables receive module-lifetime storage before module
initialization begins. This includes declarations inside top-level lexical
blocks: semantic analysis classifies them as global storage even when their
names later leave lexical scope, so their addresses must not become temporary
stack addresses.

Source global storage begins with a zero representation. Consequently, if
runtime code reaches a global indirectly before that declaration's explicit
initializer has executed, CogIR v1 observes the zero representation rather than
backend-dependent garbage. This startup rule is deliberately deterministic
across backends; a later frontend rule may choose to reject such pre-initializer
reads without changing the ordered module-init model.

Source-level explicit initializers are not folded into this initial data in
CogIR v1; they execute at their source position in the module initializer. This
preserves observable source ordering even when an earlier initializer calls a
function that can access storage for a later-declared global.

Compiler-generated immutable data, such as backing storage for a C string
literal, may use a non-zero static constant initializer because it has no
source-level initialization point or side effects.

A later optimization may move a source initializer into static data only when
it proves the transformation observationally equivalent to the ordered module
initializer.

### Module initializer

A module has an optional distinguished internal function:

```text
module.init_function : () -> void
```

It is compiler-generated, is not source-callable, and uses the Coglet ABI.
Lowering walks runtime-bearing top-level syntax in source order and emits its
runtime effects into this CFG:

- a global variable declaration with an initializer evaluates the initializer
  and stores the result at that point;
- a global declaration without an initializer emits no action;
- top-level expression, mutation, block, `if`, `switch`, `while`, and `for`
  statements lower normally into the initializer CFG;
- type, enum, constant, and function declarations emit no runtime action;
- nested function/type declarations are flattened into module tables but do not
  execute merely because their declaration is encountered.

The initializer ends with an implicit `ret void` if control reaches its end.
Source `return` at program scope remains a semantic error.

For an executable, the runtime/backend contract is that module initialization
runs exactly once before the Coglet program entry point. A trap during module
initialization prevents entry execution. Future multi-module/import work will
specify dependency ordering between separate module initializers; CogIR v1 only
requires deterministic ordering inside one compilation unit.

The host-C, LLVM, and future native backends must all honor this same initializer
CFG. Startup integration is a backend/link-driver concern; initialization
semantics are not.

## Stable IDs

IR identity uses compact integer IDs rather than C pointer identity.

A concrete implementation may use `uint32_t` with `UINT32_MAX` as the invalid
value:

```c
typedef uint32_t CogIrTypeId;
typedef uint32_t CogIrAbiTypeId;
typedef uint32_t CogIrConstId;
typedef uint32_t CogIrGlobalId;
typedef uint32_t CogIrFunctionId;

typedef uint32_t CogIrBlockId;   /* function-local */
typedef uint32_t CogIrValueId;   /* function-local */
typedef uint32_t CogIrSlotId;    /* function-local */
```

`SemDeclId` is an input mapping key during lowering, not persistent backend
identity. The lowerer maintains maps such as:

```text
SemDeclId -> CogIrTypeId
SemDeclId -> CogIrGlobalId
SemDeclId -> CogIrFunctionId
SemDeclId -> function-local CogIrSlotId
```

Backends only receive the resulting CogIR IDs.

## Source provenance

CogIR keeps `SourceSpan` on declarations and instructions/terminators where a
source operation exists. Compiler-generated entities may use an invalid span or
an explicit synthetic marker.

Because frontend `SourceManager` source buffers are borrowed, `CogIrModule`
must copy the registered filenames and source text required for diagnostics.
The existing `SourceFileId` values may be preserved during this copy so
`SourceSpan` itself can remain unchanged.

This permits IR lowering/verifier/backend diagnostics after `CompileResult` has
been destroyed and gives later debug-info generation a direct path back to
Coglet source.

## CogIR type system

CogIR contains only runtime-representable types:

```text
void
bool

i8  i16  i32  i64
u8  u16  u32  u64

f32 f64

ptr<T, access, volatile>
opaque_ptr<access, volatile>
array<N, T>

struct <nominal-id>
union  <nominal-id>
enum   <nominal-id, backing-type>

fn(params...) -> result [abi, calling-convention, variadic]
```

The following frontend-only kinds are forbidden:

```text
named
untyped-int
untyped-float
null
```

An adaptable literal is materialized according to `SemExprInfo` before it
becomes an IR constant. `null` becomes a null constant whose type is the exact
concrete pointer/function type selected by semantic analysis.

### Pointer types

Typed and opaque raw pointers remain distinct. Immediate readonly and volatile
qualifiers remain part of the CogIR pointer type because they are part of
Coglet type identity and C ABI contracts.

Actual volatile memory behavior is also explicit on `load`/`store`; it must not
be inferred only from a backend's pointer type representation.

### Arrays

Only concrete fixed-size arrays reach CogIR. Unsized/frontend placeholder array
forms are not runtime IR types.

Array values do not implicitly decay to pointers. Array-element addressing and
raw-pointer indexing remain distinct IR operations.

### Nominal aggregates

Structs, unions, and enums retain nominal identity through their CogIR type ID.
Two declarations with identical layout are still different CogIR types.

Struct/union field metadata stores field type IDs in source order. Incomplete
`#repr(c)` structs have a nominal type but no body and remain usable only where
semantic analysis already permitted them.

CogIR does not initially bake target byte offsets into ordinary aggregate types.
Field-address operations identify the nominal type and field index; target
layout is computed by the relevant target/layout layer. `#repr(c)` packing and
explicit alignment remain ABI metadata.

### Enums

An enum CogIR type records its concrete integer backing type and member values.
Enum values keep the nominal enum type rather than being silently replaced by
the backing integer type. Explicit enum-to-integer casts lower as conversions.

## C ABI metadata

CogIR preserves the normalized semantic ABI contract in IR-owned form. It does
not reinterpret AST annotations.

Function declarations record separately:

```text
definition vs declaration
internal vs external linkage
Coglet vs C calling ABI
C calling convention
variadic status
external linker symbol, when external
```

A C ABI type table mirrors only the source-level ABI spelling that semantic type
canonicalization intentionally erased, for example:

```text
runtime CogIR type        C ABI spelling
------------------        --------------
i32                       c_int
u64                       c_ulong (on the selected target)
readonly ptr<i8>          readonly c_char*
fn(i32)->i32              cfn(c_int)->c_int
```

This remains separate from the normal CogIR type table. It exists so the host-C
backend and target ABI lowering can honor exact C contracts without turning
CogIR itself into a C IR.

Represented aggregate metadata retains struct-vs-union, incomplete, packed, and
explicit-alignment information. Represented enum metadata retains its exact C
backing spelling.

C variadic default promotions are explicit in executable CogIR. Before a
value enters the variadic tail of a native-C call, lowering inserts
`c.vararg.promote` whenever the target C ABI changes its runtime representation:
`bool` and integer/`#repr(c)` enum values narrower than native `c_int` become
native-width signed `c_int`, and `f32` becomes `f64`/C `double`. Values already
at or above the promoted representation, supported pointers, native C function
values, and already-materialized `c_int`/`f64` literals pass through unchanged.
The verifier rejects a C-variadic call whose tail still contains a representation
that requires one of these default promotions.

## Constants

CogIR constants are typed and backend-independent. Integer constants are stored
as fixed-width bit patterns after semantic range checking. Floating constants
are stored by exact IEEE bit pattern rather than host `double` so `f32`, `f64`,
NaN, infinity, and signed zero survive lowering exactly.

Required v1 constant forms are:

```text
bool
integer bits
enum backing bits with nominal enum type
f32 bits
f64 bits
null pointer/function value
array aggregate
struct aggregate
zero representation
```

Constants may live in a module-level table and be referenced by `CogIrConstId`.
Constant declarations themselves need no runtime storage unless a future
language feature makes them addressable.

String literals have two lowering forms:

- when initializing an array value, lower the decoded bytes plus trailing NUL as
  an array constant/value and copy it into the destination;
- at the supported C string pointer boundary, create compiler-generated readonly
  static byte storage and take its element address. This is not general
  array-to-pointer decay.

## Functions, blocks, values, and slots

A function contains:

```text
signature
ABI/linkage metadata
source/debug name
parameters
local storage slots
basic blocks
```

Function parameters and instruction results are immutable `CogIrValueId`
values. Mutable source variables use explicit storage.

### Local slots

`CogIrSlotId` represents function-lifetime addressable storage. A slot has a
concrete stored type and optional source/debug metadata.

The initial lowerer should deliberately use slots even when a local could be
SSA-promoted. Parameters are function input values; lowering may create a slot
and store each parameter in the entry block so parameter mutation/address-taking
has one uniform representation.

A later mem2reg-style pass can promote eligible slots without changing source
semantics.

### Basic blocks

Each block contains:

```text
block parameters
ordered instructions
exactly one terminator
```

Block parameters are used for values created by control-flow joins instead of
LLVM-style textual phi nodes. Branch edges carry arguments matching the target
block's parameter list.

For example, short-circuit `&&` may lower as:

```text
entry:
    %lhs = ...
    cond_br %lhs, rhs, false

rhs:
    %rhs_value = ...
    br join(%rhs_value)

false:
    br join(false)

join(%result: bool):
    ...
```

V1 lowering should generally keep instruction-produced temporary values within
the block that defines them and use block arguments for explicit cross-block
value transfer. Source locals remain in slots, so most CFG joins do not require
SSA values at all.

## Addresses and memory

Lvalue syntax does not survive into CogIR as a special expression category. The
lowerer uses an internal place abstraction while translating AST nodes:

```c
typedef struct LoweredPlace {
    CogIrValueId address;
    CogIrTypeId value_type;
    int is_volatile;
    int is_writable;
} LoweredPlace;
```

This is lowering state, not a serialized IR entity.

Core address/memory operations are:

```text
local_addr      slot -> ptr<T>
global_addr     global -> ptr<T>
field_addr      ptr<Struct>, field-index -> ptr<Field>
array_elem_addr ptr<Array>, index -> ptr<Element>
ptr_index_addr  ptr<T>, index -> ptr<T>
load            ptr<T> -> T
store           ptr<T>, T -> void
```

`load` and `store` carry an explicit volatile bit. The verifier rejects stores
through readonly addresses and checks that the value type matches the pointee.

Known array-index bounds remain a semantic compile-time check. CogIR v1 does not
invent a runtime array-bounds trap where the language does not currently specify
one.

Dereference requires no dedicated runtime instruction: lowering a dereference
as a place uses the pointer value itself as the resulting address.

For aggregate rvalues, CogIR also supports value operations such as
`make_struct`, `make_array`, `extract_field`, and `extract_element`; lvalue field
and element accesses use the corresponding address operations instead.

## Operations

CogIR operations are defined by Coglet semantics, not by C operator behavior or
LLVM poison/undefined behavior.

### Checked integer arithmetic

```text
iadd.checked
isub.checked
imul.checked
idiv.checked
irem.checked
ineg.checked
```

Signedness and width come from the concrete operand type. Operations either
produce the mathematically specified result or trap according to Coglet rules.
`idiv.checked`/`irem.checked` trap on a zero divisor and handle the signed
minimum/-1 overflow case where applicable.

Increment/decrement and arithmetic compound assignment lower to load + checked
operation + store.

### Explicit wrapping integer arithmetic

```text
iadd.wrap
isub.wrap
imul.wrap
ineg.wrap
```

These implement the existing wrapping builtins modulo the operand width and do
not trap for arithmetic overflow.

### Bitwise and shifts

```text
bit.and
bit.or
bit.xor
bit.not

shl.checked_count
shr.signed.checked_count
shr.unsigned.checked_count
```

Shift operations trap when the runtime count is negative or greater than or
equal to the left operand width. Valid left shift is a fixed-width bit-pattern
operation and discards high bits without arithmetic-overflow trapping.

### Floating point

```text
fadd
fsub
fmul
fdiv
fneg
```

These preserve Coglet's specified IEEE-754 value behavior. CogIR has no fast-math
flags in v1. Backends may not reassociate or otherwise enable transformations
that change NaN, infinity, signed-zero, rounding, or precision behavior promised
by the language.

### Comparisons

Integer comparisons are explicit about signedness after lowering:

```text
icmp.eq
icmp.ne
icmp.slt / sle / sgt / sge
icmp.ult / ule / ugt / uge
```

Pointer comparison supports equality/inequality only. Boolean and enum equality
remain typed operations; enum ordered comparison is never generated because the
frontend rejects it.

Floating comparisons use Coglet predicates whose NaN behavior is part of the
operation contract:

```text
fcmp.eq
fcmp.ne
fcmp.lt
fcmp.le
fcmp.gt
fcmp.ge
```

`eq` is false for NaN, `ne` is true when unordered/not-equal, and ordered
comparisons are false for NaN.

`&&` and `||` are lowered through CFG short-circuiting rather than eager binary
instructions.

### Explicit conversions

Required v1 conversion operations include:

```text
cast.checked        source value -> concrete destination type
int.truncate        integer -> concrete integer destination
ptr.reinterpret     typed raw pointer <-> opaque raw pointer
ptr.qualify         immediate readonly/volatile addition when needed
c.vararg.promote    required non-trapping C default argument promotion
```

`cast.checked` retains trap semantics for runtime-dependent checked numeric
casts. Integer-to-enum runtime casts are not generated because the current
frontend rejects them. Compile-time enum casts become typed constants.

A backend may split `cast.checked` into more target-oriented operations later,
but CogIR v1 keeps the source semantic contract explicit.

Contextual numeric materialization normally disappears into the selected
constant/instruction type rather than generating a runtime conversion.
`c.vararg.promote` is different: it is ABI legalization, not a checked Coglet
cast, and therefore cannot trap. Its source type determines signed versus
unsigned integer extension; `#repr(c)` enums use their CogIR backing type.

## Function values and calls

A `function_ref` operation produces a typed function value referring to a
`CogIrFunctionId`. This works for direct Coglet calls, C declarations, and
callback values without relying on textual symbol lookup.

A call contains:

```text
callee value
argument values
result type (or void)
source span
```

The callee's function type carries ABI/calling-convention/variadic information.
The verifier checks fixed arguments against the concrete signature, requires
variadic function types to use the native-C ABI (and not `stdcall`), and requires
C-variadic tail values to be ABI-legal after default promotion. Lowering emits
`c.vararg.promote` before the call when the target representation changes.

Nested Coglet functions are closure-free today. They therefore lower as ordinary
module-level internal functions with unique IDs; lexical nesting affects source
visibility only and does not imply a runtime environment/capture object.

## Globals

A `CogIrGlobal` records:

```text
concrete stored type
source/debug name
internal linkage in v1
source span
compiler-generated vs source-declared
static data initializer
mutability/readonly-data status
```

Source variables use zero static data plus ordered module-init stores as defined
above. Compiler-generated literal data may carry arbitrary constant static data.

Lexical source name is diagnostic/debug metadata, not backend identity. Two
shadowed top-level declarations may have the same source name while retaining
separate global IDs and generated backend symbols.

## Terminators

Every block ends in exactly one of:

```text
br target(args...)
cond_br condition, true_target(args...), false_target(args...)
switch value, cases..., default_target(args...)
ret
ret value
trap reason
unreachable
```

`switch` case keys are typed constants. A lowering pass always supplies a default
edge, including the implicit no-match continuation for a non-exhaustive source
switch.

`trap` is an observable abnormal termination used when an explicit lowered
check needs its own control-flow form. Checked MIR operations may retain implicit
trap semantics until a later legalization pass expands them.

`unreachable` is only for paths proven impossible by the language/IR contract;
it must not be used merely because a backend would prefer undefined behavior.

## AST/semantic lowering map

The initial lowerer should follow this conceptual mapping:

| Frontend construct | CogIR lowering |
| --- | --- |
| number/bool/null/enum constant | typed constant |
| constant identifier | referenced/inlined typed constant |
| variable identifier read | address + `load` |
| function identifier | `function_ref` |
| string array initializer | decoded array constant/value |
| C string binding | readonly generated data global + address |
| unary checked integer `-` | `ineg.checked` |
| floating `-` | `fneg` |
| `!` | Boolean not/value comparison |
| `&x` | lower place and return address |
| `*p` | pointer value becomes place address |
| arithmetic | checked integer or floating operation |
| wrapping builtin | wrapping IR operation |
| bitwise/shift | fixed-width bit/checked-count operation |
| `&&` / `||` | CFG short-circuit + block parameter |
| assignment | lower place + `store` |
| compound assignment | place + `load` + op + `store` |
| `++` / `--` | place + `load` + checked +/- 1 + `store` |
| local variable | function slot + optional initializer store |
| global variable | module global + source-order init store |
| struct literal | `make_struct` |
| array literal | `make_array` |
| field rvalue | `extract_field` or load from `field_addr` |
| field lvalue | `field_addr` |
| array lvalue index | `array_elem_addr` |
| pointer index | `ptr_index_addr` |
| checked cast | `cast.checked` or constant |
| `truncate` | `int.truncate` or constant |
| `reinterpret` | `ptr.reinterpret` |
| call | `function_ref`/callee value + `call` |
| block | sequential CFG lowering |
| `if` | conditional branches + join block |
| `while` | condition/body/exit blocks |
| `for` | condition/body/post/exit blocks |
| `break` / `continue` | branch to active loop target |
| `switch` | `switch` terminator + case/default/join blocks |
| `return` | `ret` terminator |
| type/enum/const declaration | type/constant table only |
| nested function | module-level internal function |

Semantic analysis remains authoritative for legality. The lowerer reports an IR
lowering error when required successful semantic metadata is missing; it does
not recover by performing fresh name/type reasoning.

## Verifier

`cog_ir_verify()` should exist before the host-C backend is ported. It should
check at least:

### Module/table integrity

- every ID is in range and every invalid sentinel is used legally;
- nominal type IDs are unique and definitions are internally consistent;
- no frontend-only/unresolved type exists;
- all source spans refer to copied source files or are explicitly synthetic;
- external declarations have no body and definitions have the required body;
- ABI metadata matches the associated runtime function/aggregate shape;
- `module.init_function`, when present, is an internal `() -> void` definition.

### Constants/globals

- constant payload kind matches its concrete type;
- integer/enum bits fit the declared width;
- aggregate constants have exactly the required element/field types;
- static global initializer type matches the global stored type;
- source globals obey the v1 initialization representation contract.

### Functions/CFG

- block/value/slot references belong to the correct function;
- every block has exactly one terminator;
- branch argument count/types match target block parameters;
- conditions are `bool`;
- switch keys match the switched value type and are unique;
- return value matches the function return type;
- void instructions do not manufacture value IDs;
- instruction values are defined before legal use according to the block/value
  discipline;
- local/global addresses have the expected pointer type.

### Operations/memory

- arithmetic operands/results have valid matching concrete types;
- signed/unsigned comparison and right-shift variants match operand signedness;
- checked/wrapping operations are used only for supported integer types;
- loads/stores match pointee types;
- stores never use readonly addresses;
- volatile semantic accesses are represented as volatile memory operations;
- field indexes belong to the referenced nominal aggregate;
- array-element addressing references the correct fixed array type;
- pointer indexing uses a typed raw pointer rather than an opaque pointer;
- calls match fixed signature arguments and result type;
- conversion operations are valid for their source/destination type families.

Verifier errors use the shared diagnostic infrastructure with
`DIAGNOSTIC_PHASE_IR`.

## Textual dump

A deterministic textual dumper should be implemented with the first IR builder.
It is a debug/test format, not initially a stable serialized file format.

Example:

```text
module target(pointer=64)

global @g0 "counter" : i32 = zeroinit

func @f0 "bump"(%arg0: i32) -> i32 [abi=coglet, linkage=internal] {
slots:
    $s0 : i32 "x"

entry:
    %0 = local_addr $s0
    store %0, %arg0
    %1 = load %0
    %2 = const i32 1
    %3 = iadd.checked %1, %2
    ret %3
}

init @f1 {
entry:
    %0 = global_addr @g0
    %1 = const i32 1
    store %0, %1
    ret
}
```

The exact spelling may evolve while CogIR is new; tests should canonicalize IDs
by deterministic construction order rather than pointer addresses.

## Initial C data-structure sketch

The implementation should remain arena-backed like the current compiler, but
use IR-owned arena storage.

```c
typedef struct CogIrModule {
    Arena *arena;
    TargetInfo target;

    CogIrSourceTable sources;

    CogIrType *types;
    size_t type_count;

    CogIrAbiType *abi_types;
    size_t abi_type_count;

    CogIrConstant *constants;
    size_t constant_count;

    CogIrGlobal *globals;
    size_t global_count;

    CogIrFunction *functions;
    size_t function_count;

    CogIrFunctionId init_function;
} CogIrModule;

typedef struct CogIrFunction {
    CogIrFunctionId id;
    StringView debug_name;
    SourceSpan span;

    CogIrTypeId type;
    CogIrFunctionKind kind;       /* declaration/definition */
    CogIrLinkage linkage;

    CogIrValue *parameters;
    size_t parameter_count;

    CogIrSlot *slots;
    size_t slot_count;

    CogIrBlock *blocks;
    size_t block_count;
    CogIrBlockId entry_block;

    CogIrFunctionAbi abi;
} CogIrFunction;

typedef struct CogIrBlock {
    CogIrBlockId id;
    StringView debug_name;

    CogIrBlockParam *parameters;
    size_t parameter_count;

    CogIrInstruction *instructions;
    size_t instruction_count;

    CogIrTerminator terminator;
} CogIrBlock;
```

The first implementation can use arena-grown arrays/builders internally; the
public backend-facing representation should expose stable IDs and immutable
finished tables rather than frontend pointers.

## Non-goals for CogIR v1

Do not block the first IR implementation on:

- LLVM-specific types/instructions;
- machine registers;
- instruction selection;
- register allocation;
- object-file emission;
- target register classes;
- optimization passes;
- full SSA promotion;
- persistent binary IR serialization;
- module/import dependency ordering;
- runtime reflection/metaprogramming;
- new bounds checks or other language semantics not already specified.

CogIR v1 is successful when the complete currently-supported frontend can lower
to a verified module and execution backends consume that module without consulting
AST or semantic objects. The host-C bootstrap backend now satisfies that boundary;
LLVM/native lowering can build on the same contract.

## Implementation status

The first CogIR core milestone now provides:

- `include/cog_ir.h` with stable IDs and module/type/ABI/constant/global/function/CFG structures;
- an IR-owned arena-backed builder in `src/cog_ir.c`;
- structural interning for non-nominal runtime/ABI types and declare-then-define nominal types;
- distinct states for defined aggregates and intentionally incomplete `#repr(c)` aggregates;
- exact integer/IEEE floating constant payloads and aggregate constants;
- function-local value, slot, block, instruction, and terminator containers;
- explicit optional-result emission (`cog_ir_emit`) so void instructions cannot be confused with builder failure;
- a freeze boundary that rejects builder mutation after construction;
- `cog_ir_verify()` structural/type/CFG checks using IR-phase diagnostics;
- deterministic `cog_ir_dump()` output;
- a standalone core regression that constructs and verifies CogIR without AST/semantic objects.

The IR core remains independently constructible without frontend objects. The
source-program `dump_ir` tool now exercises the real frontend lowering path and
verifies the frozen IR again after `CompileResult` has been destroyed.

### Frontend metadata lowering

`cog_ir_lower_prepare_metadata()` establishes the first frontend -> CogIR
boundary without translating executable statements. It copies all registered
source files into IR-owned storage, predeclares recursive nominal types, lowers
resolved runtime/ABI types, maps `SemDeclId` values to CogIR module identities,
materializes checked constants and enum members, creates source globals with zero
static initialization, and predeclares functions with stable `CogIrFunctionId`
values. Function-local declarations remain explicitly pending until CFG lowering.

All functions are initially added as declarations so mutually-recursive calls can
resolve stable IDs before bodies are translated. Internal declarations can make
the one-way `cog_ir_begin_function_definition()` transition before slots/blocks
are added. The metadata-only module is verifier-valid, and regression coverage
destroys `CompileResult` and verifies the module again to prove there is no
frontend-lifetime dependency.

### Executable and structured-CFG lowering

`cog_ir_lower_executable()` now lowers straight-line execution and structured
control flow after metadata preparation. Internal functions transition from their
predeclared identity to definitions, parameters receive addressable slots, and
local declarations receive slots at their source declaration. The lowering
supports concrete/checked constants, local/parameter/global/function identifiers,
identifier assignment, arithmetic compound assignment, increment/decrement,
returns, unary numeric negation/Boolean not, checked integer `+ - * / %`, floating
`+ - * /`, signed/unsigned/float/pointer/function-pointer comparisons, and typed
calls.

`if`/`else`, `while`, `for`, `break`, `continue`, and `switch` lower to explicit
basic blocks and terminators. `&&` and `||` use short-circuit CFGs with a Boolean
block parameter at the join rather than eager binary instructions. `for` continue
edges target the post block, while nested loop contexts retain independent break
and continue targets. Compile-time-true loops with no reachable break terminate
their synthetic exit block with `unreachable`, matching the frontend's non-
continuation analysis. Exhaustive Boolean/enum switches likewise route their
impossible unmatched edge to `unreachable`; non-exhaustive switches retain an
implicit path to the merge block.

Function-local instruction values are deliberately block-local in CogIR v1. If
evaluating a later subexpression can introduce CFG (notably short-circuit Boolean
arguments), lowering spills values that must survive the split to compiler-
generated slots and reloads them in the continuation block. This preserves source
evaluation order without weakening the verifier into assuming unproven dominance.
Pointer equality across immediate readonly/volatile qualifier differences emits a
`ptr.qualify` operation to a common qualified pointer type before comparison.

Source globals remain statically zero-initialized. Their explicit initializers and
top-level runtime statements, including structured control flow, are emitted in
source order into the synthetic internal `__coglet_module_init` function. This is
the implementation of the module-initialization model specified above rather than
relying on generated-C initialization behavior.

The lowering context records a strict two-stage state (`metadata_prepared`, then
`executable_lowered`) so a module cannot accidentally be lowered twice. Local and
parameter declaration bindings transition from pending identities to concrete
function/slot mappings. No lexical lookup is performed during this stage: every
identifier is recovered through semantic `SemDeclId` information.

### Data, address, aggregate, and cast lowering

The executable lowerer now has a first-class `lower_place()` path rather than
special-casing identifier mutation. A lowered place carries one evaluated address,
its value type, writability, and volatility. Identifier storage, typed-pointer
dereference, fixed-array indexing, typed-pointer indexing, and struct/union fields
all use that path. Assignment, compound assignment, and increment/decrement
therefore evaluate complex mutation targets exactly once before loading/storing.
This is especially important for indexed read-modify-write expressions whose
index may contain calls or short-circuit control flow.

Aggregate rvalues now lower through `make_array` and `make_struct`, while aggregate
assignment, by-value arguments, and returns use the ordinary typed load/store/call
machinery. Struct fields map source names to declaration field indices during
lowering; backends never perform field-name lookup. Array/struct values that must
survive a CFG-producing sibling expression reuse the existing spill/reload rule.

Address-of returns the address produced by `lower_place()`. Dereference uses the
pointer value itself as a place address. `field_addr`, `array_elem_addr`, and
`ptr_index_addr` retain readonly/volatile access in their result pointer types,
and memory operations emit explicit `.volatile` loads/stores when semantic access
requires it. The verifier checks these address/result type and qualifier
relationships.

Checked numeric casts, integer truncation, raw-pointer reinterpretation, and safe
pointer qualification now lower to their dedicated CogIR operations. Explicit
casts are also responsible for materializing frontend-only adaptable literals:
for example `cast(f32, 0.0)` and `cast(i32*, null)` directly produce concrete
CogIR constants instead of attempting to create `untyped-float` or `null` IR
values. Reinterpret verification restricts the operation to typed/opaque raw
pointer crossings and rejects qualifier loss.

Character literals lower as concrete integer values. Fixed-array string literals
lower as decoded byte-array values. The narrow direct `#extern(c)` C-string
conversion emits an IR-owned private readonly byte-array global and passes the
address of its first element; this does not introduce general array-to-pointer
decay.

The large `tests/test_assets/semantic_valid.cog` fixture is now an executable
CogIR integration target after its previously-uninitialized arrays are given
explicit initial values. A separate data/address golden test covers fields,
indexes, aggregates, pointers, volatile access, casts/reinterpretation, character
and string values, and the C-string boundary. Runtime `wrapping_add`,
`wrapping_sub`, `wrapping_mul`, and `wrapping_neg` calls
now lower to their dedicated wrapping instructions. The lowerer preserves ordinary
left-to-right evaluation and spills an earlier operand when a later wrapping
argument can introduce CFG. Compile-time wrapping calls still use semantic
constant metadata and therefore materialize as typed CogIR constants.

All 99 `semantic/valid` fixture programs now lower successfully through `dump_ir`.
A registered integration test recursively runs the entire fixture directory so
future frontend-valid additions must also cross the CogIR boundary. Native-C
variadic tails are now legalized in CogIR with explicit `c.vararg.promote`
instructions, and the verifier rejects unpromoted tails.

The host-C parity audit's final source-level policy fact is now retained by
CogIR. Ordinary Coglet functions carry `source_return_c_scalar_kind` when their
return type was spelled with a native-C scalar alias. This is descriptive source
ABI metadata, not a second runtime type: `main::() -> c_int` and
`main::() -> i32` still both execute with the same canonical integer type, while
the former dumps with `source-return=c_int`. Native-C ABI functions do not
duplicate this marker because their complete C-facing return spelling is already
stored in `CogIrFunctionAbi.return_abi_type`. The marker survives frontend
destruction and is sufficient to preserve the current host executable entry
contract in the CogIR-only host-C backend.

All data presently consumed from normalized C ABI declarations, represented
aggregates/enums, function/callback types, calling conventions, external symbols,
pointer qualifiers, executable expressions, and the executable entry policy now
has a CogIR-owned representation. `backend_c.h` accepts `const CogIrModule *` only,
and the CLI freezes/verifies the module, destroys `CompileResult`, verifies the
frozen module again, and only then invokes C emission. The current C execution
emitter supports the straight-line operations exercised by the executable/interop
suite and lowers `iadd.wrap`, `isub.wrap`, `imul.wrap`, and `ineg.wrap` with
explicit fixed-width bit-pattern semantics. Checked integer add/subtract/multiply,
division/remainder, and signed negation emit explicit guards before the C operation
and call `abort()` on the CogIR trap path. Reachable multi-block CFGs now emit as
labels/gotos with parallel block-parameter edge transfer, `br`, `cond_br`,
`switch`, `trap`, and `unreachable`; integer comparisons and Boolean negation
provide the scalar predicates required by that slice. Scalar globals are emitted
with their CogIR static initializer, so ordered runtime global/top-level execution
through the synthetic module initializer works through host-C as well. Aggregate
globals and the remaining non-CFG instruction families are still separate backend
coverage work.

## Implementation sequence

1. ~~Add `include/cog_ir.h` with IDs, module/type/constant/function/CFG structures.~~
2. ~~Add an arena-backed builder in `src/cog_ir.c`.~~
3. ~~Add deterministic `cog_ir_dump()` and the `dump_ir` source-program tool.~~
4. ~~Add `cog_ir_verify()` and verifier unit tests.~~
5. ~~Add semantic-to-IR type/declaration/source/constant maps.~~
   Metadata preparation now owns source provenance, nominal/function
   predeclaration, ABI/type mapping, constants, and zeroed global storage.
6. ~~Lower constants, globals/module init, slots, returns, basic arithmetic, and
   simple calls first.~~ Comparisons join the control-flow slice next.
7. ~~Add branches/loops/switches and short-circuit Boolean CFG lowering.~~
   Structured CFG lowering now includes comparisons, block-parameter short
   circuiting, loop targets, exhaustive switch handling, and CFG-safe value spills.
8. ~~Add pointers, arrays, structs, volatile memory, casts, and string/data lowering.~~
   Places now cover identifiers, fields, indexes, and dereferences; aggregate values,
   address-of, casts/reinterpretation, volatile accesses, and string/character values
   are lowered and verifier-checked.
9. ~~Make successful lowering independent: destroy `CompileResult` before invoking
   IR-only test consumers.~~ `dump_ir` verifies and dumps after frontend destruction.
10. ~~Lower explicit wrapping builtins to dedicated wrapping operations.~~ Runtime
    wrapping calls now emit `iadd.wrap`, `isub.wrap`, `imul.wrap`, or `ineg.wrap`,
    while constant calls retain the existing constant-materialization path.
11. ~~Audit and legalize ABI-specific calls, including C variadic default promotions.~~
    C variadic tails now carry explicit non-trapping promotion operations and
    verifier enforcement.
12. ~~Resolve the `main::() -> c_int` entry-point parity decision.~~ Ordinary
    Coglet functions now retain native-C scalar return spelling in IR-owned
    metadata, preserving the existing entry contract without retaining frontend
    type objects.
13. ~~Port the host-C backend to `const CogIrModule *` and restore all backend tests
    through AST -> semantic -> CogIR -> C.~~ The public backend API is IR-only,
    frontend lifetime ends before backend invocation, and the existing
    executable/interop suite runs through CogIR.
14. ~~Expand the host-C backend through the core structured-CFG execution slice.~~
    Wrapping and checked integer arithmetic, integer predicates, scalar globals and
    ordered module initialization now execute through reachable block labels/gotos
    with parallel block-parameter transfer, branches, switches, traps, and
    unreachable terminators. Remaining data/cast instruction families and aggregate
    globals can be filled in independently while LLVM lowering begins on the same
    verified CogIR contract.
