# Self-Hosting Requirements

Self-hosting remains a long-term objective rather than the current milestone.

Coglet now has a compiler-owned typed CFG IR (CogIR) and a host-C bootstrap backend that executes the current tested CogIR surface, including structured control flow, storage, aggregates, checked/wrapping arithmetic, pointers/casts, strings, and C interoperability. The host-C path is an execution bootstrap, not a commitment to C as the permanent backend architecture.

## Frontend Features Already Present

The semantic frontend now provides:

* structs
* closed enums
* fixed-size arrays
* raw pointer types
* function calls
* nested functions (without closure capture)
* `if`, `while`, `for`, `switch`, `break`, `continue`, and `return`
* integer, floating-point, boolean, pointer, and enum type checking
* explicit casts with compile-time validation
* compile-time constants and exact integer evaluation
* fixed byte-array string literals
* lexical scopes and nominal symbol resolution
* local and parameter definite-assignment analysis
* unified reachability analysis
* value-based switch exhaustiveness
* unreachable-statement diagnostics
* semantic expression-information verification

These analyses provide the semantic foundation required before lowering and backend implementation.

## Still Required for Practical Self-Hosting

The remaining work is primarily backend and language evolution rather than fundamental semantic analysis.

Likely requirements include:

* LLVM/native target code generation beyond the host-C bootstrap path
* explicit target ABI lowering for interoperability surfaces that C currently classifies for the compiler
* basic runtime calls and file I/O
* arena or general allocation facilities available to Coglet programs
* multi-file compilation or modules
* imports, declaration visibility, or an equivalent compilation-unit model
* a stable runtime / standard-library boundary
* stable diagnostics for large projects

## Future Language Work

These features remain intentionally deferred until real use cases justify them:

* closure environments and variable capture
* first-class owned strings
* slices and views
* generics
* modules/packages
* a package manager
* interfaces or traits
* operator overloading
* advanced compile-time execution
* optimization-focused native code generation

## Current Frontend Status

The frontend now performs:

* parsing
* symbol resolution
* nominal type checking
* compile-time constant evaluation
* semantic expression side-table generation
* definite-assignment analysis
* unified reachability analysis
* switch exhaustiveness analysis

At this stage, the remaining path toward self-hosting is primarily native backend, runtime/library, and compilation-unit engineering rather than another major semantic-analysis subsystem. The host-C backend already proves the current frontend -> CogIR -> backend execution path; major remaining gaps include multi-file/module support, allocation and I/O facilities, and a non-C target-code path suitable for an eventual self-hosted compiler.
