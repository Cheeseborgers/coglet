# Coglet Roadmap

Coglet is currently focused on building a small, correct, modern systems-language core with a clean compiler architecture.

The priority is correctness over convenience. Features should be added in small, well-defined stages so the compiler can grow without accumulating technical debt or special-case spaghetti.

## Current Focus: Core Language Semantics

Recently completed or in progress:

- Parser support for expressions, functions, structs, enums, switch, casts, break/continue, compound assignment, and array literals
- Semantic support for scope resolution, type checking, constants, structs, enums, switch exhaustiveness, return-path analysis, constant array bounds, cast validation, break/continue, compound assignment, and array initializer checking
- `%=` support as an integer-only compound assignment operator
- Array literals as contextual initializers for fixed-size arrays

## Next Milestone: String Literal Semantics

String support will be implemented in stages.

The first stage should avoid committing too early to a final high-level string type. Instead, string literals will initially work as contextual initializers for fixed-size byte arrays.

### Stage 1: String Literals as Fixed-Size Byte Array Initializers

String literals may initialize fixed-size `u8` arrays.

```c
name: u8[6] = "hello";
```

The literal includes a trailing null byte for C interop, so `"hello"` requires six bytes:

```text
h e l l o \0
```

Rules:

- String literals represent immutable compile-time byte data.
- Assigning a string literal to a mutable array creates initialized array storage.
- The destination type must be a fixed-size byte array.
- The destination array length must match the string byte length plus the trailing null byte.
- String literals are not yet general expressions.

Valid:

```c
name: u8[6] = "hello";
empty: u8[1] = "";
```

Rejected for now:

```c
name: u8[5] = "hello";   // missing space for trailing null byte
name: i32[6] = "hello";  // destination is not a byte array
name := "hello";         // string literal inference is not supported yet
print("hello");          // function argument expected-type propagation is not supported yet
```

### Stage 2: Expected-Type Propagation

Once string literals and array literals work as declaration initializers, extend semantic analysis so expected types can be passed into other contexts.

Examples that should become possible later:

```c
print::(message: u8[6]) -> void {
}

main::() -> void {
    print("hello");
}
```

This same expected-type mechanism should also improve array literals in function arguments, returns, casts, and other contexts.

### Stage 3: Slices

Add slice types after fixed-size arrays and string literal initialization are stable.

Possible future syntax:

```c
name: []u8 = "hello";
```

The exact syntax and mutability model should be decided before implementation.

Open design questions:

- Should slices distinguish mutable and readonly data?
- Should string literals coerce only to readonly slices?
- Should string literals remain null-terminated when converted to slices?
- Should slices carry length only, or length plus capacity?

### Stage 4: First-Class String Type

A later stage may introduce a first-class `string` type.

Possible future syntax:

```c
name: string = "hello";
```

This should be considered only after array, slice, mutability, and ownership rules are better defined.

Open design questions:

- Is `string` a builtin type or standard-library type?
- Is `string` always UTF-8?
- Is `string` nullable, slice-like, or owned?
- Does `string` include a trailing null byte?
- How does `string` interoperate with C APIs?

## Later Language Work

After strings and slices, larger language features can be tackled:

- Imports
- Modules
- Multi-file compilation
- Generics
- More complete code generation
- Standard library design

Imports and modules should wait until the core type system and semantic model are stable, because they affect symbol visibility, file loading, compilation units, build layout, and package boundaries.
