# Coglet Language Notes

Coglet is a modern systems language focused on explicit semantics, predictable behavior, and a compiler architecture that can grow without accumulating technical debt.

This document records user-visible language behavior for currently supported and near-term features.

## Values, Storage, and Assignability

Coglet distinguishes between expressions that produce values and expressions that denote assignable storage.

An expression is assignable only when semantic analysis determines that it is an lvalue.

Variables are assignable:

```c
x: i32 = 1;
x = 2;
```

Fields and indexes are assignable only when their base expression is assignable:

```c
point.x = 1;
values[0] = 10;
```

The following are not assignable:

```c
CONSTANT = 1;          // constants are not assignable
Color.Red = Color.Blue; // enum members are not assignable
make_point().x = 1;   // field of temporary value is not assignable
make_array()[0] = 1;  // index of temporary value is not assignable
```

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

Array literals are currently contextual initializers. They require an expected array type from the surrounding context.

Currently supported expected-type contexts include:

```c
values: i32[3] = [1, 2, 3];       // variable declaration
values = [4, 5, 6];               // assignment

takes_i32_array([1, 2, 3]);       // function-call argument

make_values::() -> i32[3] {
    return [1, 2, 3];             // return value
}

Point :: struct {
    values: i32[3];
}

p := Point {
    values = [1, 2, 3],           // struct field initializer
};
```

Array literals are not yet fully inferred standalone expressions.

Rejected for now:

```c
values := [1, 2, 3];
[1, 2, 3];
```

## String Literals

String literals represent immutable compile-time byte data.

In the current implementation stage, string literals are contextual initializers for fixed-size byte arrays.

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

name = "hello";

takes_name("hello");

make_name::() -> u8[6] {
    return "hello";
}
```

Invalid:

```c
name: u8[5] = "hello";   // missing space for trailing null byte
name: i32[6] = "hello";  // destination is not a byte array
name := "hello";         // string literal inference not supported yet
"hello";                 // bare string literals are not standalone expressions yet
```

String literals require an expected destination type. The destination must be a fixed-size `u8` array with the exact decoded byte length plus the trailing null byte.

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

```c
// "abc" has 4 initialized bytes in u8[4]:
// 'a', 'b', 'c', '\0'
```

## Future String Direction

The first string milestone intentionally does not introduce a final high-level string type.

Future stages may add:

* readonly slices
* mutable slices
* string literal coercion to slices
* a first-class `string` type
* UTF-8 validation or UTF-8-by-convention rules
* C interop through null-terminated byte data

These should be added only after their type, mutability, and ownership behavior is clearly specified.
