# Self-Hosting Requirements

Self-hosting remains a long-term objective rather than the current milestone.

The frontend is intentionally being completed before committing to a backend architecture. Lowering, code generation, runtime design, and ABI decisions remain deferred until Coglet's long-term direction is settled.

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

* a settled pointer and memory model
* runtime integer arithmetic semantics
* C interoperability design
* an execution strategy (interpreter, transpiler, bytecode, or native backend)
* lowering from the semantic AST into an implementation-oriented intermediate representation
* backend code generation
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

At this stage, the remaining path toward self-hosting is primarily a backend engineering effort rather than completing another major semantic-analysis subsystem.
