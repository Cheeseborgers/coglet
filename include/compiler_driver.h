#ifndef COGLET_COMPILER_DRIVER_H
#define COGLET_COMPILER_DRIVER_H

#include "ast.h"
#include "parser.h"
#include "semantic_anal.h"
#include "target_info.h"
#include "utils/arena.h"

typedef enum CompileStatus {
    COMPILE_STATUS_OK = 0,
    COMPILE_STATUS_SEMANTIC_ERROR,
    COMPILE_STATUS_PARSE_ERROR,
    COMPILE_STATUS_DRIVER_ERROR,
} CompileStatus;

typedef struct CompileOptions {
    /*
     * When non-zero, unresolved imports may load a canonical source before
     * semantic analysis. Dotted module names map to path components, so
     * `import std.io;` resolves a candidate `std/io.cog`.
     */
    int discover_imports;

    /*
     * Additional module search roots, consulted after the importing file's
     * directory and in the supplied order. Paths are borrowed for the call.
     */
    const char *const *module_search_dirs;
    size_t module_search_dir_count;

    /*
     * Optional fallback module root for the standard-library `std` namespace. This is
     * consulted only after importer-local and user module search roots, and
     * only for `std` or `std.*` imports. The path is borrowed for the call.
     */
    const char *stdlib_root;
} CompileOptions;

CompileOptions compile_options_default(void);

typedef struct CompileResult {
    CompileStatus status;

    const char *filename;
    char *source;

    /* Owned source buffers in explicit-then-discovered order; source aliases [0]. */
    char **source_buffers;
    size_t source_buffer_count;

    /* Multi-file-capable source provenance; filename/source identify primary. */
    SourceManager sources;
    SourceFileId primary_source_id;

    Arena *arena;
    Arena *scratch;

    Parser parser;
    Node *program;

    TargetInfo target;
    SemanticContext sem;
} CompileResult;

/*
 * Initializes out and runs the source-file parse/check pipeline.
 *
 * Ownership after return:
 *   - out->filename is borrowed
 *   - out->source is owned by out
 *   - out->arena and out->scratch are owned by out
 *   - out->target is the copied frontend target description
 *   - out->sources owns source metadata and may register multiple source files
 *   - out->program, parser diagnostics, and semantic data remain valid until
 *     compile_result_destroy(out)
 *
 * The result may be destroyed after any returned status.
 *
 * On COMPILE_STATUS_OK:
 *   - program and semantic state are available for later compiler phases
 *
 * On COMPILE_STATUS_SEMANTIC_ERROR:
 *   - program is available
 *   - semantic state may be partial and is intended only for diagnostics or
 *     debugging tools
 *
 * On COMPILE_STATUS_PARSE_ERROR:
 *   - parser state and a partial program may be available
 *   - semantic analysis has not run
 *
 * Diagnostics:
 *   - parser and semantic diagnostics are collected with SourceSpan provenance
 *   - this function prints diagnostics followed by the phase error summary
 */
CompileStatus compile_parse_and_check(const char *filename, CompileResult *out);

/*
 * Parses and checks one compilation unit assembled from explicit source files.
 * These compatibility entry points do not perform filesystem import discovery;
 * use the options-aware forms when discovery is desired. `filenames` is
 * borrowed; source contents are owned by `out` until compile_result_destroy().
 */
CompileStatus compile_parse_and_check_files(
    const char *const *filenames,
    size_t filename_count,
    CompileResult *out
);

/*
 * Options-aware host-target compilation. When discover_imports is enabled,
 * missing imports may add canonical module sources before semantic analysis;
 * dotted names map to path components (`std.io` -> `std/io.cog`). The optional
 * stdlib root is a final fallback for the standard-library `std` namespace only.
 */
CompileStatus compile_parse_and_check_files_with_options(
    const char *const *filenames,
    size_t filename_count,
    const CompileOptions *options,
    CompileResult *out
);

/*
 * Same frontend pipeline using an explicit target description. The target is
 * copied into the result; the caller does not need to keep it alive.
 */
CompileStatus compile_parse_and_check_for_target(
    const char *filename,
    const TargetInfo *target,
    CompileResult *out
);

CompileStatus compile_parse_and_check_files_for_target(
    const char *const *filenames,
    size_t filename_count,
    const TargetInfo *target,
    CompileResult *out
);

CompileStatus compile_parse_and_check_files_for_target_with_options(
    const char *const *filenames,
    size_t filename_count,
    const TargetInfo *target,
    const CompileOptions *options,
    CompileResult *out
);

/*
 * Releases all resources owned by result.
 *
 * Safe to call after any normal return from compile_parse_and_check().
 * After this call, all AST, parser, and semantic pointers from result are
 * invalid.
 */
void compile_result_destroy(CompileResult *result);

int status_to_exit_code(CompileStatus status);

#endif
