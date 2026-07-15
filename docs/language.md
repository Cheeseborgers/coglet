# Coglet Language Notes

Coglet is a modern systems language focused on explicit semantics, predictable behavior, and a compiler architecture that can grow without accumulating technical debt.

This document records user-visible language behavior for currently supported and near-term features.

## Arrays

Arrays are fixed-size values with an element type and compile-time length.

```c
values: i32[3];
```

The type above means: an array of three `i32` values.

Array elements may be accessed by index:

```c
values[0] = 1;
values[1] += 2;
```

Array bounds are part of the type.

```c
a: i32[3];
b: i32[4];
```

`a` and `b` have different types.

## Array Literals

Array literals initialize fixed-size arrays.

```c
values: i32[3] = [1, 2, 3];
```

The destination type supplies the element type and expected length.

Valid:

```c
values: i32[3] = [1, 2, 3];
```

Invalid:

```c
values: i32[3] = [1, 2];       // too few elements
values: i32[3] = [1, 2, 3, 4]; // too many elements
values: i32[3] = [1, true, 3]; // wrong element type
```

Array literals are currently contextual initializers. They are not yet fully inferred standalone expressions.

Rejected for now:

```c
values := [1, 2, 3];
foo([1, 2, 3]);
```

These forms require expected-type propagation or array literal inference and should be implemented later.

## String Literals

String literals represent immutable compile-time byte data.

In the first implementation stage, string literals are only valid as initializers for fixed-size byte arrays.

```c
name: u8[6] = "hello";
```

The literal `"hello"` contains five visible bytes plus one trailing null byte:

```text
h e l l o \0
```

Therefore, the destination array must have length 6.

## String Literal Rules

Valid:

```c
name: u8[6] = "hello";
empty: u8[1] = "";
```

Invalid:

```c
name: u8[5] = "hello";   // missing space for trailing null byte
name: i32[6] = "hello";  // destination is not a byte array
name := "hello";         // string literal inference not supported yet
print("hello");          // expected-type propagation for call arguments not supported yet
```
String escape validation is shared by semantic analysis and future code generation so that string size checking and emitted bytes follow the same rules.

## String Mutability

String literals themselves are immutable.

When a string literal initializes a mutable array, the array receives its own initialized storage.

```c
name: u8[6] = "hello";
name[0] = 'H';
```

This mutates the array, not the original string literal.

This distinction is important for future code generation. String literal storage may later live in readonly static memory, while array initialization creates separate mutable storage when required.

## Future String Direction


The first string milestone intentionally does not introduce a final high-level string type.

Future stages may add:

- readonly slices
- mutable slices
- string literal coercion to slices
- a first-class `string` type
- UTF-8 validation or UTF-8-by-convention rules
- C interop through null-terminated byte data

These should be added only after their type, mutability, and ownership behavior is clearly specified.
