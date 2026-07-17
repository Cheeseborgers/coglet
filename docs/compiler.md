# Compiler driver

The compiler driver provides the shared source-file frontend pipeline used by
`coglet` and semantic test tools.

Its current responsibilities are:

1. read one source file;
2. create the main and scratch arenas;
3. initialize the parser;
4. parse the program;
5. report parser diagnostics;
6. run semantic analysis;
7. report the semantic error summary;
8. retain frontend state for the caller until explicit destruction.

The driver does not perform token or AST snapshot dumping. `dump_tokens` remains
lexer-only and `dump_ast` remains parser-only.

## Ownership

`CompileResult` owns:

- the source buffer;
- the main arena;
- the scratch arena.

`CompileResult` borrows:

- the filename string.

The following data is backed by the owned arenas:

- AST nodes;
- parser diagnostics and diagnostic messages;
- semantic symbols and scopes;
- semantic types;
- expression side-table information.

The parser and semantic contexts are embedded in `CompileResult`, but they do
not independently own their arena-backed data.

All frontend state remains valid until:

```c
compile_result_destroy(&result);
```

After destruction, the source, AST, parser diagnostics, and semantic data must
not be accessed.

Failure states

A result may be destroyed after every driver return status.

* COMPILE_STATUS_OK: parsing and semantic analysis succeeded.
* COMPILE_STATUS_SEMANTIC_ERROR: parsing succeeded; semantic state may be
partial.
* COMPILE_STATUS_PARSE_ERROR: parsing failed; semantic analysis was not run.
* COMPILE_STATUS_DRIVER_ERROR: the frontend pipeline could not be started,
such as when the source file could not be read.

Parser and driver errors map to process exit code 2. Semantic errors map to
exit code 1.

Diagnostics

Parser diagnostics are accumulated by the parser and printed by the driver
after parsing fails.

Semantic diagnostics are printed immediately during semantic analysis. The
driver prints only the final semantic error-count summary.

Callers must not print these diagnostics again.

