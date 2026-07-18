# Self-Hosting Requirements

Self-hosting remains a long-term objective, not the current milestone. Backend and runtime choices are intentionally deferred until Coglet's intended use is clearer.

## Frontend Features Already Present

- structs
- closed enums
- fixed arrays
- pointers as types
- function calls
- `if`, `while`, `for`, and `switch`
- integer, floating-point, and boolean operations
- explicit casts with compile-time validation
- compile-time constants and exact integer evaluation
- fixed byte-array string literals
- lexical scopes, diagnostics, and return analysis
- semantic-information verification

## Still Required for Practical Self-Hosting

- address-of and dereference expressions
- a settled pointer and memory-safety model
- an execution strategy: interpreter, transpiler, or native backend
- basic runtime calls and file I/O
- arena or general allocation facilities available to Coglet programs
- multi-file compilation or a temporary include mechanism
- imports, declaration visibility, or an equivalent compilation-unit model
- a usable runtime/standard-library boundary
- stable diagnostics for larger programs

## Explicitly Not Required Initially

- first-class owned strings
- generics
- a full package manager
- closures
- interfaces or traits
- operator overloading
- advanced compile-time execution
- optimization-focused native code generation
